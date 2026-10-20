/* SPDX-License-Identifier: Apache-2.0 */
#include "lvgl_view.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
constexpr int kWidth = 640;
constexpr int kHeight = 480;
std::array<uint16_t, kWidth * kHeight> pixels{};
std::array<uint16_t, kWidth * 16> strip{};
lv_display_t *display;
lv_indev_t *input;
lv_point_t input_point{};
lv_indev_state_t input_state = LV_INDEV_STATE_RELEASED;
unsigned requests;
unsigned provision_requests;
unsigned provision_attempts;
unsigned cancel_requests;
unsigned cancel_attempts;
bool provision_busy;
bool accept_provision = true;
bool accept_cancel = true;
int provision_abort;
unsigned widgets_after_create;
unsigned timers_after_create;
const char *output_directory;

#if CONFIG_MYBOT_LANGUAGE_ZH_CN
constexpr const char *kSetup = "设置";
constexpr const char *kReset = "恢复出厂";
constexpr const char *kBack = "返回";
constexpr const char *kConfirmTitle = "恢复出厂？";
constexpr const char *kConfirm = "确认恢复";
constexpr const char *kCancel = "取消";
constexpr const char *kRetry = "重试";
constexpr const char *kUnavailable = "触控不可用";
constexpr const char *kDisconnected = "网络已断开";
constexpr const char *kRestoring = "正在恢复出厂设置";
constexpr const char *kFailed = "恢复出厂失败";
constexpr const char *kRebooting = "恢复完成，正在重启";
constexpr const char *kProvision = "配网";
constexpr const char *kProvisionConfirmTitle = "开始配网？";
constexpr const char *kProvisionConfirm = "开始配网";
constexpr const char *kCancelProvision = "取消配网";
constexpr const char *kRetryProvision = "重新配网";
constexpr const char *kPreparing = "正在准备配网";
constexpr const char *kCancelling = "正在取消配网";
constexpr const char *kProvisionFailed = "配网失败";
constexpr const char *kFinishing = "正在保存Wi-Fi";
constexpr const char *kReconnecting = "正在连接Wi-Fi";
#else
constexpr const char *kSetup = "Setup";
constexpr const char *kReset = "Factory reset";
constexpr const char *kBack = "Back";
constexpr const char *kConfirmTitle = "Factory reset?";
constexpr const char *kConfirm = "Reset";
constexpr const char *kCancel = "Cancel";
constexpr const char *kRetry = "Retry";
constexpr const char *kUnavailable = "Touch unavailable";
constexpr const char *kDisconnected = "Wi-Fi disconnected";
constexpr const char *kRestoring = "Restoring factory settings";
constexpr const char *kFailed = "Factory reset failed";
constexpr const char *kRebooting = "Reset complete. Restarting";
constexpr const char *kProvision = "Wi-Fi setup";
constexpr const char *kProvisionConfirmTitle = "Start Wi-Fi setup?";
constexpr const char *kProvisionConfirm = "Start setup";
constexpr const char *kCancelProvision = "Cancel setup";
constexpr const char *kRetryProvision = "Retry Wi-Fi setup";
constexpr const char *kPreparing = "Preparing Wi-Fi setup";
constexpr const char *kCancelling = "Cancelling Wi-Fi setup";
constexpr const char *kProvisionFailed = "Wi-Fi setup failed";
constexpr const char *kFinishing = "Saving Wi-Fi";
constexpr const char *kReconnecting = "Connecting to Wi-Fi";
#endif

lv_obj_t *screen() { return lv_display_get_screen_active(display); }

void flush(lv_display_t *target, const lv_area_t *area, uint8_t *data) {
    assert(area->x1 >= 0 && area->y1 >= 0 && area->x2 < kWidth && area->y2 < kHeight);
    const auto *source = reinterpret_cast<const uint16_t *>(data);
    const size_t width = static_cast<size_t>(area->x2 - area->x1 + 1);
    for (int y = area->y1; y <= area->y2; ++y) {
        std::memcpy(pixels.data() + y * kWidth + area->x1, source, width * sizeof(uint16_t));
        source += width;
    }
    lv_display_flush_ready(target);
}

