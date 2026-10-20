/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bk7259_touch.c"

static tp_event_handler_t handler;
static void *handler_arg;
static lv_indev_t *device;
static unsigned init_calls, stop_calls, deletes;
static int fail_register, fail_driver, fail_stop, fail_indev, fail_timer;
static bool held_on_init;

bk_err_t rtos_init_mutex(beken_mutex_t *mutex) {
    pthread_mutex_t *native = malloc(sizeof(*native));
    assert(native && pthread_mutex_init(native, NULL) == 0);
    *mutex = native;
    return BK_OK;
}
bk_err_t rtos_lock_mutex(beken_mutex_t *mutex) {
    return pthread_mutex_lock(*mutex) == 0 ? BK_OK : BK_FAIL;
}
bk_err_t rtos_unlock_mutex(beken_mutex_t *mutex) {
    return pthread_mutex_unlock(*mutex) == 0 ? BK_OK : BK_FAIL;
}
unsigned lv_display_get_horizontal_resolution(lv_display_t *display) { return display->width; }
unsigned lv_display_get_vertical_resolution(lv_display_t *display) { return display->height; }
lv_indev_t *lv_indev_create(void) {
    if (fail_indev) return NULL;
    device = calloc(1, sizeof(*device));
    assert(device);
    return device;
}
void lv_indev_delete(lv_indev_t *indev) {
    assert(indev == device);
    free(indev);
    device = NULL;
    deletes++;
}
void lv_indev_reset(lv_indev_t *indev, void *object) { assert(!object); indev->resets++; }
void lv_indev_wait_release(lv_indev_t *indev) { indev->waits++; }
void lv_indev_set_type(lv_indev_t *indev, int type) { assert(type == LV_INDEV_TYPE_POINTER); }
void lv_indev_set_display(lv_indev_t *indev, lv_display_t *display) { indev->display = display; }
void lv_indev_set_read_cb(lv_indev_t *indev, void (*callback)(lv_indev_t *, lv_indev_data_t *)) {
    indev->read_cb = callback;
}
lv_timer_t *lv_indev_get_read_timer(lv_indev_t *indev) {
    return fail_timer ? NULL : &indev->timer;
}
void lv_timer_set_period(lv_timer_t *timer, uint32_t period) { timer->period = period; }
bk_err_t bk_tp_driver_set_callback(tp_event_handler_t callback, void *arg) {
    if (fail_register && callback) return BK_FAIL;
    handler = callback;
    handler_arg = arg;
    return BK_OK;
}
bk_err_t bk_tp_driver_init(tp_config_t *config) {
    init_calls++;
    assert(config->ppi == ((480U << 16) | 640U));
    assert(config->refresh_rate == 20 && config->tp_num == 2);
    assert(config->int_type == TP_INT_TYPE_FALLING_EDGE);
    if (fail_driver) return BK_FAIL;
    const tp_data_t initial[2] = {{.event = held_on_init ? TP_EVENT_TYPE_DOWN : TP_EVENT_TYPE_NONE,
                                  .x_coordinate = 20, .y_coordinate = 40}, {0}};
    assert(handler);
    handler(initial, 2, handler_arg);
    return BK_OK;
}
bk_err_t bk_tp_driver_deinit(void) { stop_calls++; return fail_stop ? BK_FAIL : BK_OK; }

