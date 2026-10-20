/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <driver/i2c.h>
typedef struct { int sda_pin, scl_pin; } sw_i2c_config_t;
typedef struct { int value; } sw_i2c_handle_t;
sw_i2c_handle_t *sw_i2c_init(const sw_i2c_config_t *config);
bk_err_t sw_i2c_deinit(sw_i2c_handle_t *handle);
bk_err_t sw_i2c_memory_read(sw_i2c_handle_t *handle, i2c_mem_param_t *param);
bk_err_t sw_i2c_memory_write(sw_i2c_handle_t *handle, i2c_mem_param_t *param);
bk_err_t sw_i2c_master_read(sw_i2c_handle_t *handle, uint32_t addr, uint8_t *data,
                           uint32_t len, uintptr_t timeout);
bk_err_t sw_i2c_master_write(sw_i2c_handle_t *handle, uint32_t addr, const uint8_t *data,
                            uint32_t len, uintptr_t timeout);
