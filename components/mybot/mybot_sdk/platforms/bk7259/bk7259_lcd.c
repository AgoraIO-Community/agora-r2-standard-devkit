/* SPDX-License-Identifier: Apache-2.0 */
#include "bk7259_ops.h"
#include "bk7259_expression.h"
#include "bk7259_factory_reset.h"
#include "bk7259_gimbal.h"
#include "bk7259_lcd.h"
#include "bk7259_touch.h"
#include "bk7259_platform_log.h"
#include "display/expression_policy.h"
#include "display/lvgl_view.h"

#include <api/aosl_atomic.h>
#include <common/avdk_pixel_types.h>
#include <common/bk_err.h>
#include <components/bk_display.h>
#include <components/bk_frame_buffer.h>
#include <driver/gpio.h>
#include <gpio_driver.h>
#include <lcd/lcd_mipi_gc9503cv_480x640.h>
#include <modules/pm.h>
#include <os/mem.h>
#include <os/os.h>
/* Pinned LVGL 9.5 exposes handler setters publicly but defines their fields
 * here. Preserve its copy/stride/alignment callbacks and replace allocation
 * only, instead of duplicating the vendor's pixel-format copy routines. */
#include <src/draw/lv_draw_buf_private.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TAG "mybot_lcd"
#define LCD_NATIVE_WIDTH 480
#define LCD_NATIVE_HEIGHT 640
#define LCD_LOGICAL_WIDTH LCD_NATIVE_HEIGHT
#define LCD_LOGICAL_HEIGHT LCD_NATIVE_WIDTH
#define LCD_FRAME_BYTES (LCD_NATIVE_WIDTH * LCD_NATIVE_HEIGHT * 2U)
#define LCD_FRAME_COUNT 2
#define LCD_DRAW_ROWS 16
#define LCD_DRAW_BYTES (LCD_LOGICAL_WIDTH * LCD_DRAW_ROWS * 2U)
#define LCD_RENDER_TIMEOUT_MS 250U
#define LCD_STOP_TIMEOUT_MS 3000U
#define LCD_UI_STACK_BYTES (8U * 1024U)
/* Beken priorities run in reverse order: keep UI below the audio workers. */
#define LCD_UI_PRIORITY BEKEN_APPLICATION_PRIORITY
#define LCD_SSID_CAPACITY 33
#define LCD_POWER_GPIO GPIO_20
/* GPIO29 enables the shared LCD/camera 2.8V rail. Keep it on from platform
 * prepare until full shutdown; stopping video must only reset the sensor. */
#define LCD_POWER_2V8_GPIO GPIO_29
/* The replacement MIPI sensor needs the board's external 1.2V rail enabled.
 * Its GPIO27 gate follows the same platform-lifetime power ownership. */
#define CAMERA_POWER_1V2_GPIO GPIO_27
#define LCD_RESET_GPIO GPIO_42
#define LCD_BACKLIGHT_GPIO GPIO_40

/* lcd_flush consumes tightly packed RGB565 rows from the partial draw buffer. */
_Static_assert(LV_DRAW_BUF_STRIDE_ALIGN == 1, "LCD partial flush requires packed rows");

typedef struct {
    bk_display_bus_handle_t bus;
    bk_avdk_lcd_panel_handle_t panel;
    bk_display_ctlr_handle_t controller;
    uint16_t *frames[LCD_FRAME_COUNT];
    aosl_atomic_t frame_busy[LCD_FRAME_COUNT];
    aosl_atomic_t callbacks_active;
    aosl_atomic_t stopping;
    beken_semaphore_t frame_done;
    beken_semaphore_t wake;
    beken_semaphore_t worker_done;
    beken_thread_t worker;
    lv_display_t *display;
    uint8_t *draw_buffer;
    bool view_created;
    bool touch_attempted;
    int composing;
    int previous;
    bool discard_refresh;
    bool redraw_needed;
    bool flush_fault;
    bool vddio_owned;
    bool power_gpio_owned;
    bool power_2v8_gpio_owned;
    bool power_1v2_gpio_owned;
    bool backlight_gpio_owned;
    bool cpu_vote_owned;
    bool controller_inited;
    bool controller_open;
    /* Snapshot lock protects these fields; only the UI owner touches LVGL. */
    bool prepared;
    bool accepting;
    bool sdk_attached;
    bool pending;
    bool factory_reset_active;
    bk7259_factory_reset_screen_t factory_reset_screen;
    /* State edges and one-shot requests are recorded before snapshots coalesce.
     * Only the UI owner submits faces and starts their display clocks. */
    bk7259_expression_policy_t expression_policy;
    bool expression_displayed;
    bk7259_expression_t displayed_expression;
    bk7259_expression_source_t displayed_source;
    mybot_lcd_content_t latest;
    bk7259_lcd_runtime_status_t runtime_status;
    char provisioning_ssid[LCD_SSID_CAPACITY];
} lcd_context_t;

static lcd_context_t s_lcd;
/* Created by the product owner before SDK start; kept for the process lifetime.
 * Lifecycle waits never hold the snapshot lock. */
static beken_mutex_t s_lifecycle_lock;
static beken_mutex_t s_snapshot_lock;
static bool s_frame_buffer_initialized;
static bool s_lvgl_initialized;
static const bk_display_dpu_config_t s_controller_config = {
    .video = {.enable = true, .decompress = false, .format = BK_PIXEL_FORMAT_RGB565},
};

static void unlock(beken_mutex_t *lock)
{
    if (rtos_unlock_mutex(lock) != BK_OK) {
        MYBOT_LOGE(TAG, "mutex unlock failed");
    }
}

/* Both notifications are coalescing: with a valid semaphore, BK's only give
 * failure means its counter is already full. State/ownership lives elsewhere. */
