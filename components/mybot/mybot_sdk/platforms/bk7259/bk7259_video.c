/* SPDX-License-Identifier: Apache-2.0 */
/*
 * BK7259 MyBot video source.
 *
 * The Beken robot demo sends frames through video_engine/network_engine.  MyBot
 * owns the RTC transport, so this adapter keeps only the camera + H.264 source
 * side: MIPI CSI -> ISP MP (NV12) -> HW-flexa H.264 -> encoded frame pool.
 */

#include <mybot/platform/mybot_video.h>

#include "bk7259_platform_log.h"

#include <common/bk_err.h>
#include <components/bk_flexa_bond.h>
#include <components/bk_encode/bk_h264_encode_ctlr.h>
#include <components/bk_encode/bk_h264_encode_types.h>
#include <driver/gpio.h>
#include <gpio_driver.h>
#include <os/mem.h>
#include <os/os.h>

#include "app_camera.h"
#include "app_camera_types.h"
#include "app_codec.h"
#include "multimedia_img_manager.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef CONFIG_MYBOT_VIDEO_WIDTH
#define CONFIG_MYBOT_VIDEO_WIDTH 640
#endif
#ifndef CONFIG_MYBOT_VIDEO_HEIGHT
#define CONFIG_MYBOT_VIDEO_HEIGHT 480
#endif
#ifndef CONFIG_MYBOT_VIDEO_SENSOR_WIDTH
#define CONFIG_MYBOT_VIDEO_SENSOR_WIDTH 1280
#endif
#ifndef CONFIG_MYBOT_VIDEO_SENSOR_HEIGHT
#define CONFIG_MYBOT_VIDEO_SENSOR_HEIGHT 720
#endif
#ifndef CONFIG_MYBOT_VIDEO_SENSOR_FPS
#define CONFIG_MYBOT_VIDEO_SENSOR_FPS 5
#endif
#ifndef CONFIG_MYBOT_VIDEO_MIN_BPS
#define CONFIG_MYBOT_VIDEO_MIN_BPS 256000
#endif
#ifndef CONFIG_MYBOT_VIDEO_MAX_BPS
#define CONFIG_MYBOT_VIDEO_MAX_BPS 512000
#endif

#define TAG "mybot_video"
#define VIDEO_QUEUE_WAIT_MS 100U
#define VIDEO_STOP_WAIT_MS 3000U
#define VIDEO_ENCODER_DRAIN_MS 80U
#define VIDEO_CAMERA_RESET_ASSERT_MS 5U
#define VIDEO_FRAME_POOL_MAX 16U
#define VIDEO_TASK_PRIORITY BEKEN_DEFAULT_WORKER_PRIORITY
#define VIDEO_TASK_STACK_SIZE (8U * 1024U)

/* Robot R2 BK7259 MIPI CSI (GC2053) wiring. */
#define VIDEO_CAM_SCL GPIO_64
#define VIDEO_CAM_SDA GPIO_65
#define VIDEO_CAM_RESET GPIO_30
#define VIDEO_CAM_XCLK GPIO_59
#define VIDEO_CAM_I2C_ID 0

typedef struct {
    beken_mutex_t lock;
    beken_semaphore_t task_done;
    beken_thread_t task;
    mybot_video_frame_handler_t handler;
    void *handler_user_data;
    void *h264_bond;
    uint32_t width;
    uint32_t height;
    uint32_t target_bps;
    bool running;
    bool stopping;
    bool camera_opened;
    bool camera_open_attempted;
    bool camera_reset_pending;
} bk7259_video_context_t;

static void video_task(void *arg);
static int video_camera_close(bk7259_video_context_t *ctx);

static int video_camera_reset_set(bool asserted)
{
    bk_err_t ret;

    ret = gpio_dev_unmap(VIDEO_CAM_RESET);
    if (ret != BK_OK) {
        MYBOT_LOGE(TAG, "camera reset GPIO unmap failed: %d", ret);
        return -1;
    }
    ret = bk_gpio_enable_output(VIDEO_CAM_RESET);
    if (ret != BK_OK) {
        MYBOT_LOGE(TAG, "camera reset GPIO output failed: %d", ret);
        return -1;
    }
    ret = asserted ? bk_gpio_set_output_low(VIDEO_CAM_RESET)
                   : bk_gpio_set_output_high(VIDEO_CAM_RESET);
    if (ret != BK_OK) {
        MYBOT_LOGE(TAG, "camera reset GPIO level failed: %d", ret);
        return -1;
    }
    return 0;
}

