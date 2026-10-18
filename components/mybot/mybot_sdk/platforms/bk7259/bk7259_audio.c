/* SPDX-License-Identifier: Apache-2.0 */
#include "bk7259_ops.h"

#include <api/aosl_mm.h>
#include <api/aosl_thread.h>
#include <common/bk_err.h>
#include <components/bk_audio/audio_algorithms/aec_v3_algorithm_v2.h>
#include <components/bk_audio/audio_pipeline/audio_element.h>
#include <components/bk_audio/audio_pipeline/audio_pipeline.h>
#include <components/bk_audio/audio_pipeline/audio_port.h>
#include <components/bk_audio/audio_streams/onboard_mic_stream_v2.h>
#include <components/bk_audio/audio_streams/onboard_speaker_stream_v2.h>
#include <components/bk_audio/audio_streams/raw_stream.h>
#include "bk7259_platform_log.h"
#include <components/system.h>
#include <modules/pm.h>
#include <modules/wifi.h>
#include <modules/wifi_types.h>

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

#define MYBOT_AUDIO_RATE 16000
#define MYBOT_AUDIO_CHANNELS 1
#define MYBOT_CAPTURE_ADC_CHANNELS 3
#define MYBOT_AUDIO_BITS 16
#define MYBOT_AUDIO_BLOCK_BYTES 640U
#define MYBOT_AUDIO_POOL_BYTES (MYBOT_AUDIO_BLOCK_BYTES * 6U)
#define MYBOT_AUDIO_IO_TIMEOUT_MS 25
#define MYBOT_CAPTURE_DEVICE_BLOCKS 2
#define MYBOT_PLAYBACK_RAW_BLOCKS 8
#define TAG "mybot_audio"

typedef struct {
    aosl_lock_t lock;
    aosl_cond_t idle;
    unsigned int in_flight;
    bool started;
    bool stopping;
} io_state_t;

typedef struct {
    io_state_t io;
    audio_pipeline_handle_t pipeline;
    audio_element_handle_t raw;
    audio_element_handle_t device;
    audio_element_handle_t aec;
    audio_port_handle_t raw_port;
    bool raw_registered;
    bool device_registered;
    bool aec_registered;
    bool pm_acquired;
    bool playback_owner_reserved;
    bool failed;
} audio_context_t;

static aosl_static_lock_t s_audio_pm_lock = AOSL_STATIC_LOCK_INIT;
static unsigned int s_audio_pm_users;

/* onboard_speaker_stream_v2 keeps a process-wide singleton. Reserve it before
 * calling its init function so a second playback context cannot overwrite the
 * vendor global while the first context is still alive. */
static aosl_static_lock_t s_playback_owner_lock = AOSL_STATIC_LOCK_INIT;
static audio_context_t *s_playback_owner;

/* The V2 speaker stream keeps the DAC state in a process-wide element.  Keep
 * the published context behind a separate static lock so a volume callback
 * cannot race audio_context_release() and retain a freed speaker handle. */
static aosl_static_lock_t s_playback_gain_lock = AOSL_STATIC_LOCK_INIT;
static audio_context_t *s_playback_gain_context;

static bool playback_gain_context_is_usable(audio_context_t *ctx) {
    bool usable;

    if (!ctx || !ctx->device || !ctx->io.lock) {
        return false;
    }
    aosl_lock_lock(ctx->io.lock);
    usable = !ctx->io.stopping && !ctx->failed;
    aosl_lock_unlock(ctx->io.lock);
    return usable;
}

static int playback_owner_acquire(audio_context_t *ctx) {
    if (!ctx) {
        MYBOT_LOGE(TAG, "playback owner acquire: invalid context");
        return -1;
    }
    if (aosl_static_lock_lock(&s_playback_owner_lock) < 0) {
        MYBOT_LOGE(TAG, "playback owner lock failed");
        return -1;
    }
    if (s_playback_owner) {
        MYBOT_LOGW(TAG, "playback owner busy; rejecting second speaker pipeline");
        (void)aosl_static_lock_unlock(&s_playback_owner_lock);
        return -1;
    }
    s_playback_owner = ctx;
    ctx->playback_owner_reserved = true;
    (void)aosl_static_lock_unlock(&s_playback_owner_lock);
    MYBOT_LOGI(TAG, "playback owner acquired");
    return 0;
}

