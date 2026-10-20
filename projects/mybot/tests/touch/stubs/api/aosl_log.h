/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#define AOSL_LOG_NOTICE 5
#define AOSL_LOG_WARNING 4
#define AOSL_LOG_ERROR 3
#define AOSL_LOG_DEBUG 7
static inline void aosl_log(int level, const char *format, ...) {
    (void)level;
    (void)format;
}