static int video_set_rate_locked(bk7259_video_context_t *ctx, uint32_t bitrate)
{
    bk_h264_encode_rate_ctrl_t rate_ctrl;
    void *encoder;

    if (ctx == NULL || !ctx->camera_opened || ctx->stopping) {
        return -1;
    }
    if (bitrate < CONFIG_MYBOT_VIDEO_MIN_BPS) {
        bitrate = CONFIG_MYBOT_VIDEO_MIN_BPS;
    }
    if (bitrate > CONFIG_MYBOT_VIDEO_MAX_BPS) {
        bitrate = CONFIG_MYBOT_VIDEO_MAX_BPS;
    }

    encoder = app_h264_encode_handle_get();
    if (encoder == NULL) {
        return -1;
    }

    rate_ctrl = (bk_h264_encode_rate_ctrl_t){
        .bitrate = bitrate,
        .qp_min_i = 20,
        .qp_max_i = 45,
        .qp_min_p = 20,
        .qp_max_p = 45,
    };
    if (bk_h264_encode_set_rate_ctrl(encoder, &rate_ctrl) != BK_OK) {
        MYBOT_LOGW(TAG, "H.264 bitrate update failed: %u", (unsigned)bitrate);
        return -1;
    }
    ctx->target_bps = bitrate;
    return 0;
}

static int video_camera_open(bk7259_video_context_t *ctx)
{
    camera_board_config_t config = {0};
    void *isp;
    void *encoder;
    int ret;

    /* GPIO29 supplies both LCD and camera; GPIO27 enables the replacement
     * sensor's external 1.2V rail. The display owner holds both board rails
     * from platform prepare through shutdown. Hold the sensor in reset while
     * the camera-specific AuxLDOs and CSI clock are brought up.
     * The sensor driver's detect callback releases
     * reset after the bus is enabled, producing a deterministic low-to-high
     * edge even after a previous failed start. */
    if (video_camera_reset_set(true) < 0) {
        return -1;
    }
    ctx->camera_reset_pending = true;
    rtos_delay_milliseconds(VIDEO_CAMERA_RESET_ASSERT_MS);

    config.mipi.enable = true;
    config.mipi.pin_scl = VIDEO_CAM_SCL;
    config.mipi.pin_sda = VIDEO_CAM_SDA;
    config.mipi.i2c_id = VIDEO_CAM_I2C_ID;
    config.mipi.pin_reset = VIDEO_CAM_RESET;
    config.mipi.pin_pwdn = (uint8_t)-1;
    config.mipi.pin_xclk = VIDEO_CAM_XCLK;
    config.mipi.sensor_max_width = CONFIG_MYBOT_VIDEO_SENSOR_WIDTH;
    config.mipi.sensor_max_height = CONFIG_MYBOT_VIDEO_SENSOR_HEIGHT;
    config.mipi.sensor_fps = CONFIG_MYBOT_VIDEO_SENSOR_FPS;
    config.mipi.hmirror = 1;
    config.mipi.vflip = 0;

    config.isp.mp_enable = true;
    config.isp.mp_flexa = true;
    config.isp.mp_width = (uint16_t)CONFIG_MYBOT_VIDEO_WIDTH;
    config.isp.mp_height = (uint16_t)CONFIG_MYBOT_VIDEO_HEIGHT;
    config.isp.mp_format = BK_PIXEL_FORMAT_NV12;

    ret = app_camera_board_config_set(&config);
    if (ret != BK_OK) {
        MYBOT_LOGE(TAG, "camera board config failed: %d", ret);
        (void)video_camera_close(ctx);
        return -1;
    }
    ctx->camera_open_attempted = true;
    ret = app_isp_mipi_camera_turn_on(app_camera_board_config_get());
    if (ret != BK_OK) {
        MYBOT_LOGE(TAG, "MIPI camera open failed: %d", ret);
        (void)video_camera_close(ctx);
        return -1;
    }
    ctx->camera_opened = true;

    ret = app_h264e_turn_on();
    if (ret != BK_OK) {
        MYBOT_LOGE(TAG, "H.264 encoder open failed: %d", ret);
        (void)video_camera_close(ctx);
        return -1;
    }

    isp = app_isp_handle_get();
    encoder = app_h264_encode_handle_get();
    if (isp == NULL || encoder == NULL) {
        MYBOT_LOGE(TAG, "camera handles unavailable (isp=%p encoder=%p)", isp, encoder);
        (void)video_camera_close(ctx);
        return -1;
    }

    ret = bk_flexa_isp_h264e_bond_start(&ctx->h264_bond, isp, encoder);
    if (ret != BK_OK) {
        MYBOT_LOGE(TAG, "ISP/H.264 FLEXA bond failed: %d", ret);
        ctx->h264_bond = NULL;
        (void)video_camera_close(ctx);
        return -1;
    }

    ctx->width = CONFIG_MYBOT_VIDEO_WIDTH;
    ctx->height = CONFIG_MYBOT_VIDEO_HEIGHT;
    ctx->target_bps = CONFIG_MYBOT_VIDEO_MIN_BPS +
                      (CONFIG_MYBOT_VIDEO_MAX_BPS - CONFIG_MYBOT_VIDEO_MIN_BPS) / 2U;
    if (video_set_rate_locked(ctx, ctx->target_bps) < 0) {
        MYBOT_LOGW(TAG, "using encoder default bitrate");
    }
    MYBOT_LOGI(TAG, "H.264 source started: %ux%u, sensor=%ux%u@%u",
               (unsigned)ctx->width, (unsigned)ctx->height,
               (unsigned)CONFIG_MYBOT_VIDEO_SENSOR_WIDTH,
               (unsigned)CONFIG_MYBOT_VIDEO_SENSOR_HEIGHT,
               (unsigned)CONFIG_MYBOT_VIDEO_SENSOR_FPS);
    return 0;
}