static void notify(beken_semaphore_t *event)
{
    if (rtos_set_semaphore(event) != BK_OK) {
        /* An earlier notification is pending; the bounded wait also rechecks state. */
    }
}

static avdk_err_t frame_release_callback(void *frame)
{
    aosl_atomic_inc(&s_lcd.callbacks_active);
    for (unsigned int i = 0; i < LCD_FRAME_COUNT; ++i) {
        if (frame == s_lcd.frames[i]) {
            if (aosl_atomic_xchg(&s_lcd.frame_busy[i], 0)) {
                notify(&s_lcd.frame_done);
            }
            aosl_atomic_dec(&s_lcd.callbacks_active);
            return AVDK_ERR_OK;
        }
    }
    aosl_atomic_dec(&s_lcd.callbacks_active);
    return AVDK_ERR_INVAL;
}

static int free_frame(void)
{
    for (int i = 0; i < LCD_FRAME_COUNT; ++i) {
        if (!aosl_atomic_read(&s_lcd.frame_busy[i])) {
            return i;
        }
    }
    return -1;
}

static int acquire_frame(void)
{
    uint32_t start = rtos_get_time();
    while (!aosl_atomic_read(&s_lcd.stopping)) {
        int i = free_frame();
        if (i >= 0) {
            return i;
        }
        uint32_t elapsed = rtos_get_time() - start;
        if (elapsed >= LCD_RENDER_TIMEOUT_MS) {
            break;
        }
        uint32_t remaining = LCD_RENDER_TIMEOUT_MS - elapsed;
        bk_err_t ret = rtos_get_semaphore(&s_lcd.frame_done, remaining < 50U ? remaining : 50U);
        if (ret != BK_OK && ret != kTimeoutErr) {
            break;
        }
    }
    return -1;
}

static void flush_failed(const char *operation, int result)
{
    if (!s_lcd.flush_fault) {
        MYBOT_LOGE(TAG, "LVGL %s failed: %d; retaining scanout buffers", operation, result);
    }
    s_lcd.flush_fault = true;
    s_lcd.redraw_needed = true;
}

/* LVGL owns only the stripe. DPU owns submitted full frames until its callback
 * releases them. Never call LVGL from that IRQ callback. */
static void lcd_flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    bool last = lv_display_flush_is_last(display);
    if (aosl_atomic_read(&s_lcd.stopping)) {
        s_lcd.discard_refresh = true;
    }
    if (!s_lcd.discard_refresh && s_lcd.composing < 0) {
        s_lcd.composing = acquire_frame();
        if (s_lcd.composing < 0) {
            s_lcd.discard_refresh = true;
            flush_failed("frame acquire", -1);
        } else if (s_lcd.previous >= 0 && s_lcd.previous != s_lcd.composing) {
            /* The old scanout is immutable and may be read while DPU scans it.
             * Seed unchanged pixels once per refresh, not once per stripe. */
            memcpy(s_lcd.frames[s_lcd.composing], s_lcd.frames[s_lcd.previous], LCD_FRAME_BYTES);
        }
    }

    if (!s_lcd.discard_refresh) {
        uint16_t *dst = s_lcd.frames[s_lcd.composing];
        const uint16_t *src = (const uint16_t *)pixels;
        int width = lv_area_get_width(area);
        if (area->x1 < 0 || area->y1 < 0 || area->x2 >= LCD_LOGICAL_WIDTH ||
            area->y2 >= LCD_LOGICAL_HEIGHT || width <= 0 || lv_area_get_height(area) <= 0) {
            s_lcd.discard_refresh = true;
            flush_failed("invalid area", -1);
        } else {
            for (int y = area->y1; y <= area->y2; ++y) {
                for (int x = area->x1; x <= area->x2; ++x) {
                    /* Same 270-degree transform as the existing Robot V2 UI.
                     * RGB565 is native byte order; SPI byte swapping is incorrect here. */
                    dst[(LCD_LOGICAL_WIDTH - x - 1) * LCD_NATIVE_WIDTH + y] = *src++;
                }
            }
        }
    }

    if (last) {
        if (!s_lcd.discard_refresh) {
            int index = s_lcd.composing;
            /* Exactly two scanout buffers and one producer: acquiring a free
             * frame proves that no third/pending frame can block the vendor
             * flush routine's wait-for-slot path. */
            aosl_atomic_set(&s_lcd.frame_busy[index], 1);
            avdk_err_t ret = bk_display_flush(s_lcd.controller, s_lcd.frames[index],
                                              frame_release_callback);
            s_lcd.previous = index;
            if (ret != AVDK_ERR_OK) {
                /* Even an error may have promoted the new scanout. Only its
                 * release callback (or completed display deinit) proves it free. */
                flush_failed("submission", ret);
            } else if (s_lcd.flush_fault) {
                MYBOT_LOGI(TAG, "LVGL display submission recovered");
                s_lcd.flush_fault = false;
            }
        }
        s_lcd.composing = -1;
        s_lcd.discard_refresh = false;
    }
    /* All stripe bytes have been copied, so LVGL may reuse its stripe even
     * though the independently owned full frame remains in DPU use. */
    lv_display_flush_ready(display);
}

static uint32_t lcd_tick(void)
{
    return rtos_get_time();
}

static void *draw_buffer_malloc(size_t bytes, lv_color_format_t format)
{
    (void)format;
    if (bytes > SIZE_MAX - (LV_DRAW_BUF_ALIGN - 1U)) {
        return NULL;
    }
    /* Vendor buf_malloc falls back to PSRAM, but its paired lv_free uses
     * HSRAM. Keep glyph, image and layer allocations in the same heap even
     * on OOM; LVGL retains its normal allocation-failure handling. */
    return lv_malloc(bytes + LV_DRAW_BUF_ALIGN - 1U);
}

