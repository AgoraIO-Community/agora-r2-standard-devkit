/* SPDX-License-Identifier: Apache-2.0 */
#include "bk7259_touch.h"
#include "bk7259_platform_log.h"

#include <driver/tp.h>
#include <os/os.h>
#include <stdint.h>
#include <string.h>

#define TAG "mybot_touch"
#define TOUCH_RAW_WIDTH 480U
#define TOUCH_RAW_HEIGHT 640U
#define TOUCH_QUEUE_CAPACITY 16U
#define TOUCH_POLL_MS 20U
#define TOUCH_READ_FAILURE_LIMIT 5U

typedef struct {
    lv_point_t point;
    lv_indev_state_t state;
} touch_event_t;

static struct {
    /* The TP task is the producer; only the UI task consumes. No IRQ takes this
     * lock. The one mutex and fixed queue live for the process lifetime. */
    beken_mutex_t lock;
    touch_event_t events[TOUCH_QUEUE_CAPACITY];
    unsigned head;
    unsigned count;
    uint8_t active_ids;
    bool accepting;
    bool ready;
    bool waiting_idle;
    bool idle_valid;
    bool cancel_pending;
    bool callback_registered;
    bool driver_owned;
    unsigned read_failures;
    lv_point_t reported_point;
    lv_point_t latest_point;
    lv_indev_state_t reported_state;
    unsigned read_budget;
    lv_display_t *display;
    lv_indev_t *indev;
    uint32_t available;
} s_touch;

static void unlock_touch(void)
{
    if (rtos_unlock_mutex(&s_touch.lock) != BK_OK) {
        MYBOT_LOGE(TAG, "touch mutex unlock failed");
    }
}

/* Caller holds the queue lock. Invalidation is not a normal pointer release:
 * the UI resets LVGL's active object before emitting REL, preventing CLICKED. */
static void cancel_gesture(void)
{
    s_touch.head = 0;
    s_touch.count = 0;
    s_touch.cancel_pending = true;
    s_touch.waiting_idle = true;
    s_touch.idle_valid = false;
}

static void receive_frame(const tp_data_t *points, uint8_t count, void *arg)
{
    (void)arg;
    if (rtos_lock_mutex(&s_touch.lock) != BK_OK) {
        __atomic_store_n(&s_touch.available, 0, __ATOMIC_RELEASE);
        return;
    }
    if (!s_touch.accepting) {
        unlock_touch();
        return;
    }
    if (!points || count != 2) {
        cancel_gesture();
        if (s_touch.read_failures < TOUCH_READ_FAILURE_LIMIT) {
            s_touch.read_failures++;
        }
        if (s_touch.read_failures == TOUCH_READ_FAILURE_LIMIT &&
            __atomic_exchange_n(&s_touch.available, 0, __ATOMIC_ACQ_REL)) {
            MYBOT_LOGW(TAG, "touch unavailable after consecutive read failures");
        }
        unlock_touch();
        return;
    }

    const uint8_t previous_ids = s_touch.active_ids;
    uint8_t active_ids = previous_ids;
    bool valid = true;
    lv_point_t position = s_touch.latest_point;
    for (uint8_t i = 0; i < count; ++i) {
        const tp_data_t *point = &points[i];
        if (point->event == TP_EVENT_TYPE_NONE) {
            continue;
        }
        if (point->track_id >= 2 ||
            (point->event != TP_EVENT_TYPE_DOWN && point->event != TP_EVENT_TYPE_MOVE &&
             point->event != TP_EVENT_TYPE_UP)) {
            valid = false;
            continue;
        }
        const uint8_t bit = (uint8_t)(1U << point->track_id);
        if (point->x_coordinate >= TOUCH_RAW_WIDTH ||
            point->y_coordinate >= TOUCH_RAW_HEIGHT) {
            valid = false;
        } else {
            /* Include explicit UP coordinates: a quick drag can omit MOVE. */
            position.x = (int32_t)(TOUCH_RAW_HEIGHT - 1U - point->y_coordinate);
            position.y = point->x_coordinate;
        }
        if (point->event == TP_EVENT_TYPE_UP) {
            active_ids &= (uint8_t)~bit;
        } else {
            active_ids |= bit;
        }
    }
    s_touch.active_ids = active_ids;
    if (!valid || active_ids == 3 ||
        (previous_ids && active_ids && previous_ids != active_ids)) {
        cancel_gesture();
        unlock_touch();
        return;
    }
    s_touch.idle_valid = active_ids == 0;
    s_touch.read_failures = 0;
    if (!bk7259_touch_available() && s_touch.ready && s_touch.idle_valid) {
        cancel_gesture();
        s_touch.idle_valid = true;
        __atomic_store_n(&s_touch.available, 1, __ATOMIC_RELEASE);
        MYBOT_LOGI(TAG, "touch recovered at idle; waiting for a fresh tap");
    }
    if (s_touch.waiting_idle) {
        if (s_touch.idle_valid && !s_touch.cancel_pending) {
            s_touch.waiting_idle = false;
        }
        unlock_touch();
        return;
    }
    if (!active_ids && !previous_ids) {
        unlock_touch();
        return;
    }
    s_touch.latest_point = position;
    if (s_touch.count == TOUCH_QUEUE_CAPACITY) {
        cancel_gesture();
        s_touch.idle_valid = active_ids == 0;
    } else {
        const unsigned tail = (s_touch.head + s_touch.count) % TOUCH_QUEUE_CAPACITY;
        s_touch.events[tail] = (touch_event_t){
            .point = position,
            .state = active_ids ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED,
        };
        s_touch.count++;
    }
    unlock_touch();
}

