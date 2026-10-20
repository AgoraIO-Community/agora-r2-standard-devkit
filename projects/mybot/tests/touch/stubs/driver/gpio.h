/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <common/bk_err.h>
typedef int gpio_id_t;
typedef int gpio_int_type_t;
typedef struct { int io_mode; int pull_mode; } gpio_config_t;
#define GPIO_OUTPUT_ENABLE 1
#define GPIO_INPUT_ENABLE 2
#define GPIO_PULL_DISABLE 0
#define GPIO_INT_TYPE_RISING_EDGE 1
#define GPIO_INT_TYPE_FALLING_EDGE 2
#define GPIO_INT_TYPE_LOW_LEVEL 3
#define GPIO_INT_TYPE_HIGH_LEVEL 4
bk_err_t bk_gpio_set_config(gpio_id_t pin, const gpio_config_t *config);
bk_err_t bk_gpio_set_output_high(gpio_id_t pin);
bk_err_t bk_gpio_set_output_low(gpio_id_t pin);
bk_err_t bk_gpio_disable_interrupt(gpio_id_t pin);
bk_err_t bk_gpio_enable_interrupt(gpio_id_t pin);
bk_err_t bk_gpio_register_isr(gpio_id_t pin, void (*handler)(gpio_id_t));
bk_err_t bk_gpio_set_interrupt_type(gpio_id_t pin, gpio_int_type_t type);
