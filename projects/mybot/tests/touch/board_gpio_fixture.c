/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stddef.h>
#include <string.h>

/* run.sh deliberately selects the pinned BK7259 headers before test GPIO
 * stubs. Thus this compiles the actual gpio_default_map_t bitfield layout,
 * vendor enum values and product GPIO_DEFAULT_DEV_CONFIG initializer. */
#include <gpio_driver.h>
#include <usr_gpio_cfg.h>
#include "board_gpio_fixture.h"

static const gpio_default_map_t product_map[] = GPIO_DEFAULT_DEV_CONFIG;
static gpio_default_map_t active_map[sizeof(product_map) / sizeof(product_map[0])];

static gpio_default_map_t *find_row(unsigned pin)
{
    for (size_t i = 0; i < sizeof(active_map) / sizeof(active_map[0]); ++i) {
        if (active_map[i].gpio_id == pin) {
            return &active_map[i];
        }
    }
    return NULL;
}

void board_gpio_restore(void)
{
    memcpy(active_map, product_map, sizeof(active_map));
}

static gpio_dev_t direction_device(int mode)
{
    switch (mode) {
    case GPIO_OUTPUT_ENABLE:
        return GPIO_DEV_GPIO_OUTPUT;
    case GPIO_INPUT_ENABLE:
        return GPIO_DEV_GPIO_INPUT;
    default:
        return GPIO_DEV_INVALID;
    }
}

bool board_gpio_mode_allowed(unsigned pin, int mode)
{
    const gpio_default_map_t *row = find_row(pin);
    const gpio_dev_t direction = direction_device(mode);
    /* The same permission rule as gpio/v2px/gpio_driver_base.c's
     * bk_gpio_enable_output/input: either declared function grants access.
     * Runtime GPIO configuration cannot grant an undeclared direction. */
    return row && direction != GPIO_DEV_INVALID &&
           (row->second_func_dev == (unsigned)direction ||
            row->time_sharing_func_dev == (unsigned)direction);
}

bool board_gpio_remove_capability(unsigned pin, int mode)
{
    gpio_default_map_t *row = find_row(pin);
    const gpio_dev_t direction = direction_device(mode);
    if (!row || direction == GPIO_DEV_INVALID) {
        return false;
    }
    bool changed = false;
    if (row->second_func_dev == (unsigned)direction) {
        row->second_func_dev = GPIO_DEV_INVALID;
        changed = true;
    }
    if (row->time_sharing_func_dev == (unsigned)direction) {
        row->time_sharing_func_dev = GPIO_DEV_INVALID;
        changed = true;
    }
    return changed;
}

bool board_gpio_soft_i2c_allowed(unsigned sda_pin, unsigned scl_pin)
{
    /* Shared camera/TP software I2C drives SCL and alternates SDA between
     * output and input for address/data/ACK. Check the real declared abilities. */
    return board_gpio_mode_allowed(scl_pin, GPIO_OUTPUT_ENABLE) &&
           board_gpio_mode_allowed(sda_pin, GPIO_OUTPUT_ENABLE) &&
           board_gpio_mode_allowed(sda_pin, GPIO_INPUT_ENABLE);
}
