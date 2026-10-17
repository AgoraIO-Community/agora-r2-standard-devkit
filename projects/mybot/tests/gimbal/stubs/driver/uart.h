/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <common/bk_err.h>
#include <stdbool.h>
typedef enum { UART_ID_2 = 2 } uart_id_t;
enum { UART_DATA_8_BITS = 3, UART_PARITY_NONE = 0, UART_STOP_BITS_1 = 0,
       UART_FLOWCTRL_DISABLE = 0, UART_SCLK_XTAL_26M = 1, UART_DMA_DISABLE = 0 };
typedef struct {
    uint32_t baud_rate;
    int data_bits, parity, stop_bits, flow_ctrl, src_clk, rx_dma_en, tx_dma_en;
} uart_config_t;
bk_err_t bk_uart_init(uart_id_t id, const uart_config_t *config);
bk_err_t bk_uart_deinit(uart_id_t id);
bk_err_t bk_uart_register_rx_isr(uart_id_t id, void (*isr)(uart_id_t, void *), void *arg);
bk_err_t bk_uart_enable_rx_interrupt(uart_id_t id);
bool bk_uart_is_tx_over(uart_id_t id);
bk_err_t bk_uart_write_bytes(uart_id_t id, const void *data, uint32_t size);
int bk_uart_read_bytes(uart_id_t id, void *data, uint32_t size, uint32_t timeout_ms);
