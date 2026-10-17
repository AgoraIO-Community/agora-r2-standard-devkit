/* SPDX-License-Identifier: Apache-2.0 */
#include "bk7259_ops.h"
#include "bk7259_factory_reset.h"
#include "bk7259_key.h"

#include <api/aosl_atomic.h>
#include <common/bk_err.h>
#include <driver/gpio.h>
#include "bk7259_platform_log.h"
#include <os/os.h>

#include <stdbool.h>
#include <stdint.h>

#define TAG "mybot_key"
#define KEY_SCAN_PERIOD_MS 20U
#define KEY_DEBOUNCE_TICKS 2U
#define KEY_FACTORY_RESET_HOLD_MS 5000U

/* The Yunqu V1.0 board exposes one direct GPIO key. KEY1 is connected to
 * GPIO47 with an internal pull-up and is active low. A short release requests
 * a conversation toggle; holding it for five seconds requests factory reset. */
typedef struct {
    gpio_id_t gpio;
    uint8_t active_level;
} key_item_t;

typedef struct {
    beken_mutex_t lock;
    beken_semaphore_t callback_idle;
    beken_timer_t scan_timer;
    mybot_key_event_handler_t emit;
    void *user_data;
    unsigned int in_flight;
    uint8_t raw_pressed;
    uint8_t stable_pressed;
    uint8_t debounce_ticks;
    uint32_t raw_changed_ms;
    uint32_t pressed_at_ms;
    bool gesture_armed;
    bool press_active;
    bool long_press_fired;
    bool prepared;
    bool scan_timer_owned;
    bool sdk_attached;
    bool detaching;
} key_state_t;

static key_state_t s_key;
static aosl_atomic_t s_factory_reset_requested;
static aosl_atomic_t s_conversation_requested;
static aosl_atomic_t s_short_press_enabled;

static const key_item_t s_key_item = {
    .gpio = (gpio_id_t)CONFIG_MYBOT_KEY_GPIO,
    .active_level = (uint8_t)CONFIG_MYBOT_KEY_ACTIVE_LEVEL,
};

static void emit_key_event(mybot_key_event_t event) {
    mybot_key_event_handler_t emit;
    void *user_data;

    rtos_lock_mutex(&s_key.lock);
    if (!s_key.sdk_attached || s_key.detaching) {
        MYBOT_LOGW(TAG, "drop key event=%d: sdk_attached=%u detaching=%u",
                   (int)event, s_key.sdk_attached ? 1U : 0U,
                   s_key.detaching ? 1U : 0U);
        rtos_unlock_mutex(&s_key.lock);
        return;
    }
    s_key.in_flight++;
    emit = s_key.emit;
    user_data = s_key.user_data;
    rtos_unlock_mutex(&s_key.lock);

    emit(event, user_data);

    rtos_lock_mutex(&s_key.lock);
    s_key.in_flight--;
    if (s_key.detaching && s_key.in_flight == 0U) {
        (void)rtos_set_semaphore(&s_key.callback_idle);
    }
    rtos_unlock_mutex(&s_key.lock);
}

void bk7259_key_dispatch_conversation(bool start) {
    MYBOT_LOGI(TAG, "conversation key short press: %s", start ? "start" : "stop");
    emit_key_event(start ? MYBOT_KEY_EVENT_CONVERSATION_START
                         : MYBOT_KEY_EVENT_CONVERSATION_STOP);
}

bool bk7259_factory_reset_requested(void) {
    return aosl_atomic_read(&s_factory_reset_requested) != 0;
}

bool bk7259_factory_reset_take_request(void) {
    return aosl_atomic_xchg(&s_factory_reset_requested, 0) != 0;
}

bool bk7259_key_take_conversation_request(void) {
    return aosl_atomic_xchg(&s_conversation_requested, 0) != 0;
}

static void key_request_factory_reset(key_state_t *key) {
    key->long_press_fired = true;
    aosl_atomic_set(&s_conversation_requested, 0);
    aosl_atomic_set(&s_factory_reset_requested, 1);
    MYBOT_LOGW(TAG, "conversation key held five seconds: factory reset requested");
}