static void configure_draw_allocators(void)
{
    lv_draw_buf_get_handlers()->buf_malloc_cb = draw_buffer_malloc;
    lv_draw_buf_get_font_handlers()->buf_malloc_cb = draw_buffer_malloc;
    lv_draw_buf_get_image_handlers()->buf_malloc_cb = draw_buffer_malloc;
}

static void lvgl_log(lv_log_level_t level, const char *text)
{
    if (level >= LV_LOG_LEVEL_ERROR) {
        MYBOT_LOGE(TAG, "LVGL: %s", text);
    } else {
        MYBOT_LOGW(TAG, "LVGL: %s", text);
    }
}

static void ui_worker(void *arg)
{
    (void)arg;
    while (!aosl_atomic_read(&s_lcd.stopping)) {
        /* LVGL and its input device belong to this worker. Hardware callbacks
         * only copy touch frames; availability changes update the same view. */
        bk7259_lvgl_view_set_touch_available(bk7259_touch_available());
        /* Serialize the brief content application with SDK detach and portal
         * updates. The snapshot lock is never held during rendering/flush. */
        if (rtos_lock_mutex(&s_snapshot_lock) == BK_OK) {
            if (s_lcd.accepting) {
                if (s_lcd.factory_reset_active) {
                    if (s_lcd.pending)
                        bk7259_lvgl_view_show_factory_reset(s_lcd.factory_reset_screen);
                    s_lcd.expression_displayed = false;
                } else {
                    if (s_lcd.pending)
                        mybot_lvgl_view_update(&s_lcd.latest, s_lcd.provisioning_ssid,
                                               &s_lcd.runtime_status);
                    bk7259_expression_t expression;
                    bool replay;
#if CONFIG_MYBOT_LVGL_UI_ANIMATIONS
                    const uint32_t transition_ms = BK7259_EXPRESSION_TRANSITION_MS;
#else
                    const uint32_t transition_ms = 0;
#endif
                    /* Timed changes run even when no SDK snapshot is pending. */
                    if (bk7259_expression_policy_select(&s_lcd.expression_policy,
                                                        rtos_get_time(), transition_ms,
                                                        &expression, &replay)) {
                        int result = replay
                            ? bk7259_lvgl_view_replay_expression(expression)
                            : bk7259_lvgl_view_show_expression(expression);
                        if (result < 0) {
                            MYBOT_LOGW(TAG, "expression update rejected: %d", (int)expression);
                        } else {
                            bk7259_expression_source_t source = s_lcd.expression_policy.source;
                            bk7259_expression_policy_displayed(&s_lcd.expression_policy,
                                                               rtos_get_time());
                            if (!s_lcd.expression_displayed || replay ||
                                s_lcd.displayed_expression != expression ||
                                s_lcd.displayed_source != source) {
                                MYBOT_LOGI(TAG, "LCD expression -> %s source=%s",
                                           bk7259_expression_name(expression),
                                           bk7259_expression_source_name(source));
                            }
                            s_lcd.expression_displayed = true;
                            s_lcd.displayed_expression = expression;
                            s_lcd.displayed_source = source;
                        }
                    } else {
                        s_lcd.expression_displayed = false;
                    }
                }
                s_lcd.pending = false;
            }
            unlock(&s_snapshot_lock);
        }
        if (s_lcd.redraw_needed && free_frame() >= 0) {
            s_lcd.redraw_needed = false;
            lv_obj_invalidate(lv_display_get_screen_active(s_lcd.display));
        }
        uint32_t wait_ms = lv_timer_handler();
        if (wait_ms > 50U) {
            wait_ms = 50U;
        } else if (wait_ms < 5U) {
            wait_ms = 5U;
        }
        bk_err_t ret = rtos_get_semaphore(&s_lcd.wake, wait_ms);
        if (ret != BK_OK && ret != kTimeoutErr) {
            MYBOT_LOGE(TAG, "UI wake wait failed: %d", ret);
            break;
        }
    }
    aosl_atomic_set(&s_lcd.stopping, 1);
    /* Last display-resource access. Notification semaphores live for the
     * process lifetime, so a waking owner cannot delete this semaphore while
     * the kernel's give operation is still returning on the other core. */
    notify(&s_lcd.worker_done);
    rtos_delete_thread(NULL);
}

static int set_backlight(bool enabled)
{
    if (!s_lcd.backlight_gpio_owned) {
        return -1;
    }
    return (enabled ? bk_gpio_set_output_low(LCD_BACKLIGHT_GPIO)
                    : bk_gpio_set_output_high(LCD_BACKLIGHT_GPIO)) == BK_OK
               ? 0
               : -1;
}