void read_pointer(lv_indev_t *, lv_indev_data_t *data) {
    data->state = input_state;
    data->point = input_point;
}

void settle(unsigned milliseconds = 20) {
    for (unsigned elapsed = 0; elapsed < milliseconds; elapsed += 10) {
        lv_tick_inc(10);
        lv_timer_handler();
    }
    lv_obj_update_layout(screen());
    lv_refr_now(display);
}

void pointer(lv_point_t point, lv_indev_state_t state) {
    input_point = point;
    input_state = state;
    lv_tick_inc(10);
    lv_indev_read(input);
    lv_obj_update_layout(screen());
    lv_refr_now(display);
}

lv_obj_t *find_label(lv_obj_t *object, const char *text, bool hidden = false) {
    hidden = hidden || lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN);
    if (hidden) {
        return nullptr;
    }
    if (lv_obj_check_type(object, &lv_label_class) &&
        std::strcmp(lv_label_get_text(object), text) == 0) {
        return object;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) {
        if (auto *found = find_label(lv_obj_get_child(object, i), text, hidden)) {
            return found;
        }
    }
    return nullptr;
}

lv_obj_t *label(const char *text) { return find_label(screen(), text); }

lv_obj_t *find_button(lv_obj_t *object, const char *text, bool hidden = false) {
    hidden = hidden || lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN);
    if (hidden) {
        return nullptr;
    }
    if (lv_obj_check_type(object, &lv_label_class) &&
        std::strcmp(lv_label_get_text(object), text) == 0 &&
        lv_obj_has_flag(lv_obj_get_parent(object), LV_OBJ_FLAG_CLICKABLE)) {
        return lv_obj_get_parent(object);
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) {
        if (auto *found = find_button(lv_obj_get_child(object, i), text, hidden)) {
            return found;
        }
    }
    return nullptr;
}

lv_obj_t *button(const char *text) {
    auto *found = find_button(screen(), text);
    assert(found);
    return found;
}

lv_point_t center(lv_obj_t *object) {
    lv_area_t bounds{};
    lv_obj_get_coords(object, &bounds);
    return {(bounds.x1 + bounds.x2) / 2, (bounds.y1 + bounds.y2) / 2};
}

void tap(const char *text) {
    const lv_point_t point = center(button(text));
    pointer(point, LV_INDEV_STATE_PRESSED);
    pointer(point, LV_INDEV_STATE_RELEASED);
    /* Clear wait-release without inventing a second press. */
    pointer(point, LV_INDEV_STATE_RELEASED);
}

unsigned object_count(lv_obj_t *object) {
    unsigned count = 1;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) {
        count += object_count(lv_obj_get_child(object, i));
    }
    return count;
}

unsigned timer_count() {
    unsigned count = 0;
    for (lv_timer_t *timer = lv_timer_get_next(nullptr); timer; timer = lv_timer_get_next(timer)) {
        ++count;
    }
    return count;
}

uint32_t codepoint(const unsigned char *&text) {
    uint32_t value = *text++;
    if (value < 0x80) {
        return value;
    }
    int extra = value >= 0xf0 ? 3 : value >= 0xe0 ? 2 : 1;
    value &= extra == 3 ? 7 : extra == 2 ? 15 : 31;
    while (extra--) {
        assert((*text & 0xc0) == 0x80);
        value = (value << 6) | (*text++ & 0x3f);
    }
    return value;
}

