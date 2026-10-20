/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct { int32_t x, y; } lv_point_t;
typedef enum { LV_INDEV_STATE_RELEASED, LV_INDEV_STATE_PRESSED } lv_indev_state_t;
typedef struct { unsigned period; } lv_timer_t;
typedef struct { unsigned width, height; } lv_display_t;
typedef struct {
    lv_point_t point;
    lv_indev_state_t state;
    bool continue_reading;
} lv_indev_data_t;
typedef struct lv_indev_t {
    void (*read_cb)(struct lv_indev_t *, lv_indev_data_t *);
    lv_display_t *display;
    lv_timer_t timer;
    unsigned resets;
    unsigned waits;
} lv_indev_t;
#define LV_INDEV_TYPE_POINTER 1
unsigned lv_display_get_horizontal_resolution(lv_display_t *display);
unsigned lv_display_get_vertical_resolution(lv_display_t *display);
lv_indev_t *lv_indev_create(void);
void lv_indev_delete(lv_indev_t *indev);
void lv_indev_reset(lv_indev_t *indev, void *object);
void lv_indev_wait_release(lv_indev_t *indev);
void lv_indev_set_type(lv_indev_t *indev, int type);
void lv_indev_set_display(lv_indev_t *indev, lv_display_t *display);
void lv_indev_set_read_cb(lv_indev_t *indev, void (*read_cb)(lv_indev_t *, lv_indev_data_t *));
lv_timer_t *lv_indev_get_read_timer(lv_indev_t *indev);
void lv_timer_set_period(lv_timer_t *timer, uint32_t period);