static void read_touch(lv_indev_t *indev, lv_indev_data_t *data)
{
    bool cancel = false;
    data->continue_reading = false;
    if (rtos_lock_mutex(&s_touch.lock) != BK_OK) {
        cancel = true;
        __atomic_store_n(&s_touch.available, 0, __ATOMIC_RELEASE);
    } else {
        cancel = s_touch.cancel_pending || !s_touch.accepting || !bk7259_touch_available();
        if (cancel) {
            s_touch.cancel_pending = false;
            s_touch.head = 0;
            s_touch.count = 0;
            if (s_touch.idle_valid) {
                s_touch.waiting_idle = false;
            }
        } else if (s_touch.count) {
            const touch_event_t event = s_touch.events[s_touch.head];
            s_touch.head = (s_touch.head + 1U) % TOUCH_QUEUE_CAPACITY;
            s_touch.count--;
            s_touch.reported_point = event.point;
            s_touch.reported_state = event.state;
            /* Bound one LVGL read burst even if the producer keeps posting. */
            if (--s_touch.read_budget && s_touch.count) {
                data->continue_reading = true;
            }
        }
        if (!data->continue_reading) {
            s_touch.read_budget = TOUCH_QUEUE_CAPACITY;
        }
        unlock_touch();
    }
    if (cancel) {
        lv_indev_reset(indev, NULL);
        lv_indev_wait_release(indev);
        s_touch.reported_state = LV_INDEV_STATE_RELEASED;
    }
    data->point = s_touch.reported_point;
    data->state = s_touch.reported_state;
}

bool bk7259_touch_available(void)
{
    return __atomic_load_n(&s_touch.available, __ATOMIC_ACQUIRE) != 0;
}

