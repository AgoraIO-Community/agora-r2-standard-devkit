/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
enum { AOSL_LOG_ERROR = 3, AOSL_LOG_WARNING = 4, AOSL_LOG_NOTICE = 5, AOSL_LOG_DEBUG = 7 };
void aosl_log(int level, const char *format, ...);