static void playback_owner_release(audio_context_t *ctx) {
    if (!ctx || !ctx->playback_owner_reserved) {
        return;
    }
    if (aosl_static_lock_lock(&s_playback_owner_lock) < 0) {
        MYBOT_LOGE(TAG, "playback owner release lock failed");
        return;
    }
    if (s_playback_owner == ctx) {
        s_playback_owner = NULL;
    }
    ctx->playback_owner_reserved = false;
    (void)aosl_static_lock_unlock(&s_playback_owner_lock);
    MYBOT_LOGI(TAG, "playback owner released");
}

int bk7259_audio_playback_gain_publish(void *opaque) {
    audio_context_t *ctx = opaque;

    if (!ctx || !ctx->device) {
        MYBOT_LOGW(TAG, "playback gain publish: speaker unavailable");
        return -1;
    }
    if (aosl_static_lock_lock(&s_playback_gain_lock) < 0) {
        MYBOT_LOGE(TAG, "playback gain publish lock failed");
        return -1;
    }
    if (s_playback_gain_context && s_playback_gain_context != ctx) {
        MYBOT_LOGW(TAG, "playback gain already owned by another speaker");
        (void)aosl_static_lock_unlock(&s_playback_gain_lock);
        return -1;
    }
    s_playback_gain_context = ctx;
    (void)aosl_static_lock_unlock(&s_playback_gain_lock);
    return 0;
}

void bk7259_audio_playback_gain_unpublish(void *opaque) {
    audio_context_t *ctx = opaque;

    if (!ctx || aosl_static_lock_lock(&s_playback_gain_lock) < 0) {
        if (ctx) {
            MYBOT_LOGE(TAG, "playback gain unpublish lock failed");
        }
        return;
    }
    if (s_playback_gain_context == ctx) {
        s_playback_gain_context = NULL;
    }
    (void)aosl_static_lock_unlock(&s_playback_gain_lock);
}

int bk7259_audio_playback_gain_set(float gain_db) {
    int result = -1;

    if (aosl_static_lock_lock(&s_playback_gain_lock) < 0) {
        MYBOT_LOGE(TAG, "playback gain set lock failed");
        return -1;
    }
    if (s_playback_gain_context && playback_gain_context_is_usable(s_playback_gain_context) &&
        onboard_speaker_stream_set_digital_gain(s_playback_gain_context->device, gain_db) == BK_OK) {
        result = 0;
    }
    if (result < 0) {
        MYBOT_LOGW(TAG, "playback gain set unavailable or rejected (%.2f dB)", gain_db);
    }
    (void)aosl_static_lock_unlock(&s_playback_gain_lock);
    return result;
}

int bk7259_audio_playback_gain_get(float *gain_db) {
    int result = -1;

    if (!gain_db || aosl_static_lock_lock(&s_playback_gain_lock) < 0) {
        if (gain_db) {
            MYBOT_LOGE(TAG, "playback gain get lock failed");
        }
        return -1;
    }
    if (s_playback_gain_context && playback_gain_context_is_usable(s_playback_gain_context) &&
        onboard_speaker_stream_get_digital_gain(s_playback_gain_context->device, gain_db) == BK_OK) {
        result = 0;
    }
    if (result < 0) {
        MYBOT_LOGW(TAG, "playback gain get unavailable");
    }
    (void)aosl_static_lock_unlock(&s_playback_gain_lock);
    return result;
}

static int io_state_init(io_state_t *state) {
    if (!state) {
        MYBOT_LOGE(TAG, "audio I/O state: invalid context");
        return -1;
    }
    state->lock = aosl_lock_create();
    state->idle = aosl_cond_create();
    if (!state->lock || !state->idle) {
        if (state->idle) {
            aosl_cond_destroy(state->idle);
        }
        if (state->lock) {
            aosl_lock_destroy(state->lock);
        }
        MYBOT_LOGE(TAG, "audio I/O state allocation failed");
        return -1;
    }
    return 0;
}

static void io_state_destroy(io_state_t *state) {
    aosl_cond_destroy(state->idle);
    aosl_lock_destroy(state->lock);
}

