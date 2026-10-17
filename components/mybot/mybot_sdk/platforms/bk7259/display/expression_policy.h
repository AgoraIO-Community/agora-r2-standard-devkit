/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BK7259_EXPRESSION_POLICY_H_
#define BK7259_EXPRESSION_POLICY_H_

#include "bk7259_expression.h"
#include <mybot/platform/mybot_lcd.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Display policy only. The LCD snapshot lock serializes all accesses; no SDK
 * lifecycle, RTM parsing, hardware control, allocation or timer is owned here. */
typedef enum {
    BK7259_EXPRESSION_SOURCE_STATE = 0,
    BK7259_EXPRESSION_SOURCE_OPENING,
    BK7259_EXPRESSION_SOURCE_MCP,
    BK7259_EXPRESSION_SOURCE_LOCAL,
} bk7259_expression_source_t;

typedef struct {
    mybot_lcd_screen_t screen;
    uint32_t phase;
    uint32_t phase_since;
    bk7259_expression_source_t source;
    bk7259_expression_t foreground;
    uint32_t foreground_since;
    bool foreground_pending;
} bk7259_expression_policy_t;

static inline void bk7259_expression_policy_init(bk7259_expression_policy_t *policy)
{
    memset(policy, 0, sizeof(*policy));
    policy->screen = MYBOT_LCD_SCREEN_STARTING;
}

static inline bool bk7259_expression_policy_clear(bk7259_expression_policy_t *policy)
{
    bool changed = policy->source != BK7259_EXPRESSION_SOURCE_STATE;
    policy->source = BK7259_EXPRESSION_SOURCE_STATE;
    policy->foreground_pending = false;
    policy->foreground_since = 0;
    return changed;
}

/* Used at SDK detach/reset: retain the workflow page, but discard all state
 * and deadlines borrowed from the ended conversation. */
static inline void bk7259_expression_policy_cancel(bk7259_expression_policy_t *policy)
{
    (void)bk7259_expression_policy_clear(policy);
    policy->phase = 0;
    policy->phase_since = 0;
}

static inline void bk7259_expression_policy_update(bk7259_expression_policy_t *policy,
                                                   const mybot_lcd_content_t *content,
                                                   uint32_t now)
{
    bool conversation = content->screen == MYBOT_LCD_SCREEN_IN_CONVERSATION;
    if (policy->screen != content->screen) {
        (void)bk7259_expression_policy_clear(policy);
        if (conversation) {
            policy->source = BK7259_EXPRESSION_SOURCE_OPENING;
            policy->foreground = BK7259_EXPRESSION_SURPRISED;
            policy->foreground_pending = true;
        }
    }
    policy->screen = content->screen;
    uint32_t phase = 0;
    if (conversation) {
        /* Match the terminal's deterministic priority for a malformed mask. */
        if (content->indicators & MYBOT_LCD_INDICATOR_SPEAKING)
            phase = MYBOT_LCD_INDICATOR_SPEAKING;
        else if (content->indicators & MYBOT_LCD_INDICATOR_THINKING)
            phase = MYBOT_LCD_INDICATOR_THINKING;
        else if (content->indicators & MYBOT_LCD_INDICATOR_LISTENING)
            phase = MYBOT_LCD_INDICATOR_LISTENING;
    }
    if (phase != policy->phase) {
        policy->phase = phase;
        policy->phase_since = now;
    }
}

static inline uint32_t bk7259_expression_mcp_hold_ms(bk7259_expression_t expression)
{
    switch (expression) {
    case BK7259_EXPRESSION_HAPPY: return 2000U;
    case BK7259_EXPRESSION_SAD: return 3000U;
    case BK7259_EXPRESSION_ANNOYED: return 1200U;
    case BK7259_EXPRESSION_SURPRISED: return 800U;
    default: return 0;
    }
}

static inline bool bk7259_expression_policy_mcp(bk7259_expression_policy_t *policy,
                                                bk7259_expression_t expression)
{
    if (policy->screen != MYBOT_LCD_SCREEN_IN_CONVERSATION ||
        !bk7259_expression_mcp_hold_ms(expression))
        return false;
    policy->source = BK7259_EXPRESSION_SOURCE_MCP;
    policy->foreground = expression;
    policy->foreground_pending = true; /* Same name is still a new command. */
    return true;
}

static inline bool bk7259_expression_policy_local(bk7259_expression_policy_t *policy,
                                                  bk7259_expression_t expression)
{
    if (policy->source == BK7259_EXPRESSION_SOURCE_LOCAL && policy->foreground == expression)
        return false;
    policy->source = BK7259_EXPRESSION_SOURCE_LOCAL;
    policy->foreground = expression;
    policy->foreground_pending = false; /* Persistent; identical art need not replay. */
    return true;
}

/* The UI owner calls this every bounded tick, including without a new render.
 * Pending foreground clocks start only after successful display submission.
 * Hold time excludes the entering transition, including for the 300 ms opener. */
static inline bool bk7259_expression_policy_select(bk7259_expression_policy_t *policy,
                                                   uint32_t now, uint32_t transition_ms,
                                                   bk7259_expression_t *expression,
                                                   bool *replay)
{
    *replay = false;
    if (policy->screen != MYBOT_LCD_SCREEN_READY &&
        policy->screen != MYBOT_LCD_SCREEN_IN_CONVERSATION)
        return false;
    if (policy->source == BK7259_EXPRESSION_SOURCE_OPENING ||
        policy->source == BK7259_EXPRESSION_SOURCE_MCP) {
        uint32_t hold = policy->source == BK7259_EXPRESSION_SOURCE_OPENING
                            ? 300U : bk7259_expression_mcp_hold_ms(policy->foreground);
        if (!policy->foreground_pending &&
            (uint32_t)(now - policy->foreground_since) >= hold + transition_ms)
            (void)bk7259_expression_policy_clear(policy);
    }
    if (policy->source != BK7259_EXPRESSION_SOURCE_STATE) {
        *expression = policy->foreground;
        *replay = policy->foreground_pending;
    } else if (policy->phase == MYBOT_LCD_INDICATOR_LISTENING) {
        *expression = BK7259_EXPRESSION_LISTENING;
    } else if (policy->phase == MYBOT_LCD_INDICATOR_THINKING) {
        *expression = (uint32_t)(now - policy->phase_since) >= 3000U
                          ? BK7259_EXPRESSION_PROCESSING : BK7259_EXPRESSION_THINKING;
    } else {
        *expression = BK7259_EXPRESSION_NEUTRAL;
    }
    return true;
}

static inline void bk7259_expression_policy_displayed(bk7259_expression_policy_t *policy,
                                                      uint32_t now)
{
    if (policy->foreground_pending) {
        policy->foreground_since = now;
        policy->foreground_pending = false;
    }
}

static inline const char *bk7259_expression_source_name(bk7259_expression_source_t source)
{
    switch (source) {
    case BK7259_EXPRESSION_SOURCE_OPENING: return "conversation_start";
    case BK7259_EXPRESSION_SOURCE_MCP: return "mcp";
    case BK7259_EXPRESSION_SOURCE_LOCAL: return "local";
    default: return "state";
    }
}

#endif /* BK7259_EXPRESSION_POLICY_H_ */