void check_layout(lv_obj_t *object, bool hidden = false) {
    hidden = hidden || lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN);
    if (!hidden && lv_obj_check_type(object, &lv_label_class)) {
        const char *text = lv_label_get_text(object);
        const lv_font_t *font = lv_obj_get_style_text_font(object, LV_PART_MAIN);
        lv_point_t extent{};
        lv_text_get_size(&extent, text, font, 0,
                         lv_obj_get_style_text_line_space(object, LV_PART_MAIN),
                         lv_obj_get_width(object), LV_TEXT_FLAG_NONE);
        if (extent.y > lv_obj_get_height(object)) {
            std::fprintf(stderr, "label overflows vertically: '%s': %d > %d\n", text, extent.y,
                         lv_obj_get_height(object));
            assert(false);
        }
        lv_area_t bounds{};
        lv_obj_get_coords(object, &bounds);
        assert(bounds.x1 >= 0 && bounds.y1 >= 0 && bounds.x2 < kWidth && bounds.y2 < kHeight);
        const auto *encoded = reinterpret_cast<const unsigned char *>(text);
        while (*encoded) {
            uint32_t character = codepoint(encoded);
            if (character == '\n' || character == '\r') {
                continue;
            }
            lv_font_glyph_dsc_t glyph{};
            if (!lv_font_get_glyph_dsc(font, &glyph, character, 0) || glyph.is_placeholder) {
                std::fprintf(stderr, "missing glyph U+%04x in '%s'\n", character, text);
                assert(false);
            }
        }
        assert(!std::strstr(text, "长按"));
        assert(!std::strstr(text, "Hold 5"));
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) {
        check_layout(lv_obj_get_child(object, i), hidden);
    }
}

void snapshot(const char *name) {
    settle(300);
    check_layout(screen());
    assert(object_count(screen()) == widgets_after_create);
    assert(timer_count() == timers_after_create);
    const std::string path = std::string(output_directory) + "/" + name + ".ppm";
    FILE *file = std::fopen(path.c_str(), "wb");
    assert(file);
    std::fprintf(file, "P6\n%d %d\n255\n", kWidth, kHeight);
    for (uint16_t pixel : pixels) {
        const unsigned r = (pixel >> 11) & 31;
        const unsigned g = (pixel >> 5) & 63;
        const unsigned b = pixel & 31;
        const unsigned char rgb[] = {static_cast<unsigned char>((r << 3) | (r >> 2)),
                                     static_cast<unsigned char>((g << 2) | (g >> 4)),
                                     static_cast<unsigned char>((b << 3) | (b >> 2))};
        assert(std::fwrite(rgb, 1, 3, file) == 3);
    }
    assert(std::fclose(file) == 0);
}

void update(mybot_lcd_screen_t workflow, const char *state = "ready") {
    mybot_lcd_content_t content{};
    content.screen = workflow;
    content.indicators = MYBOT_LCD_INDICATOR_LISTENING | MYBOT_LCD_INDICATOR_VP_REGISTERED;
    std::strcpy(content.pair_code, "000042");
    bk7259_lcd_runtime_status_t status{};
    std::snprintf(status.state, sizeof(status.state), "%s", state);
    status.in_conversation = workflow == MYBOT_LCD_SCREEN_IN_CONVERSATION;
    mybot_lvgl_view_update(&content, "MyBot-0123456789abcdefghijklmnopqr", &status);
    if (workflow == MYBOT_LCD_SCREEN_READY || workflow == MYBOT_LCD_SCREEN_IN_CONVERSATION) {
        assert(bk7259_lvgl_view_show_expression(BK7259_EXPRESSION_NEUTRAL) == 0);
    }
    settle();
}

void open_confirmation() {
    tap(kSetup);
    assert(label(kReset) && label(kBack));
    tap(kReset);
    assert(label(kConfirmTitle) && label(kConfirm) && label(kCancel));
    assert(requests == 0);
}

void test_availability_and_workflows() {
    assert(label(kUnavailable));
    assert(lv_obj_has_state(button(kSetup), LV_STATE_DISABLED));
    tap(kSetup);
    assert(!label(kReset));
    bk7259_lvgl_view_set_touch_available(true);
    assert(!label(kUnavailable));
    assert(!lv_obj_has_state(button(kSetup), LV_STATE_DISABLED));
    const lv_area_t expected = {560, 12, 623, 59};
    for (int workflow = MYBOT_LCD_SCREEN_STARTING; workflow < MYBOT_LCD_SCREEN_COUNT; ++workflow) {
        update(static_cast<mybot_lcd_screen_t>(workflow));
        lv_area_t actual{};
        lv_obj_get_coords(button(kSetup), &actual);
        assert(std::memcmp(&actual, &expected, sizeof(actual)) == 0);
        tap(kSetup);
        assert(label(kReset));
        check_layout(screen());
        tap(kBack);
        assert(!label(kReset));
        assert(requests == 0);
    }
    update(MYBOT_LCD_SCREEN_PAIR_CODE, "pairing");
    assert(label("000042"));
    snapshot("pair-code");
}