static void key_scan_timer(void *arg) {
    key_state_t *key = arg;
    bool pressed;
    uint8_t level;

    if (key != &s_key) {
        MYBOT_LOGW(TAG, "scan timer received unexpected context=%p", arg);
        return;
    }

    level = bk_gpio_get_input(s_key_item.gpio) ? 1U : 0U;
    pressed = level == s_key_item.active_level;
    uint32_t now_ms = rtos_get_time();

    if ((uint8_t)pressed != key->raw_pressed) {
        key->raw_pressed = (uint8_t)pressed;
        key->debounce_ticks = 1U;
        key->raw_changed_ms = now_ms;
    } else if (key->debounce_ticks > 0U &&
               key->debounce_ticks < KEY_DEBOUNCE_TICKS) {
        key->debounce_ticks++;
    }

    if (key->debounce_ticks >= KEY_DEBOUNCE_TICKS &&
        key->stable_pressed != key->raw_pressed) {
        key->debounce_ticks = 0U;
        key->stable_pressed = key->raw_pressed;
        if (key->stable_pressed) {
            if (key->gesture_armed) {
                key->press_active = true;
                key->long_press_fired = false;
                key->pressed_at_ms = key->raw_changed_ms;
            }
        } else {
            if (key->press_active && !key->long_press_fired) {
                /* Use the first observed release time, not the later debounce
                 * completion, so release debounce cannot extend the hold. */
                if ((uint32_t)(key->raw_changed_ms - key->pressed_at_ms) >=
                    KEY_FACTORY_RESET_HOLD_MS) {
                    key_request_factory_reset(key);
                } else if (aosl_atomic_read(&s_short_press_enabled) &&
                           !bk7259_factory_reset_requested()) {
                    aosl_atomic_set(&s_conversation_requested, 1);
                }
            }
            key->press_active = false;
            /* A key held across boot must be released before a new gesture. */
            key->gesture_armed = true;
        }
    }
    if (key->press_active && key->stable_pressed && key->raw_pressed &&
        !key->long_press_fired &&
        (uint32_t)(now_ms - key->pressed_at_ms) >= KEY_FACTORY_RESET_HOLD_MS) {
        key_request_factory_reset(key);
    }
}

static void key_detach(void) {
    bool wait_for_callback;

    aosl_atomic_set(&s_short_press_enabled, 0);
    aosl_atomic_set(&s_conversation_requested, 0);
    rtos_lock_mutex(&s_key.lock);
    if (!s_key.sdk_attached) {
        rtos_unlock_mutex(&s_key.lock);
        return;
    }
    s_key.sdk_attached = false;
    s_key.detaching = true;
    s_key.emit = NULL;
    s_key.user_data = NULL;
    wait_for_callback = s_key.in_flight != 0U;
    rtos_unlock_mutex(&s_key.lock);

    if (wait_for_callback) {
        (void)rtos_get_semaphore(&s_key.callback_idle, BEKEN_WAIT_FOREVER);
    }

    rtos_lock_mutex(&s_key.lock);
    s_key.detaching = false;
    rtos_unlock_mutex(&s_key.lock);
    MYBOT_LOGI(TAG, "key callbacks detached");
}

static void key_release_framework(void) {
    if (s_key.scan_timer_owned) {
        if (rtos_stop_timer(&s_key.scan_timer) != BK_OK) {
            MYBOT_LOGW(TAG, "key scan timer stop failed");
        }
        if (rtos_deinit_timer(&s_key.scan_timer) != BK_OK) {
            MYBOT_LOGW(TAG, "key scan timer deinit failed");
        }
        s_key.scan_timer_owned = false;
    }
    rtos_deinit_semaphore(&s_key.callback_idle);
    rtos_deinit_mutex(&s_key.lock);
}

