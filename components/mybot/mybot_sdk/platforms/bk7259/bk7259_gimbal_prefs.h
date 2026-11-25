/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BK7259_GIMBAL_PREFS_H_
#define BK7259_GIMBAL_PREFS_H_

#include <sdkconfig.h>
#include <stdbool.h>

#define BK7259_GIMBAL_PREFS_KEY "mybot.gimbal.v1"

#if defined(CONFIG_MYBOT_GIMBAL) && CONFIG_MYBOT_GIMBAL
/* Missing records use the product defaults. An unreadable record disables
 * both features and returns -1. */
int bk7259_gimbal_prefs_load(bool *motion, bool *charge);
int bk7259_gimbal_prefs_save(bool motion, bool charge);

/* Finish any save in flight and close the writer until reboot. The product
 * reset owner must close it before clearing the durable settings. */
int bk7259_gimbal_prefs_block_writes(void);
#else
static inline int bk7259_gimbal_prefs_block_writes(void) { return 0; }
#endif

#endif /* BK7259_GIMBAL_PREFS_H_ */
