/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>

/* This translation unit uses the real vendor GPIO enum/row declarations and
 * project configuration, independently of the TP harness's lightweight stubs. */
void board_gpio_restore(void);
bool board_gpio_mode_allowed(unsigned pin, int mode);
bool board_gpio_remove_capability(unsigned pin, int mode);
bool board_gpio_soft_i2c_allowed(unsigned sda_pin, unsigned scl_pin);