static int video_camera_close(bk7259_video_context_t *ctx)
{
    int result = 0;

    if (ctx == NULL) {
        return -1;
    }
    if (ctx->h264_bond != NULL) {
        bk_flexa_isp_h264e_bond_stop(ctx->h264_bond);
        ctx->h264_bond = NULL;
        rtos_delay_milliseconds(VIDEO_ENCODER_DRAIN_MS);
    }
    if (app_h264e_turn_off() != BK_OK) {
        MYBOT_LOGE(TAG, "H.264 encoder close failed");
        result = -1;
    }
    /* app_h264e_turn_off() is intentionally idempotent, but a failed encoder
     * open may have initialized the shared pool without creating a handle.
     * Release the producer side explicitly in both cases. */
    if (bk_encoded_data_manager_deinit(1) != BK_OK) {
        MYBOT_LOGW(TAG, "encoded frame producer cleanup reported an error");
        result = -1;
    }
    if (ctx->camera_open_attempted && app_isp_camera_turn_off() != BK_OK) {
        MYBOT_LOGE(TAG, "MIPI camera close failed");
        result = -1;
    }
    if (ctx->camera_reset_pending) {
        if (video_camera_reset_set(true) < 0) {
            result = -1;
        } else {
            ctx->camera_reset_pending = false;
        }
    }
    /* app_h264e_turn_off() releases the producer side.  Release the MyBot
     * consumer side only after the transfer task has returned its last frame. */
    if (bk_encoded_data_manager_deinit(0) != BK_OK) {
        MYBOT_LOGW(TAG, "encoded frame consumer cleanup reported an error");
        result = -1;
    }
    /* The shared manager's v4.0.1 consumer deinit normalizes every slot into
     * its ready queue.  Move those bounded slots back to free before a later
     * start, otherwise the next encoder has no output buffer available. */
    for (uint32_t i = 0; i < VIDEO_FRAME_POOL_MAX; ++i) {
        frame_buffer_t *frame =
            (frame_buffer_t *)bk_encoded_complete_data_request(BEKEN_NO_WAIT);
        if (frame == NULL) {
            break;
        }
        if (bk_encoded_complete_data_free_request((uint8_t *)frame) != BK_OK) {
            MYBOT_LOGW(TAG, "encoded frame pool reset failed");
            result = -1;
            break;
        }
    }
    ctx->camera_opened = false;
    ctx->camera_open_attempted = false;
    ctx->width = 0;
    ctx->height = 0;
    ctx->target_bps = 0;
    if (result == 0) {
        MYBOT_LOGI(TAG, "video stopped: sensor reset asserted; shared LCD/camera 2.8V rail retained");
    }
    return result;
}