static void emit(uint8_t event, uint16_t x, uint16_t y) {
    const tp_data_t frame[2] = {{.event = event, .x_coordinate = x, .y_coordinate = y}, {0}};
    assert(handler);
    handler(frame, 2, handler_arg);
}
static lv_indev_data_t read_one(void) {
    lv_indev_data_t data = {0};
    assert(device && device->read_cb);
    device->read_cb(device, &data);
    return data;
}
static void idle(void) { emit(TP_EVENT_TYPE_NONE, 0, 0); }
static void test_first_tap_and_coordinates(lv_display_t *display) {
    assert(bk7259_touch_prepare(display) == 0 && bk7259_touch_available());
    assert(device->display == display && device->timer.period == 20);
    unsigned before_init = init_calls;
    assert(bk7259_touch_prepare(display) == 0 && init_calls == before_init);
    emit(TP_EVENT_TYPE_DOWN, 0, 0);
    lv_indev_data_t data = read_one();
    assert(data.state == LV_INDEV_STATE_PRESSED && data.point.x == 639 && data.point.y == 0);
    emit(TP_EVENT_TYPE_UP, 479, 639);
    data = read_one();
    assert(data.state == LV_INDEV_STATE_RELEASED && data.point.x == 0 && data.point.y == 479);
    assert(device->resets == 0);
    /* No MOVE frame: UP must carry a final drag position outside the button. */
    emit(TP_EVENT_TYPE_DOWN, 100, 100);
    data = read_one();
    assert(data.point.x == 539 && data.point.y == 100);
    emit(TP_EVENT_TYPE_UP, 220, 280);
    data = read_one();
    assert(data.state == LV_INDEV_STATE_RELEASED && data.point.x == 359 && data.point.y == 220);
    assert(bk7259_touch_shutdown() == 0 && !bk7259_touch_available());
    assert(bk7259_touch_shutdown() == 0);
}
static void test_cancel_and_recovery(lv_display_t *display) {
    assert(bk7259_touch_prepare(display) == 0);
    emit(TP_EVENT_TYPE_DOWN, 50, 60);
    assert(read_one().state == LV_INDEV_STATE_PRESSED);
    unsigned resets = device->resets;
    handler(NULL, 2, handler_arg);
    assert(read_one().state == LV_INDEV_STATE_RELEASED);
    assert(device->resets == resets + 1 && device->waits == device->resets);
    assert(bk7259_touch_available());
    emit(TP_EVENT_TYPE_MOVE, 51, 61);
    assert(read_one().state == LV_INDEV_STATE_RELEASED);
    emit(TP_EVENT_TYPE_UP, 51, 61);
    emit(TP_EVENT_TYPE_DOWN, 55, 65);
    assert(read_one().state == LV_INDEV_STATE_PRESSED);
    emit(TP_EVENT_TYPE_UP, 480, 65);
    assert(read_one().state == LV_INDEV_STATE_RELEASED);
    assert(device->resets == resets + 2);
    idle();
    const tp_data_t multi[2] = {{.event = TP_EVENT_TYPE_DOWN, .track_id = 0,
                                .x_coordinate = 20, .y_coordinate = 20},
                               {.event = TP_EVENT_TYPE_DOWN, .track_id = 1,
                                .x_coordinate = 30, .y_coordinate = 30}};
    handler(multi, 2, handler_arg);
    assert(read_one().state == LV_INDEV_STATE_RELEASED);
    assert(device->resets == resets + 3);
    emit(TP_EVENT_TYPE_UP, 20, 20);
    assert(read_one().state == LV_INDEV_STATE_RELEASED);
    tp_data_t released[2] = {{0}, {.event = TP_EVENT_TYPE_UP, .track_id = 1,
                                  .x_coordinate = 30, .y_coordinate = 30}};
    handler(released, 2, handler_arg);
    emit(TP_EVENT_TYPE_DOWN, 50, 60);
    assert(read_one().state == LV_INDEV_STATE_PRESSED);
    emit(TP_EVENT_TYPE_UP, 50, 60);
    assert(read_one().state == LV_INDEV_STATE_RELEASED);
    assert(bk7259_touch_shutdown() == 0);
}
static void test_overflow_and_burst(lv_display_t *display) {
    assert(bk7259_touch_prepare(display) == 0);
    emit(TP_EVENT_TYPE_DOWN, 20, 40);
    assert(read_one().state == LV_INDEV_STATE_PRESSED);
    for (unsigned i = 0; i <= TOUCH_QUEUE_CAPACITY; ++i) emit(TP_EVENT_TYPE_MOVE, 20, 40);
    assert(read_one().state == LV_INDEV_STATE_RELEASED && device->resets == 1);
    emit(TP_EVENT_TYPE_UP, 20, 40);
    emit(TP_EVENT_TYPE_DOWN, 20, 40);
    for (unsigned i = 1; i < TOUCH_QUEUE_CAPACITY; ++i) emit(TP_EVENT_TYPE_MOVE, 20, 40);
    for (unsigned i = 0; i < TOUCH_QUEUE_CAPACITY; ++i) {
        const lv_indev_data_t data = read_one();
        assert(data.state == LV_INDEV_STATE_PRESSED);
        assert(data.continue_reading == (i + 1 < TOUCH_QUEUE_CAPACITY));
    }
    assert(bk7259_touch_shutdown() == 0);
}
static void test_runtime_fault_and_idle_recovery(lv_display_t *display) {
    assert(bk7259_touch_prepare(display) == 0);
    emit(TP_EVENT_TYPE_DOWN, 40, 60);
    assert(read_one().state == LV_INDEV_STATE_PRESSED);
    for (unsigned i = 0; i < TOUCH_READ_FAILURE_LIMIT; ++i) {
        handler(NULL, 2, handler_arg);
        assert(bk7259_touch_available() == (i + 1 < TOUCH_READ_FAILURE_LIMIT));
        assert(read_one().state == LV_INDEV_STATE_RELEASED);
    }
    emit(TP_EVENT_TYPE_MOVE, 45, 65);
    assert(!bk7259_touch_available());
    emit(TP_EVENT_TYPE_UP, 45, 65);
    assert(bk7259_touch_available());
    assert(read_one().state == LV_INDEV_STATE_RELEASED);
    emit(TP_EVENT_TYPE_DOWN, 45, 65);
    assert(read_one().state == LV_INDEV_STATE_PRESSED);
    assert(bk7259_touch_shutdown() == 0);
}
static void test_failures_and_boot_hold(lv_display_t *display) {
    lv_display_t wrong = {480, 640};
    assert(bk7259_touch_prepare(NULL) < 0 && bk7259_touch_prepare(&wrong) < 0);
    int *failures[] = {&fail_register, &fail_driver, &fail_indev, &fail_timer};
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        *failures[i] = 1;
        assert(bk7259_touch_prepare(display) < 0);
        assert(!bk7259_touch_available() && !device && !handler);
        *failures[i] = 0;
        assert(bk7259_touch_shutdown() == 0);
    }
    held_on_init = true;
    assert(bk7259_touch_prepare(display) == 0);
    assert(read_one().state == LV_INDEV_STATE_RELEASED);
    emit(TP_EVENT_TYPE_UP, 20, 40);
    assert(read_one().state == LV_INDEV_STATE_RELEASED);
    emit(TP_EVENT_TYPE_DOWN, 20, 40);
    assert(read_one().state == LV_INDEV_STATE_PRESSED);
    held_on_init = false;
    fail_stop = 1;
    assert(bk7259_touch_shutdown() < 0 && !bk7259_touch_available() && !device && handler);
    emit(TP_EVENT_TYPE_DOWN, 30, 60); /* late native callback cannot use deleted LVGL */
    assert(bk7259_touch_prepare(display) < 0);
    fail_stop = 0;
    assert(bk7259_touch_shutdown() == 0 && !handler);
    fail_stop = fail_driver = 1;
    assert(bk7259_touch_prepare(display) < 0 && handler);
    fail_stop = fail_driver = 0;
    assert(bk7259_touch_shutdown() == 0 && !handler);
    for (unsigned i = 0; i < 10; ++i) {
        assert(bk7259_touch_prepare(display) == 0);
        assert(bk7259_touch_shutdown() == 0);
    }
}
int main(void) {
    lv_display_t display = {640, 480};
    test_first_tap_and_coordinates(&display);
    test_cancel_and_recovery(&display);
    test_overflow_and_burst(&display);
    test_runtime_fault_and_idle_recovery(&display);
    test_failures_and_boot_hold(&display);
    assert(!device && !handler && deletes > 10 && stop_calls >= init_calls);
    puts("BK7259 touch: first tap, final UP, cancel, multi-touch, bounds, overflow and lifecycle passed");
    return 0;
}