void test_modal_cancel_and_latest_background() {
    update(MYBOT_LCD_SCREEN_READY);
    open_confirmation();
    snapshot("confirmation");
    update(MYBOT_LCD_SCREEN_WIFI_DISCONNECTED, "wifi_disconnected");
    assert(label(kConfirmTitle));
    tap(kCancel);
    assert(label(kDisconnected));
    assert(!label(kConfirmTitle) && requests == 0);
    snapshot("cancel-latest-state");
    tap(kSetup);
    snapshot("settings");
    bk7259_lvgl_view_set_touch_available(false);
    assert(!label(kBack) && label(kUnavailable));
    bk7259_lvgl_view_set_touch_available(false);
    bk7259_lvgl_view_set_touch_available(true);
}

void test_no_press_through() {
    update(MYBOT_LCD_SCREEN_READY);
    lv_point_t point = center(button(kSetup));
    pointer(point, LV_INDEV_STATE_PRESSED);
    for (unsigned i = 0; i < 5; ++i) {
        pointer(point, LV_INDEV_STATE_PRESSED);
        assert(!label(kReset));
    }
    pointer(point, LV_INDEV_STATE_RELEASED);
    for (unsigned i = 0; i < 5; ++i) {
        pointer(point, LV_INDEV_STATE_RELEASED);
        assert(label(kReset) && !label(kConfirmTitle));
    }
    point = center(button(kReset));
    pointer(point, LV_INDEV_STATE_PRESSED);
    assert(!label(kConfirmTitle));
    pointer(point, LV_INDEV_STATE_RELEASED);
    for (unsigned i = 0; i < 5; ++i) {
        pointer(point, LV_INDEV_STATE_RELEASED);
        assert(label(kConfirmTitle) && requests == 0);
    }
    tap(kCancel);
}

void test_drag_cancel_and_indev_reset() {
    const lv_point_t setup = center(button(kSetup));
    pointer({320, 450}, LV_INDEV_STATE_PRESSED);
    pointer(setup, LV_INDEV_STATE_PRESSED);
    pointer(setup, LV_INDEV_STATE_RELEASED);
    assert(!label(kReset) && requests == 0);
    open_confirmation();
    lv_point_t point = center(button(kConfirm));
    pointer({96, 90}, LV_INDEV_STATE_PRESSED);
    pointer(point, LV_INDEV_STATE_PRESSED);
    pointer(point, LV_INDEV_STATE_RELEASED);
    assert(requests == 0 && label(kConfirmTitle));
    pointer(point, LV_INDEV_STATE_PRESSED);
    pointer({point.x + 24, point.y}, LV_INDEV_STATE_PRESSED);
    pointer(point, LV_INDEV_STATE_PRESSED);
    pointer(point, LV_INDEV_STATE_RELEASED);
    assert(requests == 0 && label(kConfirmTitle));
    pointer(point, LV_INDEV_STATE_PRESSED);
    lv_obj_send_event(button(kConfirm), LV_EVENT_CANCEL, nullptr);
    pointer(point, LV_INDEV_STATE_RELEASED);
    assert(requests == 0 && label(kConfirmTitle));
    pointer(point, LV_INDEV_STATE_PRESSED);
    /* The transport performs this cancellation for multi-touch, malformed
     * coordinates, I2C errors and queue overflow. It is not an ordinary UP. */
    lv_indev_reset(input, nullptr);
    lv_indev_wait_release(input);
    pointer(point, LV_INDEV_STATE_RELEASED);
    pointer(point, LV_INDEV_STATE_RELEASED);
    assert(requests == 0 && label(kConfirmTitle));
    pointer(point, LV_INDEV_STATE_PRESSED);
    bk7259_lvgl_view_set_touch_available(false);
    bk7259_lvgl_view_set_touch_available(true);
    pointer(point, LV_INDEV_STATE_RELEASED);
    assert(requests == 0 && !label(kConfirmTitle));
}