static int panel_power_on(void)
{
    if (gpio_dev_unmap(LCD_BACKLIGHT_GPIO) != BK_OK ||
        gpio_dev_unmap(LCD_POWER_GPIO) != BK_OK ||
        gpio_dev_unmap(LCD_POWER_2V8_GPIO) != BK_OK ||
        gpio_dev_unmap(LCD_RESET_GPIO) != BK_OK ||
        bk_gpio_enable_output(LCD_RESET_GPIO) != BK_OK ||
        bk_gpio_set_output_low(LCD_RESET_GPIO) != BK_OK ||
        bk_gpio_enable_output(LCD_BACKLIGHT_GPIO) != BK_OK ||
        bk_gpio_pull_down(LCD_BACKLIGHT_GPIO) != BK_OK) {
        return -1;
    }
    /* The pinned BK7259 API exposes this setter as bool but its HAL returns
     * BK_OK (0) on success; keep the reference board's best-effort use. */
    (void)bk_gpio_set_capacity(LCD_BACKLIGHT_GPIO, GPIO_DRIVER_CAPACITY_3);
    if (bk_gpio_set_output_high(LCD_BACKLIGHT_GPIO) != BK_OK ||
        bk_gpio_enable_output(LCD_POWER_GPIO) != BK_OK ||
        bk_gpio_pull_up(LCD_POWER_GPIO) != BK_OK ||
        bk_gpio_set_output_high(LCD_POWER_GPIO) != BK_OK ||
        bk_gpio_enable_output(LCD_POWER_2V8_GPIO) != BK_OK ||
        bk_gpio_pull_down(LCD_POWER_2V8_GPIO) != BK_OK ||
        bk_gpio_set_output_high(LCD_POWER_2V8_GPIO) != BK_OK) {
        return -1;
    }
    s_lcd.backlight_gpio_owned = true;
    if (set_backlight(false) < 0) {
        return -1;
    }
    s_lcd.power_gpio_owned = true;
    s_lcd.power_2v8_gpio_owned = true;

    if (gpio_dev_unmap(CAMERA_POWER_1V2_GPIO) != BK_OK ||
        bk_gpio_enable_output(CAMERA_POWER_1V2_GPIO) != BK_OK) {
        MYBOT_LOGE(TAG, "camera 1.2V power GPIO setup failed");
        return -1;
    }
    /* Record ownership before enabling the rail so a later failure still
     * releases it during preparation rollback. */
    s_lcd.power_1v2_gpio_owned = true;
    if (bk_gpio_pull_up(CAMERA_POWER_1V2_GPIO) != BK_OK ||
        bk_gpio_set_output_high(CAMERA_POWER_1V2_GPIO) != BK_OK) {
        MYBOT_LOGE(TAG, "camera 1.2V power enable failed");
        return -1;
    }
    MYBOT_LOGI(TAG, "camera 1.2V power enabled: gpio=27 level=1");

    pm_auxldo_ctrl_cfg_t vddio = {
        .ldo = AUXLDOS_SEL_1P8V,
        .out = PM_AUXLDO_1P8V_OUT_1P8V,
        .user = PM_AUXLDO_USER_DISPLAY,
        .state = PM_AUXLDO_ENABLE,
    };
    if (bk_pm_auxldo_ctrl_vote(&vddio) != BK_OK) {
        return -1;
    }
    s_lcd.vddio_owned = true;
    rtos_delay_milliseconds(10);
    return 0;
}

static int panel_power_off(void)
{
    int result = 0;

    if (s_lcd.backlight_gpio_owned) {
        if (set_backlight(false) != 0) {
            MYBOT_LOGW(TAG, "failed to disable panel backlight");
            result = -1;
        } else {
            s_lcd.backlight_gpio_owned = false;
        }
    }
    if (bk_gpio_enable_output(LCD_RESET_GPIO) != BK_OK ||
        bk_gpio_set_output_low(LCD_RESET_GPIO) != BK_OK) {
        MYBOT_LOGW(TAG, "failed to assert panel reset");
        return -1;
    }
    if (s_lcd.vddio_owned) {
        pm_auxldo_ctrl_cfg_t vddio = {
            .ldo = AUXLDOS_SEL_1P8V,
            .out = PM_AUXLDO_1P8V_OUT_1P8V,
            .user = PM_AUXLDO_USER_DISPLAY,
            .state = PM_AUXLDO_DISABLE,
        };
        if (bk_pm_auxldo_ctrl_vote(&vddio) != BK_OK) {
            MYBOT_LOGW(TAG, "failed to release panel VDDIO");
            return -1;
        }
        s_lcd.vddio_owned = false;
    }
    if (s_lcd.power_1v2_gpio_owned) {
        if (bk_gpio_set_output_low(CAMERA_POWER_1V2_GPIO) != BK_OK) {
            MYBOT_LOGW(TAG, "failed to disable camera 1.2V power; retaining power ownership");
            return -1;
        }
        s_lcd.power_1v2_gpio_owned = false;
        MYBOT_LOGI(TAG, "camera 1.2V power disabled: gpio=27 level=0");
    }
    if (s_lcd.power_gpio_owned) {
        if (bk_gpio_set_output_low(LCD_POWER_GPIO) != BK_OK) {
            MYBOT_LOGW(TAG, "failed to disable panel 3.3V");
            result = -1;
        } else {
            s_lcd.power_gpio_owned = false;
        }
    }
    if (s_lcd.power_2v8_gpio_owned) {
        if (bk_gpio_set_output_low(LCD_POWER_2V8_GPIO) != BK_OK) {
            MYBOT_LOGW(TAG, "failed to disable panel 2.8V");
            result = -1;
        } else {
            s_lcd.power_2v8_gpio_owned = false;
        }
    }
    return result;
}

static bool resources_owned(void)
{
    return s_lcd.worker || s_lcd.display || s_lcd.draw_buffer || s_lcd.touch_attempted ||
           s_lcd.controller || s_lcd.panel || s_lcd.bus || s_lcd.frames[0] ||
           s_lcd.frames[1] ||
           s_lcd.vddio_owned || s_lcd.power_gpio_owned || s_lcd.power_2v8_gpio_owned ||
           s_lcd.power_1v2_gpio_owned ||
           s_lcd.backlight_gpio_owned ||
           s_lcd.cpu_vote_owned;
}

static int stop_worker(void)
{
    aosl_atomic_set(&s_lcd.stopping, 1);
    if (!s_lcd.worker) {
        return 0;
    }
    notify(&s_lcd.wake);
    if (rtos_get_semaphore(&s_lcd.worker_done, LCD_STOP_TIMEOUT_MS) != BK_OK) {
        MYBOT_LOGE(TAG, "UI stop timed out; retaining display and worker resources");
        return -1;
    }
    /* The worker now only self-deletes. Do not use BK's polling join here:
     * it dereferences a TCB which an idle core may already have reclaimed. */
    s_lcd.worker = NULL;
    return 0;
}

