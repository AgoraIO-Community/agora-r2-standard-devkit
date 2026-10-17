/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BK7259_EXPRESSION_H_
#define BK7259_EXPRESSION_H_

/* Shared by the renderer and the display owner's expression timing policy. */
#define BK7259_EXPRESSION_TRANSITION_MS 250U

#ifdef __cplusplus
extern "C" {
#endif

/* IDs follow the product simulator's EMOTIONS table. They describe display
 * artwork only and do not change the SDK, connectivity, or motor state. */
typedef enum {
    BK7259_EXPRESSION_NEUTRAL = 0,
    BK7259_EXPRESSION_GREETING,
    BK7259_EXPRESSION_HAPPY,
    BK7259_EXPRESSION_EXCITED,
    BK7259_EXPRESSION_CURIOUS,
    BK7259_EXPRESSION_CONFUSED,
    BK7259_EXPRESSION_THINKING,
    BK7259_EXPRESSION_LISTENING,
    BK7259_EXPRESSION_SURPRISED,
    BK7259_EXPRESSION_SHY,
    BK7259_EXPRESSION_PROUD,
    BK7259_EXPRESSION_LOVE,
    BK7259_EXPRESSION_SLEEPY,
    BK7259_EXPRESSION_BORED,
    BK7259_EXPRESSION_SAD,
    BK7259_EXPRESSION_PLEADING,
    BK7259_EXPRESSION_APOLOGY,
    BK7259_EXPRESSION_WORRIED,
    BK7259_EXPRESSION_ANNOYED,
    BK7259_EXPRESSION_REFUSE,
    BK7259_EXPRESSION_PROCESSING,
    BK7259_EXPRESSION_ERROR,
    BK7259_EXPRESSION_SLEEP,
    BK7259_EXPRESSION_COUNT,
} bk7259_expression_t;

/* Stable uppercase simulator name, or NULL for an invalid ID. The returned
 * storage is immutable and valid for the process lifetime. */
const char *bk7259_expression_name(bk7259_expression_t expression);
/* Accept an exact simulator name, ignoring ASCII letter case. */
int bk7259_expression_from_name(const char *name, bk7259_expression_t *expression);

/* Copy a request to the display owner; successful return means accepted,
 * not that a scanout has completed. Calls never run LVGL on the caller.
 * Overrides are allowed only on READY/IN_CONVERSATION face pages, and are
 * cleared by a workflow screen change or clear_expression(). Factory reset
 * and operating guidance retain priority. Return 0 on acceptance, -1 on
 * invalid ID or when the prepared display cannot accept a face request.
 * Repeating this local override is a no-op. SDK MCP emotion render calls
 * use a separate one-shot path and replay even when the ID is unchanged. */
int bk7259_lcd_set_expression(bk7259_expression_t expression);
int bk7259_lcd_clear_expression(void);

#ifdef __cplusplus
}
#endif

#endif /* BK7259_EXPRESSION_H_ */