void test_all_expressions() {
    update(MYBOT_LCD_SCREEN_READY);
    for (int expression = BK7259_EXPRESSION_NEUTRAL; expression < BK7259_EXPRESSION_COUNT;
         ++expression) {
        const auto value = static_cast<bk7259_expression_t>(expression);
        assert(bk7259_lvgl_view_replay_expression(value) == 0);
        snapshot(bk7259_expression_name(value));
        assert(label(kSetup));
    }
}

void open_provision_confirmation() {
    tap(kSetup);
    tap(kProvision);
    assert(label(kProvisionConfirmTitle) && label(kProvisionConfirm));
}

void close_provision() {
    provision_busy = false;
    provision_abort = 0;
    accept_provision = accept_cancel = true;
    bk7259_lvgl_view_show_provision(BK7259_PROVISION_NONE, nullptr);
    update(MYBOT_LCD_SCREEN_READY);
}

void test_provision_restores_unchanged_terminal() {
    const mybot_lcd_screen_t workflows[] = {MYBOT_LCD_SCREEN_READY,
                                            MYBOT_LCD_SCREEN_IN_CONVERSATION};
    const char *states[] = {"ready", "in_conversation"};
    const char *lines[] = {"> state: ready", "> state: listening | vp:ok"};
    for (unsigned i = 0; i < 2; ++i) {
        update(workflows[i], states[i]);
        assert(label(lines[i]));
        bk7259_lvgl_view_show_provision(BK7259_PROVISION_PREPARING, nullptr);
        assert(label("> state: wifi_setup"));
        bk7259_lvgl_view_show_provision(BK7259_PROVISION_NONE, nullptr);
        update(workflows[i], states[i]);
        assert(label(lines[i]) && !label("> state: wifi_setup"));
    }
    close_provision();
}

void test_provision_confirmation_and_cancel() {
    update(MYBOT_LCD_SCREEN_IN_CONVERSATION, "in_conversation");
    open_provision_confirmation();
    snapshot("provision-confirmation");
    update(MYBOT_LCD_SCREEN_WIFI_DISCONNECTED, "wifi_disconnected");
    tap(kCancel);
    assert(label(kDisconnected));
    assert(provision_attempts == 0 && provision_requests == 0 && requests == 0);
    open_provision_confirmation();
    accept_provision = false;
    tap(kProvisionConfirm);
    assert(provision_attempts == 1 && provision_requests == 0);
    assert(label(kProvisionConfirmTitle) && !label(kPreparing));
    accept_provision = true;
    const lv_point_t point = center(button(kProvisionConfirm));
    pointer(point, LV_INDEV_STATE_PRESSED);
    assert(provision_requests == 0);
    pointer(point, LV_INDEV_STATE_PRESSED);
    pointer(point, LV_INDEV_STATE_RELEASED);
    assert(provision_requests == 1 && label(kPreparing) && label(kCancelProvision));
    pointer(point, LV_INDEV_STATE_RELEASED);
    pointer(point, LV_INDEV_STATE_PRESSED);
    pointer(point, LV_INDEV_STATE_RELEASED);
    assert(provision_requests == 1);
    update(MYBOT_LCD_SCREEN_FAILED, "failed");
    assert(label(kPreparing));
    assert(bk7259_lvgl_view_replay_expression(BK7259_EXPRESSION_HAPPY) == -1);
    tap(kSetup);
    assert(lv_obj_has_state(button(kProvision), LV_STATE_DISABLED));
    tap(kProvision);
    assert(!label(kProvisionConfirmTitle) && provision_requests == 1);
    tap(kBack);
    const lv_point_t cancel_point = center(button(kCancelProvision));
    tap(kCancelProvision);
    assert(cancel_requests == 1 && label(kCancelling));
    assert(!label(kCancelProvision));
    bk7259_lvgl_view_show_provision(BK7259_PROVISION_PREPARING, nullptr);
    bk7259_lvgl_view_show_provision(BK7259_PROVISION_ACTIVE, "mybot-late-active");
    assert(label(kCancelling) && !label("mybot-late-active"));
    pointer(cancel_point, LV_INDEV_STATE_PRESSED);
    pointer(cancel_point, LV_INDEV_STATE_RELEASED);
    assert(cancel_requests == 1);
    snapshot("provision-cancelling");
    close_provision();
}

