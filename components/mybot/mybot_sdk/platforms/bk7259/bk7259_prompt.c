/* SPDX-License-Identifier: Apache-2.0 */
#include "bk7259_ops.h"
#include "bk7259_assets.h"
#include "bk7259_ogg_pcm.h"
#include "bk7259_prompt.h"
#include <mybot_bk7259_platform.h>

#include "bk7259_platform_log.h"
#include <os/mem.h>
#include <os/os.h>

#include <api/aosl_thread.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TAG "mybot_prompt"
#define PROMPT_RATE_HZ 16000
#define PROMPT_CHANNELS 1
#define PROMPT_BITS 16
#define PROMPT_FRAMES_PER_WRITE 320
#define PROMPT_MAX_CONSECUTIVE_TIMEOUTS 200
#define PROMPT_DRAIN_MS 250
#define PROMPT_MAX_FRAMES (512U * 1024U / sizeof(int16_t))

#define PROVISIONING_PROMPT_PATH \
    MYBOT_ASSETS_DIR "/locales/" MYBOT_LANGUAGE_TAG "/wificonfig.ogg"
#define SUCCESS_PROMPT_PATH \
    MYBOT_ASSETS_DIR "/locales/" MYBOT_LANGUAGE_TAG "/success.ogg"

static aosl_static_lock_t s_prompt_lock = AOSL_STATIC_LOCK_INIT;

static int play_asset(const char *path) {
    bk7259_asset_t asset = {0};
    bk7259_ogg_pcm_t decoded = {0};
    void *playback_ctx = NULL;
    void *volume_ctx = NULL;
    bool playback_started = false;
    bool volume_active = false;
    int offset = 0;
    int timeout_count = 0;
    int result = -1;

    if (!path || bk7259_asset_find(path, &asset) < 0) {
        MYBOT_LOGW(TAG, "prompt asset unavailable: %s", path ? path : "(null)");
        return -1;
    }
    if (bk7259_ogg_pcm_load_memory(path, asset.data, asset.size, PROMPT_RATE_HZ,
                                   &decoded) < 0 || decoded.frames <= 0 ||
        (size_t)decoded.frames > PROMPT_MAX_FRAMES) {
        MYBOT_LOGW(TAG, "prompt decode failed or out of range: %s", path);
        bk7259_ogg_pcm_free(&decoded);
        return -1;
    }

    /* The SDK playback ops own the V2 speaker pipeline.  Reusing those ops
     * keeps the prompt path ABI-identical to normal MyBot audio and lets the
     * gain adapter reject a concurrent second speaker instance. */
    if (g_mybot_bk7259_playback_ops.init(&playback_ctx, PROMPT_RATE_HZ,
                                         PROMPT_CHANNELS, PROMPT_BITS) < 0) {
        goto cleanup;
    }
    if (g_mybot_bk7259_volume_ops.init(&volume_ctx) == 0) {
        volume_active = true;
    }
    if (g_mybot_bk7259_playback_ops.start(playback_ctx) < 0) {
        goto cleanup;
    }
    playback_started = true;

    while (offset < decoded.frames) {
        int requested = decoded.frames - offset;
        if (requested > PROMPT_FRAMES_PER_WRITE) {
            requested = PROMPT_FRAMES_PER_WRITE;
        }
        int written = g_mybot_bk7259_playback_ops.write(
            playback_ctx, decoded.pcm + offset, requested);
        if (written < 0 || written > requested) {
            MYBOT_LOGW(TAG, "prompt write failed: %s offset=%d", path, offset);
            goto cleanup;
        }
        if (written == 0) {
            if (++timeout_count >= PROMPT_MAX_CONSECUTIVE_TIMEOUTS) {
                MYBOT_LOGW(TAG, "prompt write timed out: %s", path);
                goto cleanup;
            }
            (void)rtos_delay_milliseconds(1);
            continue;
        }
        timeout_count = 0;
        offset += written;
    }

    /* Let the raw-stream and DAC queues drain before stop() tears down the
     * temporary pipeline; otherwise the tail of the prompt can be clipped. */
    (void)rtos_delay_milliseconds(PROMPT_DRAIN_MS);
    result = 0;

cleanup:
    if (playback_started) {
        (void)g_mybot_bk7259_playback_ops.stop(playback_ctx);
    }
    if (playback_ctx) {
        g_mybot_bk7259_playback_ops.destroy(playback_ctx);
    }
    /* The volume implementation deliberately does not touch the speaker in
     * destroy(); call it after playback_destroy() has released the handle. */
    if (volume_active) {
        g_mybot_bk7259_volume_ops.destroy(volume_ctx);
    }
    bk7259_ogg_pcm_free(&decoded);
    return result;
}

static int play_prompt(const char *path) {
    int result;

    if (aosl_static_lock_lock(&s_prompt_lock) < 0) {
        return -1;
    }
    result = play_asset(path);
    (void)aosl_static_lock_unlock(&s_prompt_lock);
    return result;
}

int bk7259_prompt_play_provisioning(void) {
    MYBOT_LOGI(TAG, "playing provisioning prompt");
    return play_prompt(PROVISIONING_PROMPT_PATH);
}

int bk7259_prompt_play_success(void) {
    MYBOT_LOGI(TAG, "playing provisioning success prompt");
    return play_prompt(SUCCESS_PROMPT_PATH);
}