int bk7259_key_prepare(void) {
    if (s_key.prepared) {
        return 0;
    }
    if (rtos_init_mutex(&s_key.lock) != BK_OK) {
        MYBOT_LOGE(TAG, "key mutex initialization failed");
        return -1;
    }
    if (rtos_init_semaphore(&s_key.callback_idle, 1) != BK_OK) {
        MYBOT_LOGE(TAG, "key callback barrier initialization failed");
        rtos_deinit_mutex(&s_key.lock);
        return -1;
    }

    if (bk_gpio_disable_output(s_key_item.gpio) != BK_OK ||
        bk_gpio_enable_input(s_key_item.gpio) != BK_OK ||
        bk_gpio_enable_pull(s_key_item.gpio) != BK_OK ||
        (s_key_item.active_level == 0U ? bk_gpio_pull_up(s_key_item.gpio)
                                       : bk_gpio_pull_down(s_key_item.gpio)) != BK_OK) {
        MYBOT_LOGE(TAG, "key GPIO configuration failed: gpio=%u active=%u",
                   (unsigned)s_key_item.gpio, (unsigned)s_key_item.active_level);
        key_release_framework();
        return -1;
    }

    s_key.raw_pressed = bk_gpio_get_input(s_key_item.gpio) == s_key_item.active_level;
    s_key.stable_pressed = s_key.raw_pressed;
    s_key.gesture_armed = !s_key.raw_pressed;
    s_key.press_active = false;
    s_key.long_press_fired = false;
    s_key.raw_changed_ms = rtos_get_time();
    aosl_atomic_set(&s_factory_reset_requested, 0);
    aosl_atomic_set(&s_conversation_requested, 0);
    aosl_atomic_set(&s_short_press_enabled, 0);
    s_key.prepared = true;

    if (rtos_init_timer(&s_key.scan_timer, KEY_SCAN_PERIOD_MS,
                        key_scan_timer, &s_key) != BK_OK) {
        MYBOT_LOGE(TAG, "key scan timer initialization failed");
        s_key.prepared = false;
        key_release_framework();
        return -1;
    }
    s_key.scan_timer_owned = true;
    if (rtos_start_timer(&s_key.scan_timer) != BK_OK) {
        MYBOT_LOGE(TAG, "key scan timer start failed");
        s_key.prepared = false;
        key_release_framework();
        return -1;
    }

    MYBOT_LOGI(TAG,
               "key ready: short release toggles conversation, hold=5000 ms resets, gpio=%u active=%u initial_level=%u",
               (unsigned)s_key_item.gpio, (unsigned)s_key_item.active_level,
               (unsigned)bk_gpio_get_input(s_key_item.gpio));
    return 0;
}

void bk7259_key_shutdown(void) {
    if (!s_key.prepared) {
        return;
    }

    key_detach();
    rtos_lock_mutex(&s_key.lock);
    s_key.prepared = false;
    rtos_unlock_mutex(&s_key.lock);
    key_release_framework();
    MYBOT_LOGI(TAG, "key stopped");
}

static int key_init(void **out_ctx, mybot_key_event_handler_t emit, void *user_data) {
    if (!out_ctx || !emit || !s_key.prepared) {
        MYBOT_LOGE(TAG, "key init rejected: out_ctx=%p emit=%p prepared=%u",
                   out_ctx, emit, s_key.prepared ? 1U : 0U);
        return -1;
    }
    *out_ctx = NULL;

    rtos_lock_mutex(&s_key.lock);
    if (s_key.sdk_attached || s_key.detaching) {
        rtos_unlock_mutex(&s_key.lock);
        MYBOT_LOGE(TAG, "key init rejected: sdk_attached=%u detaching=%u",
                   s_key.sdk_attached ? 1U : 0U, s_key.detaching ? 1U : 0U);
        return -1;
    }
    s_key.emit = emit;
    s_key.user_data = user_data;
    *out_ctx = &s_key;
    s_key.sdk_attached = true;
    aosl_atomic_set(&s_short_press_enabled, 1);
    rtos_unlock_mutex(&s_key.lock);
    MYBOT_LOGI(TAG, "key callbacks attached emit=%p user_data=%p",
               emit, user_data);
    return 0;
}

static void key_destroy(void *opaque) {
    if (opaque == &s_key && s_key.prepared) {
        key_detach();
    }
}

const mybot_key_ops_t g_mybot_bk7259_key_ops = {
    .init = key_init,
    .destroy = key_destroy,
};