/* Called under lifecycle lock with accepting=false; callbacks remain valid
 * until display deinit completes, including after a failed earlier teardown. */
static int teardown_locked(void)
{
    if (stop_worker() < 0) {
        return -1;
    }
    /* No more LVGL reads can run. Do not free its display or input device while
     * the TP driver may still own a callback or shared software-I2C handle. */
    if (s_lcd.touch_attempted) {
        if (bk7259_touch_shutdown() < 0) {
            MYBOT_LOGE(TAG, "touch shutdown incomplete; retaining display resources");
            return -1;
        }
        s_lcd.touch_attempted = false;
    }
    if (s_lcd.backlight_gpio_owned && set_backlight(false) < 0) {
        MYBOT_LOGW(TAG, "failed to disable display backlight");
    }
    if (s_lcd.controller_open) {
        if (bk_display_close(s_lcd.controller) != AVDK_ERR_OK) {
            MYBOT_LOGE(TAG, "display close failed; retaining resources");
            return -1;
        }
        s_lcd.controller_open = false;
    }
    if (s_lcd.controller_inited) {
        if (bk_display_deinit(s_lcd.controller) != AVDK_ERR_OK) {
            MYBOT_LOGE(TAG, "display deinit failed; retaining scanout buffers");
            return -1;
        }
        s_lcd.controller_inited = false;
    }
    uint32_t callback_wait = rtos_get_time();
    while (aosl_atomic_read(&s_lcd.frame_busy[0]) ||
           aosl_atomic_read(&s_lcd.frame_busy[1]) ||
           aosl_atomic_read(&s_lcd.callbacks_active)) {
        if ((uint32_t)(rtos_get_time() - callback_wait) >= LCD_RENDER_TIMEOUT_MS) {
            MYBOT_LOGE(TAG, "display frame release unconfirmed; retaining resources");
            return -1;
        }
        rtos_delay_milliseconds(1);
    }
    if (s_lcd.controller) {
        if (bk_display_delete(s_lcd.controller) != AVDK_ERR_OK) {
            MYBOT_LOGE(TAG, "display delete failed; retaining resources");
            return -1;
        }
        s_lcd.controller = NULL;
    }
    if (s_lcd.panel) {
        if (bk_lcd_panel_delete(s_lcd.panel) != BK_OK) {
            MYBOT_LOGE(TAG, "panel delete failed; retaining resources");
            return -1;
        }
        s_lcd.panel = NULL;
    }
    if (s_lcd.bus) {
        if (bk_display_bus_delete(s_lcd.bus) != AVDK_ERR_OK) {
            MYBOT_LOGE(TAG, "DSI bus delete failed; retaining resources");
            return -1;
        }
        s_lcd.bus = NULL;
    }
    if (s_lcd.view_created) {
        mybot_lvgl_view_destroy();
        s_lcd.view_created = false;
    }
    if (s_lcd.display) {
        lv_display_delete(s_lcd.display);
        s_lcd.display = NULL;
    }
    if (s_lcd.draw_buffer) {
        hsram_free(s_lcd.draw_buffer);
        s_lcd.draw_buffer = NULL;
    }
    for (unsigned int i = 0; i < LCD_FRAME_COUNT; ++i) {
        aosl_atomic_set(&s_lcd.frame_busy[i], 0);
        if (s_lcd.frames[i]) {
            bk_frame_buffer_free(s_lcd.frames[i]);
            s_lcd.frames[i] = NULL;
        }
    }
    if (s_lcd.cpu_vote_owned) {
        if (bk_pm_module_vote_cpu_freq(PM_DEV_ID_LVGL, PM_CPU_FRQ_DEFAULT) != BK_OK) {
            MYBOT_LOGW(TAG, "failed to release UI CPU frequency vote");
            return -1;
        }
        s_lcd.cpu_vote_owned = false;
    }
    return panel_power_off();
}