int bk7259_touch_shutdown(void)
{
    __atomic_store_n(&s_touch.available, 0, __ATOMIC_RELEASE);
    if (s_touch.lock) {
        if (rtos_lock_mutex(&s_touch.lock) != BK_OK) {
            return -1;
        }
        s_touch.accepting = false;
        s_touch.ready = false;
        cancel_gesture();
        unlock_touch();
    }
    if (s_touch.indev) {
        lv_indev_reset(s_touch.indev, NULL);
        lv_indev_delete(s_touch.indev);
        s_touch.indev = NULL;
    }
    /* A failed init can leave a cooperative native shutdown pending. Retain
     * its callback and our static mailbox until its owner confirms exit. */
    if (s_touch.driver_owned) {
        if (bk_tp_driver_deinit() != BK_OK) {
            MYBOT_LOGW(TAG, "touch stop pending; retaining driver ownership");
            return -1;
        }
        s_touch.driver_owned = false;
    }
    if (s_touch.callback_registered) {
        if (bk_tp_driver_set_callback(NULL, NULL) != BK_OK) {
            return -1;
        }
        s_touch.callback_registered = false;
    }
    s_touch.display = NULL;
    return 0;
}

int bk7259_touch_prepare(lv_display_t *display)
{
    if (!display || lv_display_get_horizontal_resolution(display) != TOUCH_RAW_HEIGHT ||
        lv_display_get_vertical_resolution(display) != TOUCH_RAW_WIDTH) {
        return -1;
    }
    if (bk7259_touch_available()) {
        return s_touch.display == display ? 0 : -1;
    }
    if ((s_touch.driver_owned || s_touch.callback_registered || s_touch.indev) &&
        bk7259_touch_shutdown() < 0) {
        return -1;
    }
    if (!s_touch.lock && rtos_init_mutex(&s_touch.lock) != BK_OK) {
        return -1;
    }
    if (rtos_lock_mutex(&s_touch.lock) != BK_OK) {
        return -1;
    }
    s_touch.head = 0;
    s_touch.count = 0;
    s_touch.active_ids = 0;
    s_touch.read_failures = 0;
    s_touch.ready = false;
    s_touch.waiting_idle = true;
    s_touch.idle_valid = false;
    s_touch.cancel_pending = false;
    s_touch.reported_point = (lv_point_t){0};
    s_touch.latest_point = (lv_point_t){0};
    s_touch.reported_state = LV_INDEV_STATE_RELEASED;
    s_touch.read_budget = TOUCH_QUEUE_CAPACITY;
    s_touch.accepting = true;
    unlock_touch();
    if (bk_tp_driver_set_callback(receive_frame, NULL) != BK_OK) {
        goto fail;
    }
    s_touch.callback_registered = true;
    tp_config_t config = {
        .ppi = (TOUCH_RAW_WIDTH << 16) | TOUCH_RAW_HEIGHT,
        .int_type = TP_INT_TYPE_FALLING_EDGE,
        .refresh_rate = TOUCH_POLL_MS,
        .tp_num = 2,
    };
    s_touch.driver_owned = true;
    if (bk_tp_driver_init(&config) != BK_OK) {
        MYBOT_LOGW(TAG, "CST7XX touch unavailable; display remains usable");
        goto fail;
    }
    s_touch.indev = lv_indev_create();
    if (!s_touch.indev) {
        goto fail;
    }
    s_touch.display = display;
    lv_indev_set_type(s_touch.indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_touch.indev, display);
    lv_indev_set_read_cb(s_touch.indev, read_touch);
    lv_timer_t *timer = lv_indev_get_read_timer(s_touch.indev);
    if (!timer) {
        goto fail;
    }
    lv_timer_set_period(timer, TOUCH_POLL_MS);
    if (rtos_lock_mutex(&s_touch.lock) != BK_OK) {
        goto fail;
    }
    s_touch.ready = true;
    __atomic_store_n(&s_touch.available, 1, __ATOMIC_RELEASE);
    unlock_touch();
    MYBOT_LOGI(TAG, "CST7XX touch ready: raw=480x640 logical=640x480 poll=20ms queue=16");
    return 0;

fail:
    if (bk7259_touch_shutdown() < 0) {
        MYBOT_LOGW(TAG, "touch preparation rollback requires shutdown retry");
    }
    return -1;
}