static bool io_begin(io_state_t *state) {
    aosl_lock_lock(state->lock);
    if (!state->started || state->stopping) {
        aosl_lock_unlock(state->lock);
        return false;
    }
    state->in_flight++;
    aosl_lock_unlock(state->lock);
    return true;
}

static bool io_end(io_state_t *state) {
    aosl_lock_lock(state->lock);
    state->in_flight--;
    bool stopping = state->stopping;
    if (state->stopping && state->in_flight == 0) {
        aosl_cond_broadcast(state->idle);
    }
    aosl_lock_unlock(state->lock);
    return stopping;
}

static bool io_begin_stop(io_state_t *state) {
    aosl_lock_lock(state->lock);
    while (state->stopping) {
        aosl_cond_wait(state->idle, state->lock);
    }
    if (!state->started) {
        aosl_lock_unlock(state->lock);
        return false;
    }
    state->stopping = true;
    aosl_lock_unlock(state->lock);
    return true;
}

static void io_wait_idle(io_state_t *state) {
    aosl_lock_lock(state->lock);
    while (state->in_flight != 0) {
        aosl_cond_wait(state->idle, state->lock);
    }
    aosl_lock_unlock(state->lock);
}

static void io_finish_stop(audio_context_t *ctx, bool failed) {
    aosl_lock_lock(ctx->io.lock);
    ctx->failed = ctx->failed || failed;
    ctx->io.started = false;
    ctx->io.stopping = false;
    aosl_cond_broadcast(ctx->io.idle);
    aosl_lock_unlock(ctx->io.lock);
}

/* Conversation-start power policy, mirroring the BK725x controller's
 * mybot_audio_bk725x_power_acquire(): raise the CPU to 480 MHz, keep the audio
 * subsystem out of sleep, and ask the Wi-Fi MAC for the fluent media profile.
 * The Wi-Fi media quality is deliberately not restored on release, exactly as
 * the controller leaves it. */
static int audio_pm_vote_on(void) {
    if (bk_pm_module_vote_cpu_freq(PM_DEV_ID_AUDIO, PM_CPU_FRQ_480M) != BK_OK) {
        MYBOT_LOGE(TAG, "audio CPU vote to 480 MHz failed");
        return -1;
    }
    if (bk_pm_module_vote_sleep_ctrl(PM_SLEEP_MODULE_NAME_AUDP, 0, 0) != BK_OK) {
        MYBOT_LOGE(TAG, "AUDP sleep-disable vote failed");
        if (bk_pm_module_vote_cpu_freq(PM_DEV_ID_AUDIO, PM_CPU_FRQ_DEFAULT) != BK_OK) {
            MYBOT_LOGE(TAG, "audio CPU vote rollback failed");
        }
        return -1;
    }
    if (bk_wifi_set_video_quality(WIFI_VIDEO_QUALITY_FD) != BK_OK) {
        MYBOT_LOGW(TAG, "Wi-Fi FD media-quality setting failed");
    }
    MYBOT_LOGI(TAG, "active: CPU=480 MHz, AUDP sleep=disabled, Wi-Fi quality=FD");
    return 0;
}

static int audio_pm_acquire(void) {
    int result = 0;

    if (aosl_static_lock_lock(&s_audio_pm_lock) < 0) {
        MYBOT_LOGE(TAG, "audio CPU vote lock failed");
        return -1;
    }
    if (s_audio_pm_users == 0 && audio_pm_vote_on() < 0) {
        result = -1;
    } else {
        s_audio_pm_users++;
        MYBOT_LOGI(TAG, "audio CPU vote acquired (users=%u)", s_audio_pm_users);
    }
    (void)aosl_static_lock_unlock(&s_audio_pm_lock);
    return result;
}

static void audio_pm_release(void) {
    if (aosl_static_lock_lock(&s_audio_pm_lock) < 0) {
        MYBOT_LOGE(TAG, "audio CPU vote release lock failed");
        return;
    }
    if (s_audio_pm_users != 0 && --s_audio_pm_users == 0) {
        if (bk_pm_module_vote_cpu_freq(PM_DEV_ID_AUDIO, PM_CPU_FRQ_DEFAULT) != BK_OK) {
            MYBOT_LOGE(TAG, "failed to release audio CPU frequency vote");
        } else {
            MYBOT_LOGI(TAG, "audio CPU vote released (users=0)");
        }
        if (bk_pm_module_vote_sleep_ctrl(PM_SLEEP_MODULE_NAME_AUDP, 1, 0) != BK_OK) {
            MYBOT_LOGE(TAG, "AUDP sleep-enable vote failed");
        }
    } else if (s_audio_pm_users != 0) {
        MYBOT_LOGI(TAG, "audio CPU vote retained (users=%u)", s_audio_pm_users);
    }
    (void)aosl_static_lock_unlock(&s_audio_pm_lock);
}

