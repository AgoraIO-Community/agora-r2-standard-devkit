/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_bk7259_platform.h"
#include "bk7259_ops.h"
#include "bk7259_gimbal.h"

#include "bk7259_platform_log.h"
#include <components/system.h>
#include <mybot/platform/mybot_platform.h>
#include <os/os.h>

#define TAG "mybot_platform"

static bool s_prepared;

int mybot_bk7259_platform_prepare(void) {
    if (s_prepared) {
        return 0;
    }
    if (bk7259_lcd_prepare() < 0) {
        MYBOT_LOGE(TAG, "LCD preparation failed");
        return -1;
    }
    if (bk7259_wifi_prepare() < 0) {
        MYBOT_LOGE(TAG, "Wi-Fi preparation failed");
        bk7259_lcd_shutdown();
        return -1;
    }
    if (bk7259_key_prepare() < 0) {
        MYBOT_LOGE(TAG, "key preparation failed");
        bk7259_wifi_shutdown();
        bk7259_lcd_shutdown();
        return -1;
    }
    if (bk7259_gimbal_prepare() < 0) {
        /* Optional actuator failure must not prevent voice/UI startup. */
        MYBOT_LOGW(TAG, "gimbal preparation failed; continuing without motion");
    }
    s_prepared = true;
    MYBOT_LOGI(TAG, "platform prepared");
    return 0;
}

int mybot_bk7259_platform_register(void) {
    static const mybot_platform_descriptor_t descriptor = {
        .wifi = &g_mybot_bk7259_wifi_ops,
        .kv_store = &g_mybot_bk7259_kv_ops,
        .key = &g_mybot_bk7259_key_ops,
        .audio_capture = &g_mybot_bk7259_capture_ops,
        .audio_playback = &g_mybot_bk7259_playback_ops,
        .audio_volume = &g_mybot_bk7259_volume_ops,
        .https = &g_mybot_bk7259_https_ops,
        .lcd = &g_mybot_bk7259_lcd_ops,
        .announce = &g_mybot_bk7259_announce_ops,
#if defined(CONFIG_MYBOT_VIDEO) && CONFIG_MYBOT_VIDEO
        .video = &g_mybot_bk7259_video_ops,
#else
        .video = NULL,
#endif
        .wake_words = NULL,
    };

    int result = mybot_platform_register(&descriptor);
    if (result < 0) {
        MYBOT_LOGE(TAG, "platform descriptor registration failed");
        return result;
    }
    MYBOT_LOGI(TAG, "platform descriptor registered");
    return 0;
}

void mybot_bk7259_platform_shutdown(void) {
    if (!s_prepared) {
        return;
    }
    MYBOT_LOGI(TAG, "platform shutting down");
    bk7259_key_shutdown();
    bk7259_wifi_shutdown();
    if (bk7259_gimbal_shutdown() < 0) {
        MYBOT_LOGE(TAG, "gimbal shutdown incomplete; retaining platform resources");
        return;
    }
    bk7259_lcd_shutdown();
    s_prepared = false;
    MYBOT_LOGI(TAG, "platform shut down");
}