int bk7259_lcd_prepare(void)
{
    /* This entry point is called by the sole product startup/shutdown owner. */
    if ((!s_lifecycle_lock && rtos_init_mutex(&s_lifecycle_lock) != BK_OK) ||
        (!s_snapshot_lock && rtos_init_mutex(&s_snapshot_lock) != BK_OK) ||
        rtos_lock_mutex(&s_lifecycle_lock) != BK_OK) {
        return -1;
    }
    if (rtos_lock_mutex(&s_snapshot_lock) != BK_OK) {
        unlock(&s_lifecycle_lock);
        return -1;
    }
    if (s_lcd.prepared && !aosl_atomic_read(&s_lcd.stopping)) {
        unlock(&s_snapshot_lock);
        unlock(&s_lifecycle_lock);
        return 0;
    }
    /* A worker that exited on an RTOS error must be collected before retrying. */
    s_lcd.accepting = false;
    s_lcd.sdk_attached = false;
    s_lcd.prepared = false;
    s_lcd.pending = false;
    unlock(&s_snapshot_lock);
    if (resources_owned() && teardown_locked() < 0) {
        unlock(&s_lifecycle_lock);
        return -1;
    }
    s_lcd.composing = -1;
    s_lcd.previous = -1;
    s_lcd.discard_refresh = false;
    s_lcd.redraw_needed = false;
    s_lcd.flush_fault = false;
    aosl_atomic_set(&s_lcd.stopping, 0);
    if (!s_frame_buffer_initialized) {
        bk_frame_buffer_init();
        s_frame_buffer_initialized = true;
    }
    /* Three bounded, process-lifetime notifications reused across prepare /
     * shutdown. In particular worker_done and frame_done may be given in an
     * ISR/on another core while the notified task wakes. */
    if ((!s_lcd.frame_done && rtos_init_semaphore_ex(&s_lcd.frame_done, LCD_FRAME_COUNT, 0) != BK_OK) ||
        (!s_lcd.wake && rtos_init_semaphore_ex(&s_lcd.wake, 1, 0) != BK_OK) ||
        (!s_lcd.worker_done && rtos_init_semaphore_ex(&s_lcd.worker_done, 1, 0) != BK_OK)) {
        goto failed;
    }
    while (rtos_get_semaphore(&s_lcd.frame_done, 0) == BK_OK) {}
    while (rtos_get_semaphore(&s_lcd.wake, 0) == BK_OK) {}
    for (unsigned int i = 0; i < LCD_FRAME_COUNT; ++i) {
        s_lcd.frames[i] = bk_frame_buffer_malloc(MEM_SLAB_HEAP_UNCODED, LCD_FRAME_BYTES);
        if (!s_lcd.frames[i]) {
            MYBOT_LOGE(TAG, "failed to allocate scanout buffer %u", i);
            goto failed;
        }
        memset(s_lcd.frames[i], 0, LCD_FRAME_BYTES);
    }
    s_lcd.draw_buffer = hsram_malloc(LCD_DRAW_BYTES);
    if (!s_lcd.draw_buffer || panel_power_on() < 0 ||
        bk_display_dsi_bus_new(&s_lcd.bus, NULL) != AVDK_ERR_OK) {
        goto failed;
    }
    const bk_lcd_panel_config_t panel_config = {.reset_pin = LCD_RESET_GPIO};
    if (bk_lcd_mipi_panel_new(s_lcd.bus, &panel_config, &lcd_device_gc9503cv_mipi_480x640,
                            &s_lcd.panel) != BK_OK ||
        bk_display_dpu_ctlr_new(&s_lcd.controller, s_lcd.panel, &s_controller_config) != AVDK_ERR_OK ||
        bk_display_init(s_lcd.controller) != AVDK_ERR_OK) {
        goto failed;
    }
    s_lcd.controller_inited = true;
    if (bk_display_open(s_lcd.controller) != AVDK_ERR_OK) {
        goto failed;
    }
    s_lcd.controller_open = true;
    if (bk_pm_module_vote_cpu_freq(PM_DEV_ID_LVGL, PM_CPU_FRQ_480M) != BK_OK) {
        goto failed;
    }
    s_lcd.cpu_vote_owned = true;
    if (!s_lvgl_initialized) {
        lv_init();
        configure_draw_allocators();
        s_lvgl_initialized = true;
    }
    lv_log_register_print_cb(lvgl_log);
    lv_tick_set_cb(lcd_tick);
    s_lcd.display = lv_display_create(LCD_LOGICAL_WIDTH, LCD_LOGICAL_HEIGHT);
    if (!s_lcd.display) {
        goto failed;
    }
    lv_display_set_color_format(s_lcd.display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_lcd.display, s_lcd.draw_buffer, NULL, LCD_DRAW_BYTES,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_lcd.display, lcd_flush);
    if (mybot_lvgl_view_create(s_lcd.display) < 0) {
        goto failed;
    }
    s_lcd.view_created = true;
    /* The product UI is available before boot reset-intent/network handling.
     * A missing panel touch controller must not prevent ordinary SDK startup. */
    s_lcd.touch_attempted = true;
    if (bk7259_touch_prepare(s_lcd.display) < 0) {
        MYBOT_LOGW(TAG, "touch unavailable; screen recovery controls are disabled");
    }
    bk7259_lvgl_view_set_touch_available(bk7259_touch_available());
    lv_refr_now(s_lcd.display);
    if (s_lcd.flush_fault || set_backlight(true) < 0) {
        goto failed;
    }
    if (rtos_lock_mutex(&s_snapshot_lock) != BK_OK) {
        goto failed;
    }
    s_lcd.latest = (mybot_lcd_content_t){.screen = MYBOT_LCD_SCREEN_STARTING};
    s_lcd.runtime_status = (bk7259_lcd_runtime_status_t){.state = "stopped"};
    s_lcd.provisioning_ssid[0] = '\0';
    s_lcd.pending = false;
    s_lcd.factory_reset_active = false;
    bk7259_expression_policy_init(&s_lcd.expression_policy);
    s_lcd.expression_displayed = false;
    s_lcd.accepting = true;
    s_lcd.sdk_attached = false;
    /* No other LVGL calls after this transfer until worker_done. */
    bk_err_t ret = rtos_create_hsram_thread(&s_lcd.worker, LCD_UI_PRIORITY, "mybot_ui",
                                           ui_worker, LCD_UI_STACK_BYTES, NULL);
    if (ret != BK_OK) {
        s_lcd.worker = NULL;
        s_lcd.accepting = false;
        unlock(&s_snapshot_lock);
        goto failed;
    }
    s_lcd.prepared = true;
    unlock(&s_snapshot_lock);
    MYBOT_LOGI(TAG, "LVGL UI ready: %ux%u RGB565, scanout=%u stripe=%u stack=%u bytes",
               LCD_LOGICAL_WIDTH, LCD_LOGICAL_HEIGHT, (unsigned)(LCD_FRAME_BYTES * 2U),
               (unsigned)LCD_DRAW_BYTES, (unsigned)LCD_UI_STACK_BYTES);
    unlock(&s_lifecycle_lock);
    return 0;

failed:
    MYBOT_LOGE(TAG, "LVGL display preparation failed");
    (void)teardown_locked();
    unlock(&s_lifecycle_lock);
    return -1;
}