static bk_err_t discard_audio_event(audio_element_handle_t element,
                                    audio_event_iface_msg_t *event, void *user_data) {
    (void)element;
    (void)event;
    (void)user_data;
    return BK_OK;
}

static bool format_supported(int rate, int channels, int bits) {
    return rate == MYBOT_AUDIO_RATE && channels == MYBOT_AUDIO_CHANNELS &&
           bits == MYBOT_AUDIO_BITS;
}

static void audio_context_release(audio_context_t *ctx) {
    if (!ctx) {
        return;
    }

    MYBOT_LOGI(TAG, "audio context destroy begin");

    /* Serialize with gain set/get before the speaker element is deinitialized. */
    bk7259_audio_playback_gain_unpublish(ctx);

    if (ctx->pipeline) {
        (void)audio_pipeline_deinit(ctx->pipeline);
    }
    if (ctx->raw && !ctx->raw_registered) {
        (void)audio_element_deinit(ctx->raw);
    }
    if (ctx->device && !ctx->device_registered) {
        (void)audio_element_deinit(ctx->device);
    }
    if (ctx->aec && !ctx->aec_registered) {
        (void)audio_element_deinit(ctx->aec);
    }
    if (ctx->pm_acquired) {
        audio_pm_release();
        ctx->pm_acquired = false;
    }
    playback_owner_release(ctx);
    io_state_destroy(&ctx->io);
    aosl_free(ctx);
    MYBOT_LOGI(TAG, "audio context destroyed");
}

static int audio_context_start(audio_context_t *ctx) {
    if (!ctx || !ctx->pipeline) {
        MYBOT_LOGE(TAG, "audio pipeline start: invalid context");
        return -1;
    }

    aosl_lock_lock(ctx->io.lock);
    while (ctx->io.stopping) {
        aosl_cond_wait(ctx->io.idle, ctx->io.lock);
    }
    if (ctx->failed) {
        MYBOT_LOGW(TAG, "audio pipeline start rejected after previous failure");
        aosl_lock_unlock(ctx->io.lock);
        return -1;
    }
    if (ctx->io.started) {
        aosl_lock_unlock(ctx->io.lock);
        return 0;
    }
    if (audio_port_reset(ctx->raw_port) != BK_OK ||
        audio_pipeline_run(ctx->pipeline) != BK_OK) {
        ctx->failed = true;
        aosl_lock_unlock(ctx->io.lock);
        MYBOT_LOGE(TAG, "audio pipeline start failed");
        return -1;
    }
    ctx->io.started = true;
    aosl_lock_unlock(ctx->io.lock);
    MYBOT_LOGI(TAG, "audio pipeline started");
    return 0;
}

static int audio_context_abort_io(audio_context_t *ctx) {
    return ctx->raw_port && audio_port_abort(ctx->raw_port) == BK_OK ? 0 : -1;
}

static int audio_context_stop(audio_context_t *ctx) {
    if (!ctx || !ctx->pipeline || !ctx->raw) {
        MYBOT_LOGE(TAG, "audio pipeline stop: invalid context");
        return -1;
    }
    if (!io_begin_stop(&ctx->io)) {
        return 0;
    }

    int result = audio_context_abort_io(ctx);
    if (result < 0) {
        MYBOT_LOGE(TAG, "audio raw I/O abort failed");
    }
    io_wait_idle(&ctx->io);
    if (audio_pipeline_stop(ctx->pipeline) != BK_OK) {
        MYBOT_LOGE(TAG, "audio pipeline stop failed");
        result = -1;
    }
    if (audio_pipeline_wait_for_stop(ctx->pipeline) != BK_OK) {
        MYBOT_LOGE(TAG, "audio pipeline wait-for-stop failed");
        result = -1;
    }
    io_finish_stop(ctx, result < 0);
    if (result == 0) {
        MYBOT_LOGI(TAG, "audio pipeline stopped");
    } else {
        MYBOT_LOGE(TAG, "audio pipeline stopped with errors");
    }
    return result;
}

