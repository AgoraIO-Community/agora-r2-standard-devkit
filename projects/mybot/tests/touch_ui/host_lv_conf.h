/* SPDX-License-Identifier: Apache-2.0 */
#include "../../ap/lv_conf_custom.h"
/* Preserve the actual product configuration; use the native allocator only. */
#undef LV_USE_STDLIB_MALLOC
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define CONFIG_LV_USE_HPDMA 0
#define CONFIG_VFS 0
