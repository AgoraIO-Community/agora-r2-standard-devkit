/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <common/bk_err.h>
#include <stdint.h>
typedef struct {
    uint32_t dev_addr, mem_addr, mem_addr_size;
    uint8_t *data;
    uint32_t data_size, timeout_ms;
} i2c_mem_param_t;
typedef struct { unsigned baud_rate, addr_mode; } i2c_config_t;
#define I2C_MEM_ADDR_SIZE_8BIT 1
#define I2C_MEM_ADDR_SIZE_16BIT 2
#define I2C_BAUD_RATE_100KHZ 100000
#define I2C_ADDR_MODE_7BIT 7
bk_err_t bk_i2c_init(int id, i2c_config_t *config);
