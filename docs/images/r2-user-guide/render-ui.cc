/* SPDX-License-Identifier: Apache-2.0 */
/* Share the real-LVGL framebuffer, input and request mocks with the UI tests.
 * Renaming their entry point leaves firmware and test sources unchanged. */
#define main touch_ui_regression_main
#include "../../../projects/mybot/tests/touch_ui/test_touch_ui.cc"
#undef main

#include "expression_policy.h"

namespace {
bk7259_expression_policy_t manual_policy;

void advance_policy(unsigned milliseconds) {
    for (unsigned elapsed = 0; elapsed < milliseconds; elapsed += 10) {
        bk7259_expression_t expression;
        bool replay;
        if (bk7259_expression_policy_select(&manual_policy, lv_tick_get(),
                                            BK7259_EXPRESSION_TRANSITION_MS,
                                            &expression, &replay)) {
            const int result = replay ? bk7259_lvgl_view_replay_expression(expression)
                                      : bk7259_lvgl_view_show_expression(expression);
            assert(result == 0);
            bk7259_expression_policy_displayed(&manual_policy, lv_tick_get());
        }
        settle(10);
    }
}

void show_manual(mybot_lcd_screen_t workflow, const char *state, uint32_t indicators = 0) {
    mybot_lcd_content_t content{};
    content.screen = workflow;
    content.indicators = indicators;
    std::strcpy(content.pair_code, "001028");
    bk7259_lcd_runtime_status_t status{};
    std::snprintf(status.state, sizeof(status.state), "%s", state);
    status.in_conversation = workflow == MYBOT_LCD_SCREEN_IN_CONVERSATION;
    mybot_lvgl_view_update(&content, "mybot-a1b2", &status);
    bk7259_expression_policy_update(&manual_policy, &content, lv_tick_get());
    advance_policy(700); /* Let the real 300 ms conversation opener finish. */
}
} // namespace

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
    bk7259_lvgl_view_set_touch_available(true);
    bk7259_expression_policy_init(&manual_policy);
    settle();
    widgets_after_create = object_count(screen());
    timers_after_create = timer_count();

    show_manual(MYBOT_LCD_SCREEN_READY, "ready");
    snapshot("ready");
    show_manual(MYBOT_LCD_SCREEN_IN_CONVERSATION, "in_conversation",
                MYBOT_LCD_INDICATOR_LISTENING);
    snapshot("conversation-listening");
    show_manual(MYBOT_LCD_SCREEN_IN_CONVERSATION, "in_conversation",
                MYBOT_LCD_INDICATOR_THINKING | MYBOT_LCD_INDICATOR_VP_REGISTERED);
    snapshot("conversation-thinking");
    show_manual(MYBOT_LCD_SCREEN_IN_CONVERSATION, "in_conversation",
                MYBOT_LCD_INDICATOR_SPEAKING | MYBOT_LCD_INDICATOR_VP_REGISTERED);
    snapshot("conversation-speaking");

    show_manual(MYBOT_LCD_SCREEN_READY, "ready");
    tap(kSetup);
    snapshot("settings");
    tap(kProvision);
    snapshot("provision-confirmation");
    tap(kCancel);
    tap(kSetup);
    tap(kReset);
    snapshot("factory-confirmation");
    tap(kCancel);

    show_manual(MYBOT_LCD_SCREEN_PAIR_CODE, "pairing");
    snapshot("pair-code");
    provision_busy = true;
    bk7259_lvgl_view_show_provision(BK7259_PROVISION_ACTIVE, "mybot-a1b2");
    snapshot("provision-active");
    provision_busy = false;
    bk7259_lvgl_view_show_provision(BK7259_PROVISION_FAILED, nullptr);
    snapshot("provision-failed");
    bk7259_lvgl_view_show_provision(BK7259_PROVISION_NONE, nullptr);
    show_manual(MYBOT_LCD_SCREEN_READY, "ready");

    bk7259_lvgl_view_show_factory_reset(BK7259_FACTORY_RESET_RESTORING);
    snapshot("restoring");
    bk7259_lvgl_view_show_factory_reset(BK7259_FACTORY_RESET_FAILED);
    snapshot("reset-failed");
    bk7259_lvgl_view_show_factory_reset(BK7259_FACTORY_RESET_REBOOTING);
    snapshot("rebooting");

    lv_indev_delete(input);
    mybot_lvgl_view_destroy();
    lv_display_delete(display);
    lv_deinit();
    std::puts("Rendered 13 Chinese R2 manual screenshots using the production LVGL view.");
    return 0;
}