void bk7259_lcd_shutdown(void)
{
    if (!s_lifecycle_lock || rtos_lock_mutex(&s_lifecycle_lock) != BK_OK) {
        return;
    }
    if (rtos_lock_mutex(&s_snapshot_lock) != BK_OK) {
        unlock(&s_lifecycle_lock);
        return;
    }
    s_lcd.accepting = false;
    s_lcd.sdk_attached = false;
    s_lcd.prepared = false;
    s_lcd.pending = false;
    bk7259_expression_policy_cancel(&s_lcd.expression_policy);
    if (bk7259_gimbal_set_conversation(false) < 0) {
        MYBOT_LOGW(TAG, "gimbal shutdown cancellation rejected");
    }
    unlock(&s_snapshot_lock);
    if (resources_owned()) {
        MYBOT_LOGI(TAG, "LVGL UI stopping");
        if (teardown_locked() == 0) {
            MYBOT_LOGI(TAG, "LVGL UI stopped");
        }
    }
    unlock(&s_lifecycle_lock);
}

static bool valid_content(const mybot_lcd_content_t *content)
{
    if (!content || content->screen < MYBOT_LCD_SCREEN_STARTING ||
        content->screen >= MYBOT_LCD_SCREEN_COUNT) {
        return false;
    }
    return content->screen != MYBOT_LCD_SCREEN_PAIR_CODE ||
           memchr(content->pair_code, '\0', sizeof(content->pair_code)) != NULL;
}

/* Calls only copy to the gimbal owner's mailbox. That owner never calls LCD
 * while holding its request lock, so snapshot -> gimbal lock ordering is fixed. */
static void set_gimbal_conversation(bool active)
{
    if (bk7259_gimbal_set_conversation(active) < 0) {
        MYBOT_LOGW(TAG, "gimbal conversation request rejected: active=%d", (int)active);
    }
}

static int read_emotion(uint32_t indicators, bk7259_expression_t *expression)
{
    switch (indicators & MYBOT_LCD_INDICATOR_EMOTION_MASK) {
    case MYBOT_LCD_INDICATOR_HAPPY:
        *expression = BK7259_EXPRESSION_HAPPY;
        return 1;
    case MYBOT_LCD_INDICATOR_SAD:
        *expression = BK7259_EXPRESSION_SAD;
        return 1;
    case MYBOT_LCD_INDICATOR_ANGRY:
        *expression = BK7259_EXPRESSION_ANNOYED;
        return 1;
    case MYBOT_LCD_INDICATOR_SURPRISED:
        *expression = BK7259_EXPRESSION_SURPRISED;
        return 1;
    case MYBOT_LCD_INDICATOR_NONE:
        return 0;
    default:
        return -1;
    }
}

/* Caller holds snapshot lock. No borrowed SDK pointer crosses this boundary. */
static int publish_content(const mybot_lcd_content_t *content)
{
    if (!s_lcd.accepting || aosl_atomic_read(&s_lcd.stopping)) {
        return -1;
    }
    /* A product reset supersedes workflow transitions until reboot. SDK stop
     * still gets a successful render result without replacing this override. */
    if (s_lcd.factory_reset_active) {
        return 0;
    }
    bk7259_expression_t expression = BK7259_EXPRESSION_NEUTRAL;
    const bool in_conversation = content->screen == MYBOT_LCD_SCREEN_IN_CONVERSATION;
    const int emotion = in_conversation
        ? read_emotion(content->indicators, &expression) : 0;
    if (emotion < 0) {
        MYBOT_LOGW(TAG, "mutually exclusive emotion indicators required: 0x%x",
                   (unsigned)(content->indicators & MYBOT_LCD_INDICATOR_EMOTION_MASK));
        return -1;
    }
    if ((s_lcd.latest.screen == MYBOT_LCD_SCREEN_IN_CONVERSATION) != in_conversation) {
        set_gimbal_conversation(in_conversation);
    }
    bk7259_expression_policy_update(&s_lcd.expression_policy, content, rtos_get_time());
    /* Consume the one-shot before retaining the ordinary latest snapshot.
     * Consecutive requests for the same scene intentionally restart it. */
    if (emotion) {
        if (!bk7259_expression_policy_mcp(&s_lcd.expression_policy, expression))
            return -1;
        MYBOT_LOGI(TAG, "MCP expression requested: %s", bk7259_expression_name(expression));
    }
    s_lcd.latest = *content;
    s_lcd.latest.indicators &= ~MYBOT_LCD_INDICATOR_EMOTION_MASK;
    s_lcd.pending = true;
    notify(&s_lcd.wake);
    return 0;
}

int bk7259_lcd_show_factory_reset(bk7259_factory_reset_screen_t screen)
{
    if (screen < BK7259_FACTORY_RESET_RESTORING || screen > BK7259_FACTORY_RESET_REBOOTING ||
        !s_snapshot_lock || rtos_lock_mutex(&s_snapshot_lock) != BK_OK) {
        return -1;
    }
    int ret = -1;
    if (s_lcd.prepared && s_lcd.accepting && !aosl_atomic_read(&s_lcd.stopping)) {
        s_lcd.factory_reset_active = true;
        bk7259_expression_policy_cancel(&s_lcd.expression_policy);
        set_gimbal_conversation(false);
        s_lcd.factory_reset_screen = screen;
        s_lcd.pending = true;
        notify(&s_lcd.wake);
        ret = 0;
    }
    unlock(&s_snapshot_lock);
    return ret;
}

/* Caller holds the snapshot lock. Expressions never replace operating or
 * recovery instructions, and a queued request has no borrowed payload. */