static void video_task(void *arg)
{
    bk7259_video_context_t *ctx = (bk7259_video_context_t *)arg;

    for (;;) {
        frame_buffer_t *frame;
        bool running;
        bool stopping;
        bool deliver;
        mybot_video_frame_handler_t handler;
        void *user_data;

        if (ctx == NULL) {
            break;
        }
        rtos_lock_mutex(&ctx->lock);
        running = ctx->running;
        stopping = ctx->stopping;
        handler = ctx->handler;
        user_data = ctx->handler_user_data;
        rtos_unlock_mutex(&ctx->lock);
        if (!running || stopping) {
            break;
        }

        frame = (frame_buffer_t *)bk_encoded_complete_data_request(VIDEO_QUEUE_WAIT_MS);
        if (frame == NULL) {
            continue;
        }

        /* Stop may have been requested while the bounded queue wait was in
         * progress.  Re-check ownership before handing the borrowed payload
         * to the SDK so shutdown drops the frame instead of starting another
         * RTC send. */
        rtos_lock_mutex(&ctx->lock);
        deliver = ctx->running && !ctx->stopping;
        handler = ctx->handler;
        user_data = ctx->handler_user_data;
        rtos_unlock_mutex(&ctx->lock);

        /* The encoded pool is owned by the H.264 producer.  Keep the payload
         * borrowed only for the handler call, then return the slot immediately. */
        if (deliver && frame->frame != NULL && frame->length != 0 &&
            frame->fmt == PIXEL_FMT_H264 && handler != NULL) {
            mybot_video_frame_t encoded = {
                .data = frame->frame,
                .len = frame->length,
                .codec = MYBOT_VIDEO_CODEC_H264,
            };
            (void)handler(&encoded, user_data);
        }
        if (bk_encoded_complete_data_free_request((uint8_t *)frame) != BK_OK) {
            MYBOT_LOGW(TAG, "encoded frame return failed");
        }
    }

    rtos_lock_mutex(&ctx->lock);
    ctx->task = NULL;
    rtos_unlock_mutex(&ctx->lock);
    if (rtos_set_semaphore(&ctx->task_done) != BK_OK) {
        MYBOT_LOGE(TAG, "video task completion signal failed");
    }
    rtos_delete_thread(NULL);
}

static int video_init(void **out_ctx, mybot_video_frame_handler_t handler,
                      void *user_data)
{
    bk7259_video_context_t *ctx;

    if (out_ctx == NULL || handler == NULL ||
        CONFIG_MYBOT_VIDEO_MAX_BPS == 0 ||
        CONFIG_MYBOT_VIDEO_MAX_BPS < CONFIG_MYBOT_VIDEO_MIN_BPS) {
        return -1;
    }
    ctx = (bk7259_video_context_t *)os_malloc(sizeof(*ctx));
    if (ctx == NULL) {
        return -1;
    }
    os_memset(ctx, 0, sizeof(*ctx));
    if (rtos_init_mutex(&ctx->lock) != BK_OK ||
        rtos_init_semaphore_ex(&ctx->task_done, 1, 0) != BK_OK) {
        if (ctx->lock != NULL) {
            (void)rtos_deinit_mutex(&ctx->lock);
        }
        os_free(ctx);
        return -1;
    }
    ctx->handler = handler;
    ctx->handler_user_data = user_data;
    *out_ctx = ctx;
    return 0;
}

static int video_start(void *opaque)
{
    bk7259_video_context_t *ctx = (bk7259_video_context_t *)opaque;
    int ret;

    if (ctx == NULL) {
        return -1;
    }
    rtos_lock_mutex(&ctx->lock);
    if (ctx->running || ctx->stopping) {
        rtos_unlock_mutex(&ctx->lock);
        return ctx->running ? 0 : -1;
    }
    ctx->stopping = false;
    /* A prior successful worker exit may have left its completion token set. */
    while (rtos_get_semaphore(&ctx->task_done, BEKEN_NO_WAIT) == BK_OK) {
    }
    ret = video_camera_open(ctx);
    if (ret < 0) {
        rtos_unlock_mutex(&ctx->lock);
        return -1;
    }
    ctx->running = true;
    ret = rtos_create_psram_thread(&ctx->task, VIDEO_TASK_PRIORITY,
                                   "mybot_video", video_task,
                                   VIDEO_TASK_STACK_SIZE, ctx);
    if (ret != BK_OK) {
        ctx->running = false;
        (void)video_camera_close(ctx);
        rtos_unlock_mutex(&ctx->lock);
        return -1;
    }
    rtos_unlock_mutex(&ctx->lock);
    return 0;
}

