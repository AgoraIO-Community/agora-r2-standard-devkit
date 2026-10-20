/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BK7259_TOUCH_H_
#define BK7259_TOUCH_H_

#include <stdbool.h>
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The LCD owner calls prepare before handing LVGL to its worker, and shutdown
 * after that worker stops. IRQ/TP callbacks never call LVGL. A failed shutdown
 * retains driver ownership for retry; keep the display alive until it succeeds. */
int bk7259_touch_prepare(lv_display_t *display);
int bk7259_touch_shutdown(void);
/* Safe from other tasks. A transient read error cancels only that gesture. */
bool bk7259_touch_available(void);

#ifdef __cplusplus
}
#endif

#endif /* BK7259_TOUCH_H_ */
