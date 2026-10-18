/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_BK7259_OPS_H_
#define MYBOT_BK7259_OPS_H_

#include "mybot_bk7259_platform.h"

#include <mybot/platform/mybot_audio.h>
#include <mybot/platform/mybot_announce.h>
#include <mybot/platform/mybot_https.h>
#include <mybot/platform/mybot_key.h>
#include <mybot/platform/mybot_kv_store.h>
#include <mybot/platform/mybot_lcd.h>
#include <mybot/platform/mybot_video.h>
#include <mybot/platform/mybot_wifi.h>

extern const mybot_audio_capture_ops_t g_mybot_bk7259_capture_ops;
extern const mybot_audio_playback_ops_t g_mybot_bk7259_playback_ops;
extern const mybot_audio_volume_ops_t g_mybot_bk7259_volume_ops;
extern const mybot_announce_ops_t g_mybot_bk7259_announce_ops;
extern const mybot_https_ops_t g_mybot_bk7259_https_ops;
extern const mybot_key_ops_t g_mybot_bk7259_key_ops;
extern const mybot_kv_store_ops_t g_mybot_bk7259_kv_ops;
extern const mybot_lcd_ops_t g_mybot_bk7259_lcd_ops;
extern const mybot_video_ops_t g_mybot_bk7259_video_ops;
extern const mybot_wifi_ops_t g_mybot_bk7259_wifi_ops;

int bk7259_key_prepare(void);
void bk7259_key_shutdown(void);
int bk7259_lcd_prepare(void);
void bk7259_lcd_shutdown(void);
int bk7259_lcd_show_screen(mybot_lcd_screen_t screen);
void bk7259_lcd_set_provisioning_ssid(const char *ssid);
int bk7259_wifi_prepare(void);
void bk7259_wifi_shutdown(void);

/* The V2 speaker stream owns the DAC handle.  These wrappers serialize
 * volume access with playback teardown and keep that private handle out of
 * the volume adapter. */
int bk7259_audio_playback_gain_publish(void *playback_ctx);
void bk7259_audio_playback_gain_unpublish(void *playback_ctx);
int bk7259_audio_playback_gain_set(float gain_db);
int bk7259_audio_playback_gain_get(float *gain_db);

int bk7259_prompt_play_provisioning(void);
int bk7259_prompt_play_success(void);

#endif /* MYBOT_BK7259_OPS_H_ */