static int capture_init(void **out_ctx, int rate, int channels, int bits) {
    const char *stage = "validate";

    if (!out_ctx || !format_supported(rate, channels, bits)) {
        MYBOT_LOGE(TAG, "capture init rejected: format=%d Hz/%d ch/%d bit", rate, channels,
                bits);
        return -1;
    }
    *out_ctx = NULL;
    MYBOT_LOGI(TAG, "capture init: format=%d Hz/%d ch/%d bit", rate, channels, bits);

    stage = "allocate";
    audio_context_t *ctx = aosl_calloc(1, sizeof(*ctx));
    if (!ctx || io_state_init(&ctx->io) < 0) {
        MYBOT_LOGE(TAG, "capture init failed at allocate");
        aosl_free(ctx);
        return -1;
    }
    stage = "audio_cpu_vote";
    if (audio_pm_acquire() < 0) {
        MYBOT_LOGE(TAG, "capture init failed at audio CPU vote");
        audio_context_release(ctx);
        return -1;
    }
    ctx->pm_acquired = true;

    stage = "pipeline";
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    ctx->pipeline = audio_pipeline_init(&pipeline_cfg);
    if (!ctx->pipeline) {
        goto fail;
    }

    stage = "microphone";
    /* Match the new beken_robot_r2 analog capture path: all three ADCs are
     * microphone inputs, with an additional hardware playback-reference lane. */
    onboard_mic_stream_cfg_t mic_cfg = DEFAULT_ONBOARD_MIC_ADC_STREAM_CONFIG();
    mic_cfg.adc_cfg.chl_num = MYBOT_CAPTURE_ADC_CHANNELS;
    mic_cfg.adc_cfg.sample_rate = MYBOT_AUDIO_RATE;
    mic_cfg.adc_cfg.aec_en = 3;
    for (unsigned int channel = 0; channel < MYBOT_CAPTURE_ADC_CHANNELS; ++channel) {
        mic_cfg.adc_cfg.chl_cfg[channel].bits = MYBOT_AUDIO_BITS;
        mic_cfg.adc_cfg.chl_cfg[channel].dig_gain = 16.0f;
        mic_cfg.adc_cfg.chl_cfg[channel].ana_gain = 20;
        mic_cfg.adc_cfg.chl_cfg[channel].adc_mode = AUD_ADC_MODE_DIFFEN;
    }
    mic_cfg.dmic_en = 0;
    mic_cfg.frame_size = MYBOT_AUDIO_BLOCK_BYTES;
    mic_cfg.out_block_size = MYBOT_AUDIO_BLOCK_BYTES;
    mic_cfg.out_block_num = MYBOT_CAPTURE_DEVICE_BLOCKS;
    mic_cfg.multi_out_port_num = 0;
    mic_cfg.ch_bitmap = ONBOARD_MIC_ADC_ACTIVE_CH_0_BIT |
                        ONBOARD_MIC_ADC_ACTIVE_CH_1_BIT |
                        ONBOARD_MIC_ADC_ACTIVE_CH_2_BIT;
    mic_cfg.hw_ref_ch = ONBOARD_MIC_HW_REF_CH_NONE;
    ctx->device = onboard_mic_stream_init(&mic_cfg);
    if (!ctx->device ||
        audio_element_set_event_callback(ctx->device, discard_audio_event, NULL) != BK_OK) {
        goto fail;
    }

    MYBOT_LOGI(TAG, "capture geometry: R2 analog ADC0/ADC1/ADC2=MIC gain=20+16 dB, DMIC=0 ADC_aec=3 appended REF");

    stage = "aec";
    aec_v3_algorithm_cfg_t aec_cfg = DEFAULT_AEC_V3_ALGORITHM_CONFIG();
    aec_cfg.aec_cfg.mode = AEC_MODE_HARDWARE;
    aec_cfg.aec_cfg.aec_loop = AEC_HW_REF_APPEND;
    aec_cfg.aec_cfg.ref_ch = AEC_REF_CH_DEFAULT;
    aec_cfg.aec_cfg.adc_ch_num = MYBOT_CAPTURE_ADC_CHANNELS;
    aec_cfg.aec_cfg.ns_type = NS_TRADITION;
    aec_cfg.dual_ch = 2;
    aec_cfg.vad_cfg.vad_enable = 0;
    aec_cfg.out_block_size = MYBOT_AUDIO_BLOCK_BYTES;
    aec_cfg.out_block_num = MYBOT_CAPTURE_DEVICE_BLOCKS;
    aec_cfg.multi_in_port_num = 0;
    aec_cfg.multi_out_port_num = 0;
    /* Match the reference DSP options; MyBot consumes the main mono output.
     * Its ASR/UI side-output ports and direction callbacks are not needed. */
    aec_cfg.aec_cfg.ec_only_output = 1;
    aec_cfg.aec_cfg.multi_output_use_ec_out = 1;
    MYBOT_LOGI(TAG, "AEC geometry: R2 appended REF trimic=1 NS=traditional VAD=0 EC_out=1 delay=%u frame=%d blocks=%d",
               (unsigned int)aec_cfg.aec_cfg.delay_points,
               aec_cfg.out_block_size, aec_cfg.out_block_num);
    ctx->aec = aec_v3_algorithm_init(&aec_cfg);
    if (!ctx->aec ||
        audio_element_set_event_callback(ctx->aec, discard_audio_event, NULL) != BK_OK) {
        goto fail;
    }

    stage = "raw_stream";
    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_READER;
    ctx->raw = raw_stream_init(&raw_cfg);
    if (!ctx->raw ||
        audio_element_set_input_timeout(
            ctx->raw, BK_MS_TO_TICKS(MYBOT_AUDIO_IO_TIMEOUT_MS)) != BK_OK ||
        audio_element_set_event_callback(ctx->raw, discard_audio_event, NULL) != BK_OK) {
        goto fail;
    }

    stage = "register_microphone";
    if (audio_pipeline_register(ctx->pipeline, ctx->device, "mic") != BK_OK) {
        goto fail;
    }
    ctx->device_registered = true;
    stage = "register_aec";
    if (audio_pipeline_register(ctx->pipeline, ctx->aec, "aec") != BK_OK) {
        goto fail;
    }
    ctx->aec_registered = true;
    stage = "register_raw_stream";
    if (audio_pipeline_register(ctx->pipeline, ctx->raw, "raw") != BK_OK) {
        goto fail;
    }
    ctx->raw_registered = true;

    stage = "link";
    const char *links[] = {"mic", "aec", "raw"};
    if (audio_pipeline_link(ctx->pipeline, links, 3) != BK_OK) {
        goto fail;
    }
    /* Check the actual producer geometry forwarded by the pipeline. */
    stage = "capture_layout";
    aud_uplink_caps_t caps = {0};
    if (audio_element_get_input_uplink_caps(ctx->aec, &caps) != BK_OK ||
        !caps.valid || caps.version != AUD_UPLINK_CAPS_VERSION ||
        caps.ch_bitmap != mic_cfg.ch_bitmap || caps.aec_en != 1 ||
        caps.ref_ch != AUD_UPLINK_REF_CH_AUTO) {
        MYBOT_LOGE(TAG, "capture layout rejected: valid=%u version=%u bitmap=0x%x adc_aec=%u ref_lane=%u",
                   (unsigned int)caps.valid, (unsigned int)caps.version,
                   (unsigned int)caps.ch_bitmap, (unsigned int)caps.aec_en,
                   (unsigned int)caps.ref_ch);
        goto fail;
    }
    MYBOT_LOGI(TAG, "capture layout verified: bitmap=0x7 lanes=4 mic=[0,1,2] appended_ref=3, output=16000 Hz mono S16");
    stage = "raw_port";
    ctx->raw_port = audio_element_get_input_port(ctx->raw);
    if (!ctx->raw_port) {
        goto fail;
    }

    *out_ctx = ctx;
    MYBOT_LOGI(TAG, "capture ready");
    return 0;

fail:
    MYBOT_LOGE(TAG, "capture init failed at %s", stage);
    audio_context_release(ctx);
    return -1;
}