void test_provision_active_and_late_cancel() {
    provision_busy = true;
    char ssid[33];
    std::memset(ssid, 'A', sizeof(ssid) - 1);
    std::memcpy(ssid, "mybot-", 6);
    ssid[sizeof(ssid) - 1] = '\0';
    const std::string original_ssid(ssid);
    bk7259_lvgl_view_show_provision(BK7259_PROVISION_ACTIVE, ssid);
    std::memset(ssid, 'Z', sizeof(ssid) - 1);
    assert(label(original_ssid.c_str()));
    assert(label("http://192.168.4.1") && label(kCancelProvision));
    snapshot("provision-active");
    const unsigned before_cancel = cancel_requests;
    accept_cancel = false; /* credential commit has already claimed the session */
    tap(kCancelProvision);
    assert(cancel_requests == before_cancel && label(original_ssid.c_str()));
    assert(label(kCancelProvision) && !label(kCancelling));
    bk7259_lvgl_view_show_provision(BK7259_PROVISION_FINISHING, nullptr);
    assert(label(kFinishing) && !label(kCancelProvision));
    snapshot("provision-finishing");
    bk7259_lvgl_view_show_provision(BK7259_PROVISION_RECONNECTING, nullptr);
    assert(label(kReconnecting) && !label(kCancelProvision));
    snapshot("provision-reconnecting");
    close_provision();
}

void test_provision_failed_retry() {
    const unsigned before = provision_requests;
    bk7259_lvgl_view_show_provision(BK7259_PROVISION_FAILED, nullptr);
    assert(label(kProvisionFailed) && label(kRetryProvision) && label(kSetup));
    bk7259_lvgl_view_set_touch_available(false);
    tap(kRetryProvision);
    assert(provision_requests == before);
    bk7259_lvgl_view_set_touch_available(true);
    provision_busy = true;
    bk7259_lvgl_view_show_provision(BK7259_PROVISION_FAILED, nullptr);
    assert(lv_obj_has_state(button(kRetryProvision), LV_STATE_DISABLED));
    tap(kRetryProvision);
    assert(provision_requests == before);
    provision_busy = false;
    bk7259_lvgl_view_show_provision(BK7259_PROVISION_FAILED, nullptr);
    snapshot("provision-failed");
    accept_provision = false;
    tap(kRetryProvision);
    assert(provision_requests == before && label(kProvisionFailed));
    accept_provision = true;
    tap(kRetryProvision);
    assert(provision_requests == before + 1 && label(kPreparing));
    assert(!label(kRetryProvision));
    close_provision();
}

