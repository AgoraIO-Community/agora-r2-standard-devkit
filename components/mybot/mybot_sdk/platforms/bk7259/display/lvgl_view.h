/* SPDX-License-Identifier: MIT */
#ifndef MYBOT_LVGL_VIEW_H_
#define MYBOT_LVGL_VIEW_H_

#include "bk7259_expression.h"
#include "bk7259_factory_reset.h"
#include "bk7259_lcd.h"
#include "bk7259_provision.h"
#include "lvgl.h"
#include <mybot/platform/mybot_lcd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Product LVGL view. Only the display owner task calls these functions and LVGL.
 * The display and its active screen must outlive the view. Its single LVGL timer
 * is owned by the view and removed before any widgets on destroy. Update borrows
 * content, provisioning_ssid and runtime status for the duration of the call.
 * SSIDs are at most 32 bytes; NULL uses the generic provisioning title. A NULL
 * runtime status displays stopped. No tasks are created here.
 * Chinese glyphs cover the fixed UI vocabulary, not arbitrary UTF-8 SSIDs.
 * On READY/IN_CONVERSATION pages the display owner selects the expression with
 * show_expression()/replay_expression() immediately after update(); update()
 * never selects a default face. Runtime state and SDK indicators update only
 * the third terminal line and preserve both expression animation clocks.
 * Objects and borrowed line-point arrays are fixed for the view's lifetime. */
int mybot_lvgl_view_create(lv_display_t *display);
void mybot_lvgl_view_update(const mybot_lcd_content_t *content, const char *provisioning_ssid,
                            const bk7259_lcd_runtime_status_t *status);
/* Display-owner-only expression selection on READY/IN_CONVERSATION pages.
 * Guidance/reset screens reject it. After update() enters a face page, this
 * also reveals the requested face when the previous page was guidance.
 * Repeating the visible current ID preserves its animation clocks. */
int bk7259_lvgl_view_show_expression(bk7259_expression_t expression);
/* Display-owner-only one-shot command. Unlike show_expression(), this also
 * restarts the transition and animation clocks for the current expression. */
int bk7259_lvgl_view_replay_expression(bk7259_expression_t expression);
/* Product-owned override; the LCD snapshot owner keeps SDK updates from
 * replacing it. Uses existing widgets/timer and borrows no external strings. */
void bk7259_lvgl_view_show_factory_reset(bk7259_factory_reset_screen_t screen);
/* Product-owned Wi-Fi setup override. Copies at most 32 SSID bytes; NONE
 * releases the override and waits for the owner's newest ordinary snapshot.
 * Factory reset has priority. Calls stay on the existing display owner. */
void bk7259_lvgl_view_show_provision(bk7259_provision_screen_t screen, const char *ssid);
/* Display-owner-only availability snapshot. Repeated values are a no-op.
 * Loss of input cancels an armed button and closes ordinary settings layers;
 * the product-owned recovery page remains visible and its retry is disabled. */
void bk7259_lvgl_view_set_touch_available(bool available);
void mybot_lvgl_view_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_LVGL_VIEW_H_ */