static int capture_start(void *opaque) {
    MYBOT_LOGI(TAG, "capture start requested");
    return audio_context_start(opaque);
}

static int capture_read(void *opaque, void *buf, int frames) {
    audio_context_t *ctx = opaque;
    if (!ctx || !ctx->raw || !buf || frames <= 0 ||
        frames > INT_MAX / (int)sizeof(int16_t)) {
        return -1;
    }
    if (!io_begin(&ctx->io)) {
        return -1;
    }

    int requested_bytes = frames * (int)sizeof(int16_t);
    int bytes = raw_stream_read(ctx->raw, buf, requested_bytes);
    bool stopping = io_end(&ctx->io);
    if (bytes == 0 || bytes == AEL_IO_TIMEOUT ||
        (bytes == AEL_IO_ABORT && stopping)) {
        return 0;
    }
    if (bytes < 0 || bytes > requested_bytes ||
        (bytes % (int)sizeof(int16_t)) != 0) {
        return -1;
    }
    return bytes / (int)sizeof(int16_t);
}

static int capture_stop(void *opaque) {
    MYBOT_LOGI(TAG, "capture stop requested");
    return audio_context_stop(opaque);
}

static void capture_destroy(void *opaque) {
    audio_context_t *ctx = opaque;
    if (!ctx) {
        return;
    }
    MYBOT_LOGI(TAG, "capture destroy requested");
    (void)audio_context_stop(ctx);
    audio_context_release(ctx);
}

