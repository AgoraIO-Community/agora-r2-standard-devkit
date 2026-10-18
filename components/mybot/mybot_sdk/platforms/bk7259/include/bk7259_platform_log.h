/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BK7259_PLATFORM_LOG_H_
#define BK7259_PLATFORM_LOG_H_

/* Platform log abstraction: route every platform-layer log line through the
 * AOSL logging API instead of the AVDK console macros. The AOSL HAL output
 * adds the shared prefix (for example the millisecond tick in
 * aosl_hal_printf) and a single AOSL log level gates every platform module.
 * Call sites keep their own line terminator, matching the existing BK7259
 * platform logs. */
#include <api/aosl_log.h>

#define BK7259_PLATFORM_LOG(level, tag, format, ...)                       \
    aosl_log((level), "[%d][aosl][%s][%s:%u]" format "\r\n", (level),      \
             (tag), __FUNCTION__, __LINE__, ##__VA_ARGS__)

#define MYBOT_LOGI(tag, format, ...)                                        \
    BK7259_PLATFORM_LOG(AOSL_LOG_NOTICE, tag, format, ##__VA_ARGS__)
#define MYBOT_LOGW(tag, format, ...)                                        \
    BK7259_PLATFORM_LOG(AOSL_LOG_WARNING, tag, format, ##__VA_ARGS__)
#define MYBOT_LOGE(tag, format, ...)                                        \
    BK7259_PLATFORM_LOG(AOSL_LOG_ERROR, tag, format, ##__VA_ARGS__)
#define MYBOT_LOGD(tag, format, ...)                                        \
    BK7259_PLATFORM_LOG(AOSL_LOG_DEBUG, tag, format, ##__VA_ARGS__)

#endif /* BK7259_PLATFORM_LOG_H_ */
