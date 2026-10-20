/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BK7259_LCD_H_
#define BK7259_LCD_H_

#include <stdbool.h>
#include <mybot/platform/mybot_lcd.h>

#define BK7259_LCD_STATE_CAPACITY 32

/* Display snapshot only. The application maps the SDK lifecycle at its public
 * boundary; the display neither reads nor owns the SDK state machine. */
typedef struct {
    char state[BK7259_LCD_STATE_CAPACITY];
    bool in_conversation;
} bk7259_lcd_runtime_status_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Copies a nonempty state name of at most 31 bytes into the prepared display's
 * snapshot. Repeated values are a no-op. Works after SDK detach; factory reset
 * retains priority. Returns 0 on success, -1 if unavailable or invalid. */
int bk7259_lcd_set_runtime_status(const char *state, bool in_conversation);

/* Product-owned startup/provisioning screens remain usable while the SDK is
 * detached. The display copies the requested screen into its UI snapshot. */
int bk7259_lcd_show_screen(mybot_lcd_screen_t screen);

#ifdef __cplusplus
}
#endif

#endif /* BK7259_LCD_H_ */