static int playback_init(void **out_ctx, int rate, int channels, int bits) {
    const char *stage = "validate";

    if (!out_ctx || !format_supported(rate, channels, bits)) {
        MYBOT_LOGE(TAG, "playback init rejected: format=%d Hz/%d ch/%d bit", rate, channels,
                bits);
        return -1;
    }
    *out_ctx = NULL;
    MYBOT_LOGI(TAG, "playback init: format=%d Hz/%d ch/%d bit", rate, channels, bits);

    stage = "allocate";
    audio_context_t *ctx = aosl_calloc(1, sizeof(*ctx));
    if (!ctx || io_state_init(&ctx->io) < 0) {
        MYBOT_LOGE(TAG, "playback init failed at allocate");
        aosl_free(ctx);
        return -1;
    }
    stage = "playback_owner";
    if (playback_owner_acquire(ctx) < 0) {
        MYBOT_LOGE(TAG, "playback init failed at playback owner");
        io_state_destroy(&ctx->io);
        aosl_free(ctx);
        return -1;
    }
    stage = "audio_cpu_vote";
    if (audio_pm_acquire() < 0) {
        MYBOT_LOGE(TAG, "playback init failed at audio CPU vote");
        audio_context_release(ctx);
        return -1;
    }
    ctx->pm_acquired = true;

    stage = "pipeline";
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    ctx->pipeline = audio_pipeline_init(&pipeline_cfg);
    if (!ctx->pipeline) {
        goto fail;
    }

    stage = "raw_stream";
    raw_stream_cfg_t raw_cfg = {
        .type = AUDIO_STREAM_WRITER,
        .out_block_size = MYBOT_AUDIO_BLOCK_BYTES,
        .out_block_num = MYBOT_PLAYBACK_RAW_BLOCKS,
        .output_port_type = PORT_TYPE_RB,
    };
    ctx->raw = raw_stream_init(&raw_cfg);
    if (!ctx->raw ||
        audio_element_set_output_timeout(
            ctx->raw, BK_MS_TO_TICKS(MYBOT_AUDIO_IO_TIMEOUT_MS)) != BK_OK) {
        goto fail;
    }

    stage = "speaker";
    onboard_speaker_stream_cfg_t speaker_cfg = DEFAULT_ONBOARD_SPEAKER_STREAM_CONFIG();
    speaker_cfg.chl_num = MYBOT_AUDIO_CHANNELS;
    speaker_cfg.bits = MYBOT_AUDIO_BITS;
    speaker_cfg.multi_in_port_num = 0;
    speaker_cfg.multi_out_port_num = 0;
    speaker_cfg.dac_source_bitmap = ONBOARD_SPEAKER_STREAM_DAC_SOURCE_CALL_BIT;
    speaker_cfg.main_dac_source = AUD_DAC_SOURCE_CALL;
    speaker_cfg.sample_rate[AUD_DAC_SOURCE_CALL] = MYBOT_AUDIO_RATE;
    speaker_cfg.frame_size[AUD_DAC_SOURCE_CALL] = MYBOT_AUDIO_BLOCK_BYTES;
    speaker_cfg.pool_length = MYBOT_AUDIO_POOL_BYTES;
    speaker_cfg.pa_ctrl_en = true;
    speaker_cfg.pa_ctrl_gpio = GPIO_50;
    speaker_cfg.pa_on_level = 1;
    speaker_cfg.pa_on_delay = 10;
    speaker_cfg.pa_off_delay = 30;
    ctx->device = onboard_speaker_stream_init(&speaker_cfg);
    if (!ctx->device ||
        audio_element_set_event_callback(ctx->raw, discard_audio_event, NULL) != BK_OK ||
        audio_element_set_event_callback(ctx->device, discard_audio_event, NULL) != BK_OK) {
        goto fail;
    }

    stage = "register_raw_stream";
    if (audio_pipeline_register(ctx->pipeline, ctx->raw, "raw") != BK_OK) {
        goto fail;
    }
    ctx->raw_registered = true;
    stage = "register_speaker";
    if (audio_pipeline_register(ctx->pipeline, ctx->device, "speaker") != BK_OK) {
        goto fail;
    }
    ctx->device_registered = true;

    stage = "link";
    const char *links[] = {"raw", "speaker"};
    if (audio_pipeline_link(ctx->pipeline, links, 2) != BK_OK) {
        goto fail;
    }
    stage = "raw_port";
    ctx->raw_port = audio_element_get_output_port(ctx->raw);
    if (!ctx->raw_port) {
        goto fail;
    }

    stage = "publish_gain";
    if (bk7259_audio_playback_gain_publish(ctx) < 0) {
        goto fail;
    }

    *out_ctx = ctx;
    MYBOT_LOGI(TAG, "playback ready");
    return 0;

fail:
    MYBOT_LOGE(TAG, "playback init failed at %s", stage);
    audio_context_release(ctx);
    return -1;
}