static int video_stop(void *opaque)
{
    bk7259_video_context_t *ctx = (bk7259_video_context_t *)opaque;
    beken_thread_t task;
    int result = 0;

    if (ctx == NULL) {
        return 0;
    }
    rtos_lock_mutex(&ctx->lock);
    if (!ctx->running && ctx->task == NULL && !ctx->camera_opened &&
        !ctx->camera_reset_pending) {
        ctx->stopping = false;
        rtos_unlock_mutex(&ctx->lock);
        return 0;
    }
    ctx->running = false;
    ctx->stopping = true;
    task = ctx->task;
    rtos_unlock_mutex(&ctx->lock);

    if (task != NULL &&
        rtos_get_semaphore(&ctx->task_done, VIDEO_STOP_WAIT_MS) != BK_OK) {
        MYBOT_LOGW(TAG, "video task did not stop within %u ms",
                   (unsigned)VIDEO_STOP_WAIT_MS);
        return -1;
    }
    rtos_lock_mutex(&ctx->lock);
    task = ctx->task;
    rtos_unlock_mutex(&ctx->lock);
    if (task != NULL) {
        return -1;
    }

    rtos_lock_mutex(&ctx->lock);
    result = video_camera_close(ctx);
    ctx->stopping = false;
    rtos_unlock_mutex(&ctx->lock);
    return result;
}

static void video_key_frame_request(void *opaque)
{
    bk7259_video_context_t *ctx = (bk7259_video_context_t *)opaque;
    void *encoder;

    if (ctx == NULL) {
        return;
    }
    rtos_lock_mutex(&ctx->lock);
    if (!ctx->running || ctx->stopping || !ctx->camera_opened) {
        rtos_unlock_mutex(&ctx->lock);
        return;
    }
    encoder = app_h264_encode_handle_get();
    if (encoder != NULL && bk_h264_encode_force_idr(encoder) != BK_OK) {
        MYBOT_LOGW(TAG, "H.264 key-frame request failed");
    }
    rtos_unlock_mutex(&ctx->lock);
}

static void video_target_bitrate_changed(void *opaque, uint32_t target_bps)
{
    bk7259_video_context_t *ctx = (bk7259_video_context_t *)opaque;

    if (ctx == NULL) {
        return;
    }
    rtos_lock_mutex(&ctx->lock);
    if (ctx->running && !ctx->stopping) {
        (void)video_set_rate_locked(ctx, target_bps);
    }
    rtos_unlock_mutex(&ctx->lock);
}

static void video_destroy(void *opaque)
{
    bk7259_video_context_t *ctx = (bk7259_video_context_t *)opaque;

    if (ctx == NULL) {
        return;
    }
    if (ctx->task != NULL || ctx->running || ctx->camera_opened ||
        ctx->camera_reset_pending) {
        MYBOT_LOGE(TAG, "destroy called before video stop; retaining resources");
        return;
    }
    if (rtos_deinit_semaphore(&ctx->task_done) != BK_OK) {
        MYBOT_LOGW(TAG, "video task semaphore deinit failed");
    }
    if (rtos_deinit_mutex(&ctx->lock) != BK_OK) {
        MYBOT_LOGW(TAG, "video mutex deinit failed");
    }
    os_free(ctx);
}

const mybot_video_ops_t g_mybot_bk7259_video_ops = {
    .min_bps = CONFIG_MYBOT_VIDEO_MIN_BPS,
    .max_bps = CONFIG_MYBOT_VIDEO_MAX_BPS,
    .init = video_init,
    .start = video_start,
    .stop = video_stop,
    .on_key_frame_request = video_key_frame_request,
    .on_target_bitrate_changed = video_target_bitrate_changed,
    .destroy = video_destroy,
};