static bool can_show_expression(void)
{
    return s_lcd.prepared && s_lcd.accepting && !s_lcd.factory_reset_active &&
           !aosl_atomic_read(&s_lcd.stopping) &&
           (s_lcd.latest.screen == MYBOT_LCD_SCREEN_READY ||
            s_lcd.latest.screen == MYBOT_LCD_SCREEN_IN_CONVERSATION);
}

int bk7259_lcd_set_expression(bk7259_expression_t expression)
{
    const char *name = bk7259_expression_name(expression);
    if (!name || !s_snapshot_lock || rtos_lock_mutex(&s_snapshot_lock) != BK_OK) {
        return -1;
    }
    int ret = -1;
    if (can_show_expression()) {
        if (bk7259_expression_policy_local(&s_lcd.expression_policy, expression)) {
            s_lcd.pending = true;
            notify(&s_lcd.wake);
            MYBOT_LOGI(TAG, "expression requested: %s", name);
        }
        ret = 0;
    }
    unlock(&s_snapshot_lock);
    return ret;
}

int bk7259_lcd_clear_expression(void)
{
    if (!s_snapshot_lock || rtos_lock_mutex(&s_snapshot_lock) != BK_OK) {
        return -1;
    }
    int ret = -1;
    if (can_show_expression()) {
        if (bk7259_expression_policy_clear(&s_lcd.expression_policy)) {
            s_lcd.pending = true;
            notify(&s_lcd.wake);
            MYBOT_LOGI(TAG, "expression override cleared");
        }
        ret = 0;
    }
    unlock(&s_snapshot_lock);
    return ret;
}

int bk7259_lcd_show_screen(mybot_lcd_screen_t screen)
{
    mybot_lcd_content_t content = {.screen = screen};
    if (screen == MYBOT_LCD_SCREEN_PAIR_CODE || !valid_content(&content) ||
        !s_snapshot_lock || rtos_lock_mutex(&s_snapshot_lock) != BK_OK) {
        return -1;
    }
    int ret = publish_content(&content);
    unlock(&s_snapshot_lock);
    return ret;
}

int bk7259_lcd_set_runtime_status(const char *state, bool in_conversation)
{
    if (!state || !state[0] ||
        strnlen(state, BK7259_LCD_STATE_CAPACITY) >= BK7259_LCD_STATE_CAPACITY ||
        !s_snapshot_lock || rtos_lock_mutex(&s_snapshot_lock) != BK_OK) {
        return -1;
    }
    int ret = -1;
    if (s_lcd.prepared && s_lcd.accepting && !aosl_atomic_read(&s_lcd.stopping)) {
        ret = 0;
        if (!s_lcd.factory_reset_active &&
            (strcmp(s_lcd.runtime_status.state, state) != 0 ||
             s_lcd.runtime_status.in_conversation != in_conversation)) {
            strcpy(s_lcd.runtime_status.state, state);
            s_lcd.runtime_status.in_conversation = in_conversation;
            s_lcd.pending = true;
            notify(&s_lcd.wake);
        }
    }
    unlock(&s_snapshot_lock);
    return ret;
}

void bk7259_lcd_set_provisioning_ssid(const char *ssid)
{
    if (!ssid || !s_snapshot_lock || rtos_lock_mutex(&s_snapshot_lock) != BK_OK) {
        return;
    }
    if (s_lcd.accepting && !s_lcd.factory_reset_active) {
        size_t len = strnlen(ssid, sizeof(s_lcd.provisioning_ssid) - 1U);
        memcpy(s_lcd.provisioning_ssid, ssid, len);
        s_lcd.provisioning_ssid[len] = '\0';
        s_lcd.pending = true;
        notify(&s_lcd.wake);
    }
    unlock(&s_snapshot_lock);
}

static int lcd_sdk_init(void **out_context)
{
    if (!out_context || !s_snapshot_lock || rtos_lock_mutex(&s_snapshot_lock) != BK_OK) {
        return -1;
    }
    *out_context = NULL;
    int ret = -1;
    if (s_lcd.prepared && s_lcd.accepting && !s_lcd.sdk_attached && !s_lcd.factory_reset_active &&
        !aosl_atomic_read(&s_lcd.stopping)) {
        s_lcd.sdk_attached = true;
        *out_context = &s_lcd;
        ret = 0;
    }
    unlock(&s_snapshot_lock);
    return ret;
}

static int lcd_sdk_render(void *context, const mybot_lcd_content_t *content)
{
    if (context != &s_lcd || !valid_content(content) || !s_snapshot_lock ||
        rtos_lock_mutex(&s_snapshot_lock) != BK_OK) {
        return -1;
    }
    int ret = s_lcd.sdk_attached ? publish_content(content) : -1;
    unlock(&s_snapshot_lock);
    return ret;
}

static void lcd_sdk_destroy(void *context)
{
    if (context != &s_lcd || !s_snapshot_lock || rtos_lock_mutex(&s_snapshot_lock) != BK_OK) {
        return;
    }
    /* Product UI remains alive for provisioning, lifecycle text and reset.
     * Retain workflow text, but never a request from the detached conversation. */
    s_lcd.sdk_attached = false;
    bk7259_expression_policy_cancel(&s_lcd.expression_policy);
    s_lcd.latest.indicators = MYBOT_LCD_INDICATOR_NONE;
    set_gimbal_conversation(false);
    if (s_lcd.accepting && !aosl_atomic_read(&s_lcd.stopping)) {
        s_lcd.pending = true;
        notify(&s_lcd.wake);
    }
    unlock(&s_snapshot_lock);
}

const mybot_lcd_ops_t g_mybot_bk7259_lcd_ops = {
    .init = lcd_sdk_init,
    .render = lcd_sdk_render,
    .destroy = lcd_sdk_destroy,
};
