/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_BK7259_PLATFORM_H_
#define MYBOT_BK7259_PLATFORM_H_

#include <stdbool.h>
#include <stdint.h>

/* Transport selection is shared with the SDK's compile-time policy. */
#if defined(CONFIG_MYBOT_HTTPS) && CONFIG_MYBOT_HTTPS
#define MYBOT_SERVER_SCHEME "https://"
#else
#define MYBOT_SERVER_SCHEME "http://"
#endif

/* Build-time language selection shared by the AP lifecycle and prompt
 * implementation. The service endpoint and embedded asset tag stay coupled. */
#if defined(CONFIG_MYBOT_LANGUAGE_EN_US) && CONFIG_MYBOT_LANGUAGE_EN_US
#define MYBOT_LANGUAGE_TAG "en-US"
#define MYBOT_SERVER_BASE MYBOT_SERVER_SCHEME "botstation.agora.io/api"
#else
#define MYBOT_LANGUAGE_TAG "zh-CN"
/* The /api prefix is part of the device service route. */
#define MYBOT_SERVER_BASE MYBOT_SERVER_SCHEME "botstation.shengwang.cn/api"
#endif

#define MYBOT_ASSETS_DIR "mybot/assets"

#ifdef __cplusplus
extern "C" {
#endif

int mybot_bk7259_platform_prepare(void);
int mybot_bk7259_platform_register(void);
/* Returns 1 when APSTA was required, 0 for an existing connection, or a
 * negative error/reset/provisioning result (see bk7259_provision.h). */
int mybot_bk7259_ensure_network(const char *device_id);
int mybot_bk7259_provision_wifi(const char *device_id);
void mybot_bk7259_platform_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_BK7259_PLATFORM_H_ */
