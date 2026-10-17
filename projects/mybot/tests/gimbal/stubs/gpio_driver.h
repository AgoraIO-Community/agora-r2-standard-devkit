/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <driver/gpio.h>
bk_err_t gpio_dev_map_by_func(int function);
bk_err_t gpio_dev_unmap_by_func(int function);
