/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "tp_cst7xx.c"

static uint8_t packet[15];
static uint8_t chip = 0x13;
static bool fail_read, fail_write;
uint32_t rtos_get_time(void) { return 123; }
static int fake_read(uint8_t address, uint8_t reg, uint8_t *data, uint16_t len) {
    assert(address == 0x15);
    if (fail_read) return BK_FAIL;
    if (reg == 0xa6) {
        assert(len == 6);
        memset(data, 0, len);
        data[4] = chip;
    } else {
        assert(reg == 0 && len == sizeof(packet));
        memcpy(data, packet, len);
    }
    return BK_OK;
}
static int fake_write(uint8_t address, uint8_t reg, uint8_t *data, uint16_t len) {
    assert(address == 0x15 && reg == 0xfe && len == 1 && data[0] == 0);
    return fail_write ? BK_FAIL : BK_OK;
}
static const tp_i2c_callback_t bus = {.read_uint8 = fake_read, .write_uint8 = fake_write};
static void contact(unsigned index, uint8_t id, uint8_t event, uint16_t x, uint16_t y) {
    assert(index < 2);
    uint8_t *p = packet + 3 + index * 6;
    p[0] = (uint8_t)((event << 6) | ((x >> 8) & 15));
    p[1] = (uint8_t)x;
    p[2] = (uint8_t)((id << 4) | ((y >> 8) & 15));
    p[3] = (uint8_t)y;
}
int main(void) {
    tp_sensor_user_config_t config = {.x_size = 480, .y_size = 640, .tp_num = 2};
    assert(TP_SUPPORT_MAX_NUM == 2);
    assert(!cst7xx_detect(NULL));
    assert(cst7xx_detect(&bus));
    chip = 0x12;
    assert(!cst7xx_detect(&bus));
    chip = 0x13;
    fail_read = true;
    assert(!cst7xx_detect(&bus));
    fail_read = false;
    assert(cst7xx_init(&bus, &config) == BK_OK);
    fail_write = true;
    assert(cst7xx_init(&bus, &config) != BK_OK);
    fail_write = false;
    assert(cst7xx_init(&bus, &config) == BK_OK);
    tp_data_t data[2] = {0};
    packet[2] = 1;
    contact(0, 0, 0, 12, 34);
    assert(cst7xx_read_tp_info(&bus, 2, (uint8_t *)data) == BK_OK);
    assert(data[0].event == TP_EVENT_TYPE_DOWN && data[0].x_coordinate == 12);
    contact(0, 0, 1, 120, 340); /* final position without an intervening MOVE */
    memset(data, 0, sizeof(data));
    assert(cst7xx_read_tp_info(&bus, 2, (uint8_t *)data) == BK_OK);
    assert(data[0].event == TP_EVENT_TYPE_UP && data[0].x_coordinate == 120 &&
           data[0].y_coordinate == 340);
    packet[2] = 2;
    contact(0, 0, 0, 479, 639);
    contact(1, 1, 0, 300, 400);
    memset(data, 0, sizeof(data));
    assert(cst7xx_read_tp_info(&bus, 2, (uint8_t *)data) == BK_OK);
    assert(data[0].event == TP_EVENT_TYPE_DOWN && data[1].event == TP_EVENT_TYPE_DOWN);
    assert(data[0].track_id == 0 && data[1].track_id == 1);
    const uint16_t previous_x = s_last_x[0];
    contact(0, 0, 2, 10, 20);
    contact(1, 0, 2, 30, 40); /* duplicate second contact invalidates whole frame */
    assert(cst7xx_read_tp_info(&bus, 2, (uint8_t *)data) != BK_OK);
    assert(s_last_x[0] == previous_x && s_tp_down[0] && s_tp_down[1]);
    contact(1, 1, 3, 30, 40);
    assert(cst7xx_read_tp_info(&bus, 2, (uint8_t *)data) != BK_OK);
    packet[2] = 3;
    assert(cst7xx_read_tp_info(&bus, 2, (uint8_t *)data) != BK_OK);
    packet[2] = 0;
    memset(data, 0, sizeof(data));
    assert(cst7xx_read_tp_info(&bus, 2, (uint8_t *)data) == BK_OK);
    assert(data[0].event == TP_EVENT_TYPE_UP && data[1].event == TP_EVENT_TYPE_UP);
    fail_read = true;
    assert(cst7xx_read_tp_info(&bus, 2, (uint8_t *)data) != BK_OK);
    assert(cst7xx_read_tp_info(&bus, 0, (uint8_t *)data) != BK_OK);
    puts("CST7XX: chip identity, I2C errors, dual contacts, final UP and atomic frame validation passed");
    return 0;
}