void test_confirmation_busy_and_retry() {
    /* Factory reset stays accessible from an active setup session and always
     * outranks a racing setup snapshot or its later NONE publication. */
    provision_busy = true;
    bk7259_lvgl_view_show_provision(BK7259_PROVISION_ACTIVE, "mybot-factory-priority");
    open_confirmation();
    const lv_point_t point = center(button(kConfirm));
    pointer(point, LV_INDEV_STATE_PRESSED);
    assert(requests == 0);
    pointer(point, LV_INDEV_STATE_PRESSED);
    pointer(point, LV_INDEV_STATE_RELEASED);
    assert(requests == 1 && label(kRestoring));
    bk7259_lvgl_view_show_provision(BK7259_PROVISION_ACTIVE, "mybot-stale");
    bk7259_lvgl_view_show_provision(BK7259_PROVISION_NONE, nullptr);
    assert(label(kRestoring) && !label(kCancelProvision));
    assert(!label(kSetup) && !label(kRetry) && !label(kCancel));
    pointer(point, LV_INDEV_STATE_RELEASED);
    pointer(point, LV_INDEV_STATE_PRESSED);
    pointer(point, LV_INDEV_STATE_RELEASED);
    assert(requests == 1);
    mybot_lcd_content_t content{};
    content.screen = MYBOT_LCD_SCREEN_READY;
    mybot_lvgl_view_update(&content, nullptr, nullptr);
    assert(label(kRestoring));
    assert(bk7259_lvgl_view_replay_expression(BK7259_EXPRESSION_HAPPY) == -1);
    snapshot("restoring");
    bk7259_lvgl_view_show_factory_reset(BK7259_FACTORY_RESET_FAILED);
    assert(label(kFailed) && label(kRetry));
    assert(!label(kSetup) && !label(kCancel) && !label(kBack));
    bk7259_lvgl_view_set_touch_available(false);
    assert(label(kUnavailable));
    assert(lv_obj_has_state(button(kRetry), LV_STATE_DISABLED));
    tap(kRetry);
    assert(requests == 1 && label(kFailed));
    bk7259_lvgl_view_set_touch_available(true);
    snapshot("reset-failed");
    tap(kRetry);
    assert(requests == 2 && label(kRestoring) && !label(kRetry));
    pointer(point, LV_INDEV_STATE_PRESSED);
    pointer(point, LV_INDEV_STATE_RELEASED);
    assert(requests == 2);
    bk7259_lvgl_view_show_factory_reset(BK7259_FACTORY_RESET_REBOOTING);
    assert(label(kRebooting) && !label(kRetry) && !label(kSetup));
    snapshot("rebooting");
}
} // namespace

extern "C" void bk7259_factory_reset_request(void) { ++requests; }

extern "C" bool bk7259_provision_request(void) {
    ++provision_attempts;
    if (!accept_provision || provision_busy) {
        return false;
    }
    ++provision_requests;
    provision_busy = true;
    provision_abort = 0;
    return true;
}

extern "C" bool bk7259_provision_cancel(void) {
    ++cancel_attempts;
    if (!accept_cancel || !provision_busy) {
        return false;
    }
    ++cancel_requests;
    provision_abort = BK7259_PROVISION_CANCELLED;
    return true;
}

extern "C" bool bk7259_provision_busy(void) { return provision_busy; }
extern "C" int bk7259_provision_abort_reason(void) { return provision_abort; }

int main(int argc, char **argv) {
    assert(argc == 2);
    output_directory = argv[1];
    lv_init();
    display = lv_display_create(kWidth, kHeight);
    assert(display);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, strip.data(), nullptr, sizeof(strip),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    input = lv_indev_create();
    assert(input);
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(input, display);
    lv_indev_set_read_cb(input, read_pointer);
    lv_indev_set_mode(input, LV_INDEV_MODE_EVENT);
    assert(mybot_lvgl_view_create(display) == 0);
    settle();
    widgets_after_create = object_count(screen());
    timers_after_create = timer_count();
    test_availability_and_workflows();
    test_modal_cancel_and_latest_background();
    test_no_press_through();
    test_drag_cancel_and_indev_reset();
    test_all_expressions();
    test_provision_restores_unchanged_terminal();
    test_provision_confirmation_and_cancel();
    test_provision_active_and_late_cancel();
    test_provision_failed_retry();
    test_confirmation_busy_and_retry();
    assert(object_count(screen()) == widgets_after_create);
    lv_indev_delete(input);
    mybot_lvgl_view_destroy();
    assert(lv_obj_get_child_count(screen()) == 0);
    /* Recreate after teardown to ensure neither recovery nor availability is
     * retained by the old view, and that no button/timer ownership leaks. */
    assert(mybot_lvgl_view_create(display) == 0);
    settle();
    assert(label(kUnavailable));
    assert(lv_obj_has_state(button(kSetup), LV_STATE_DISABLED));
    mybot_lvgl_view_destroy();
    assert(lv_obj_get_child_count(screen()) == 0);
    lv_display_delete(display);
    lv_deinit();
    std::puts(
        "Touch UI: real LVGL release/cancel paths, Wi-Fi/reset gating and 23 expressions passed");
    return 0;
}