static int playback_start(void *opaque) {
    MYBOT_LOGI(TAG, "playback start requested");
    int result = audio_context_start(opaque);
    if (result < 0) {
        return result;
    }
    if (bk7259_audio_playback_gain_publish(opaque) < 0) {
        MYBOT_LOGE(TAG, "playback start failed to publish gain context");
        (void)audio_context_stop(opaque);
        return -1;
    }
    return 0;
}

static int playback_write(void *opaque, const void *buf, int frames) {
    audio_context_t *ctx = opaque;
    if (!ctx || !ctx->raw || !buf || frames <= 0 ||
        frames > INT_MAX / (int)sizeof(int16_t)) {
        return -1;
    }
    if (!io_begin(&ctx->io)) {
        return -1;
    }

    int requested_bytes = frames * (int)sizeof(int16_t);
    int bytes = raw_stream_write(ctx->raw, (char *)buf, requested_bytes);
    bool stopping = io_end(&ctx->io);
    if (bytes == 0 || bytes == AEL_IO_TIMEOUT ||
        (bytes == AEL_IO_ABORT && stopping)) {
        return 0;
    }
    if (bytes < 0 || bytes > requested_bytes ||
        (bytes % (int)sizeof(int16_t)) != 0) {
        return -1;
    }
    return bytes / (int)sizeof(int16_t);
}

static int playback_stop(void *opaque) {
    /* Stop accepting gain operations before interrupting the pipeline. */
    MYBOT_LOGI(TAG, "playback stop requested");
    bk7259_audio_playback_gain_unpublish(opaque);
    return audio_context_stop(opaque);
}

static void playback_destroy(void *opaque) {
    audio_context_t *ctx = opaque;
    if (!ctx) {
        return;
    }
    MYBOT_LOGI(TAG, "playback destroy requested");
    bk7259_audio_playback_gain_unpublish(ctx);
    (void)audio_context_stop(ctx);
    audio_context_release(ctx);
}

const mybot_audio_capture_ops_t g_mybot_bk7259_capture_ops = {
    .init = capture_init,
    .start = capture_start,
    .read = capture_read,
    .stop = capture_stop,
    .destroy = capture_destroy,
};

const mybot_audio_playback_ops_t g_mybot_bk7259_playback_ops = {
    .init = playback_init,
    .start = playback_start,
    .write = playback_write,
    .stop = playback_stop,
    .destroy = playback_destroy,
};
