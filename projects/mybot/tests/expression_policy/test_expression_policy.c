/* SPDX-License-Identifier: Apache-2.0 */
#include "expression_policy.h"

#include <stdio.h>
#include <stdlib.h>

static unsigned int assertions;
static unsigned int groups;

#define CHECK(condition) do { \
    ++assertions; \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static void publish(bk7259_expression_policy_t *policy, mybot_lcd_screen_t screen,
                    uint32_t indicators, uint32_t now)
{
    mybot_lcd_content_t content = {0};
    content.screen = screen;
    content.indicators = indicators;
    bk7259_expression_policy_update(policy, &content, now);
}

static void face(bk7259_expression_policy_t *policy, uint32_t now,
                 uint32_t transition_ms, bk7259_expression_t expected, bool replay)
{
    bk7259_expression_t selected = BK7259_EXPRESSION_COUNT;
    bool selected_replay = !replay;
    CHECK(bk7259_expression_policy_select(policy, now, transition_ms,
                                          &selected, &selected_replay));
    CHECK(selected == expected);
    CHECK(selected_replay == replay);
}

static void no_face(bk7259_expression_policy_t *policy, uint32_t now)
{
    bk7259_expression_t selected = BK7259_EXPRESSION_COUNT;
    bool replay = true;
    CHECK(!bk7259_expression_policy_select(policy, now,
                                           BK7259_EXPRESSION_TRANSITION_MS,
                                           &selected, &replay));
    CHECK(!replay);
}

static void ready(bk7259_expression_policy_t *policy, uint32_t now)
{
    bk7259_expression_policy_init(policy);
    publish(policy, MYBOT_LCD_SCREEN_READY, 0, now);
}

/* Begin a conversation and submit its opener successfully. The returned time
 * is the first tick at which the underlying phase should become visible. */
static uint32_t conversation(bk7259_expression_policy_t *policy, uint32_t phase,
                              uint32_t start, uint32_t transition_ms)
{
    ready(policy, start - 1U);
    publish(policy, MYBOT_LCD_SCREEN_IN_CONVERSATION, phase, start);
    face(policy, start, transition_ms, BK7259_EXPRESSION_SURPRISED, true);
    bk7259_expression_policy_displayed(policy, start);
    return start + transition_ms + 300U;
}

static void test_workflow_and_opener(void)
{
    bk7259_expression_policy_t policy;
    bk7259_expression_policy_init(&policy);
    no_face(&policy, 0);
    publish(&policy, MYBOT_LCD_SCREEN_READY, 0, 10);
    face(&policy, 10, 250, BK7259_EXPRESSION_NEUTRAL, false);
    CHECK(!bk7259_expression_policy_mcp(&policy, BK7259_EXPRESSION_HAPPY));

    publish(&policy, MYBOT_LCD_SCREEN_IN_CONVERSATION,
            MYBOT_LCD_INDICATOR_LISTENING, 100);
    face(&policy, 100, 250, BK7259_EXPRESSION_SURPRISED, true);
    /* A blocked/failed display submission cannot consume the opener. */
    face(&policy, 10000, 250, BK7259_EXPRESSION_SURPRISED, true);
    bk7259_expression_policy_displayed(&policy, 10000);
    face(&policy, 10549, 250, BK7259_EXPRESSION_SURPRISED, false);
    face(&policy, 10550, 250, BK7259_EXPRESSION_LISTENING, false);
    face(&policy, 20000, 250, BK7259_EXPRESSION_LISTENING, false);
    ++groups;
}

static void test_phases_and_thinking_duration(void)
{
    bk7259_expression_policy_t policy;
    uint32_t after_opener = conversation(&policy, MYBOT_LCD_INDICATOR_LISTENING,
                                         100, 250);
    face(&policy, after_opener, 250, BK7259_EXPRESSION_LISTENING, false);
    publish(&policy, MYBOT_LCD_SCREEN_IN_CONVERSATION,
            MYBOT_LCD_INDICATOR_THINKING, 1000);
    face(&policy, 1000, 250, BK7259_EXPRESSION_THINKING, false);
    face(&policy, 3999, 250, BK7259_EXPRESSION_THINKING, false);
    face(&policy, 4000, 250, BK7259_EXPRESSION_PROCESSING, false);
    face(&policy, 10000, 250, BK7259_EXPRESSION_PROCESSING, false);
    publish(&policy, MYBOT_LCD_SCREEN_IN_CONVERSATION,
            MYBOT_LCD_INDICATOR_SPEAKING, 10001);
    face(&policy, 10001, 250, BK7259_EXPRESSION_NEUTRAL, false);
    publish(&policy, MYBOT_LCD_SCREEN_IN_CONVERSATION, 0, 10002);
    face(&policy, 10002, 250, BK7259_EXPRESSION_NEUTRAL, false);
    ++groups;
}

static void test_frequent_ordinary_refreshes(void)
{
    bk7259_expression_policy_t policy;
    (void)conversation(&policy, MYBOT_LCD_INDICATOR_THINKING, 100, 250);
    for (uint32_t now = 110; now < 3100; now += 10) {
        uint32_t vp = now % 20 ? MYBOT_LCD_INDICATOR_VP_REGISTERED : 0;
        publish(&policy, MYBOT_LCD_SCREEN_IN_CONVERSATION,
                MYBOT_LCD_INDICATOR_THINKING | vp, now);
    }
    face(&policy, 3099, 250, BK7259_EXPRESSION_THINKING, false);
    face(&policy, 3100, 250, BK7259_EXPRESSION_PROCESSING, false);
    /* An ordinary refresh after expiry must not restart the opening face. */
    publish(&policy, MYBOT_LCD_SCREEN_IN_CONVERSATION,
            MYBOT_LCD_INDICATOR_THINKING | MYBOT_LCD_INDICATOR_VP_REGISTERED,
            10000);
    face(&policy, 10000, 250, BK7259_EXPRESSION_PROCESSING, false);
    ++groups;
}

static void test_unrendered_phase_edges(void)
{
    bk7259_expression_policy_t policy;
    (void)conversation(&policy, MYBOT_LCD_INDICATOR_THINKING, 100, 250);
    face(&policy, 3100, 250, BK7259_EXPRESSION_PROCESSING, false);
    /* Every publication matters even if the UI thread has not selected a
     * face between these two events. This is a new thinking episode. */
    publish(&policy, MYBOT_LCD_SCREEN_IN_CONVERSATION,
            MYBOT_LCD_INDICATOR_LISTENING, 3200);
    publish(&policy, MYBOT_LCD_SCREEN_IN_CONVERSATION,
            MYBOT_LCD_INDICATOR_THINKING, 3201);
    face(&policy, 6200, 250, BK7259_EXPRESSION_THINKING, false);
    face(&policy, 6201, 250, BK7259_EXPRESSION_PROCESSING, false);
    ++groups;
}

static void test_mcp_holds_and_invalid_requests(void)
{
    const struct {
        bk7259_expression_t expression;
        uint32_t hold;
    } cases[] = {
        {BK7259_EXPRESSION_HAPPY, 2000},
        {BK7259_EXPRESSION_SAD, 3000},
        {BK7259_EXPRESSION_ANNOYED, 1200},
        {BK7259_EXPRESSION_SURPRISED, 800},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        bk7259_expression_policy_t policy;
        (void)conversation(&policy, MYBOT_LCD_INDICATOR_LISTENING, 100, 250);
        CHECK(bk7259_expression_policy_mcp(&policy, cases[i].expression));
        face(&policy, 1000, 250, cases[i].expression, true);
        bk7259_expression_policy_displayed(&policy, 1000);
        face(&policy, 1000 + 250 + cases[i].hold - 1, 250,
              cases[i].expression, false);
        face(&policy, 1000 + 250 + cases[i].hold, 250,
              BK7259_EXPRESSION_LISTENING, false);
    }
    bk7259_expression_policy_t policy;
    (void)conversation(&policy, 0, 100, 250);
    CHECK(!bk7259_expression_policy_mcp(&policy, BK7259_EXPRESSION_GREETING));
    CHECK(!bk7259_expression_policy_mcp(&policy, BK7259_EXPRESSION_COUNT));
    face(&policy, 650, 250, BK7259_EXPRESSION_NEUTRAL, false);
    ++groups;
}

static void test_mcp_replacement_and_same_name_replay(void)
{
    bk7259_expression_policy_t policy;
    (void)conversation(&policy, MYBOT_LCD_INDICATOR_LISTENING, 100, 250);
    CHECK(bk7259_expression_policy_mcp(&policy, BK7259_EXPRESSION_HAPPY));
    face(&policy, 1000, 250, BK7259_EXPRESSION_HAPPY, true);
    bk7259_expression_policy_displayed(&policy, 1000);
    CHECK(bk7259_expression_policy_mcp(&policy, BK7259_EXPRESSION_SAD));
    face(&policy, 1500, 250, BK7259_EXPRESSION_SAD, true);
    bk7259_expression_policy_displayed(&policy, 1500);
    CHECK(bk7259_expression_policy_mcp(&policy, BK7259_EXPRESSION_SAD));
    face(&policy, 4500, 250, BK7259_EXPRESSION_SAD, true);
    bk7259_expression_policy_displayed(&policy, 4500);
    face(&policy, 7749, 250, BK7259_EXPRESSION_SAD, false);
    face(&policy, 7750, 250, BK7259_EXPRESSION_LISTENING, false);
    ++groups;
}

static void test_mcp_restores_latest_phase(void)
{
    bk7259_expression_policy_t policy;
    (void)conversation(&policy, MYBOT_LCD_INDICATOR_LISTENING, 100, 250);
    CHECK(bk7259_expression_policy_mcp(&policy, BK7259_EXPRESSION_SAD));
    face(&policy, 1000, 250, BK7259_EXPRESSION_SAD, true);
    bk7259_expression_policy_displayed(&policy, 1000);
    publish(&policy, MYBOT_LCD_SCREEN_IN_CONVERSATION,
            MYBOT_LCD_INDICATOR_THINKING, 1100);
    for (uint32_t now = 1200; now <= 4200; now += 100) {
        publish(&policy, MYBOT_LCD_SCREEN_IN_CONVERSATION,
                MYBOT_LCD_INDICATOR_THINKING | MYBOT_LCD_INDICATOR_VP_REGISTERED,
                now);
        face(&policy, now, 250, BK7259_EXPRESSION_SAD, false);
    }
    face(&policy, 4250, 250, BK7259_EXPRESSION_PROCESSING, false);
    ++groups;
}

static void test_local_persistence_and_mcp_to_local(void)
{
    bk7259_expression_policy_t policy;
    ready(&policy, 100);
    CHECK(bk7259_expression_policy_local(&policy, BK7259_EXPRESSION_CURIOUS));
    face(&policy, 100, 250, BK7259_EXPRESSION_CURIOUS, false);
    CHECK(!bk7259_expression_policy_local(&policy, BK7259_EXPRESSION_CURIOUS));
    face(&policy, 1000000, 250, BK7259_EXPRESSION_CURIOUS, false);
    CHECK(bk7259_expression_policy_clear(&policy));
    face(&policy, 1000001, 250, BK7259_EXPRESSION_NEUTRAL, false);
    CHECK(!bk7259_expression_policy_clear(&policy));

    (void)conversation(&policy, MYBOT_LCD_INDICATOR_LISTENING, 100, 250);
    CHECK(bk7259_expression_policy_mcp(&policy, BK7259_EXPRESSION_HAPPY));
    face(&policy, 1000, 250, BK7259_EXPRESSION_HAPPY, true);
    bk7259_expression_policy_displayed(&policy, 1000);
    CHECK(bk7259_expression_policy_local(&policy, BK7259_EXPRESSION_HAPPY));
    face(&policy, 1001, 250, BK7259_EXPRESSION_HAPPY, false);
    publish(&policy, MYBOT_LCD_SCREEN_IN_CONVERSATION,
            MYBOT_LCD_INDICATOR_THINKING, 2000);
    face(&policy, 1000000, 250, BK7259_EXPRESSION_HAPPY, false);
    CHECK(!bk7259_expression_policy_local(&policy, BK7259_EXPRESSION_HAPPY));
    CHECK(bk7259_expression_policy_clear(&policy));
    face(&policy, 1000001, 250, BK7259_EXPRESSION_PROCESSING, false);
    ++groups;
}

static void test_leaving_conversation_cancels_foreground(void)
{
    const mybot_lcd_screen_t workflows[] = {
        MYBOT_LCD_SCREEN_WIFI_PROVISIONING, MYBOT_LCD_SCREEN_WIFI_DISCONNECTED,
        MYBOT_LCD_SCREEN_STARTING_SERVICES, MYBOT_LCD_SCREEN_PAIRING,
        MYBOT_LCD_SCREEN_PAIR_CODE, MYBOT_LCD_SCREEN_FAILED,
        MYBOT_LCD_SCREEN_STOPPING,
    };
    for (size_t i = 0; i < sizeof(workflows) / sizeof(workflows[0]); ++i) {
        bk7259_expression_policy_t policy;
        (void)conversation(&policy, MYBOT_LCD_INDICATOR_THINKING, 100, 250);
        CHECK(bk7259_expression_policy_mcp(&policy, BK7259_EXPRESSION_HAPPY));
        bk7259_expression_policy_displayed(&policy, 700);
        publish(&policy, workflows[i], 0, 800);
        no_face(&policy, 801);
        CHECK(!bk7259_expression_policy_mcp(&policy, BK7259_EXPRESSION_SAD));
        publish(&policy, MYBOT_LCD_SCREEN_READY, 0, 900);
        face(&policy, 900, 250, BK7259_EXPRESSION_NEUTRAL, false);
        face(&policy, 1000000, 250, BK7259_EXPRESSION_NEUTRAL, false);
    }
    bk7259_expression_policy_t policy;
    (void)conversation(&policy, MYBOT_LCD_INDICATOR_THINKING, 100, 250);
    CHECK(bk7259_expression_policy_local(&policy, BK7259_EXPRESSION_HAPPY));
    publish(&policy, MYBOT_LCD_SCREEN_READY, 0, 800);
    face(&policy, 800, 250, BK7259_EXPRESSION_NEUTRAL, false);
    ++groups;
}

static void test_cancel_and_reattach(void)
{
    bk7259_expression_policy_t policy;
    (void)conversation(&policy, MYBOT_LCD_INDICATOR_THINKING, 100, 250);
    CHECK(bk7259_expression_policy_mcp(&policy, BK7259_EXPRESSION_SAD));
    bk7259_expression_policy_displayed(&policy, 1000);
    /* Factory-reset/guide ownership and SDK detach both cancel borrowed state. */
    bk7259_expression_policy_cancel(&policy);
    bk7259_expression_policy_cancel(&policy);
    face(&policy, 1001, 250, BK7259_EXPRESSION_NEUTRAL, false);
    publish(&policy, MYBOT_LCD_SCREEN_READY, 0, 1002);
    face(&policy, 100000, 250, BK7259_EXPRESSION_NEUTRAL, false);
    publish(&policy, MYBOT_LCD_SCREEN_IN_CONVERSATION,
            MYBOT_LCD_INDICATOR_LISTENING, 100001);
    face(&policy, 100001, 250, BK7259_EXPRESSION_SURPRISED, true);
    bk7259_expression_policy_displayed(&policy, 100001);
    face(&policy, 100551, 250, BK7259_EXPRESSION_LISTENING, false);
    ++groups;
}

static void test_pending_mcp_display_failure(void)
{
    bk7259_expression_policy_t policy;
    (void)conversation(&policy, MYBOT_LCD_INDICATOR_LISTENING, 100, 250);
    CHECK(bk7259_expression_policy_mcp(&policy, BK7259_EXPRESSION_HAPPY));
    face(&policy, 1000, 250, BK7259_EXPRESSION_HAPPY, true);
    face(&policy, 1000000, 250, BK7259_EXPRESSION_HAPPY, true);
    publish(&policy, MYBOT_LCD_SCREEN_IN_CONVERSATION,
            MYBOT_LCD_INDICATOR_SPEAKING, 1000001);
    face(&policy, 1000001, 250, BK7259_EXPRESSION_HAPPY, true);
    bk7259_expression_policy_displayed(&policy, 1000002);
    face(&policy, 1002251, 250, BK7259_EXPRESSION_HAPPY, false);
    face(&policy, 1002252, 250, BK7259_EXPRESSION_NEUTRAL, false);
    ++groups;
}

static void test_disabled_transitions(void)
{
    bk7259_expression_policy_t policy;
    uint32_t after_opener = conversation(&policy, MYBOT_LCD_INDICATOR_LISTENING,
                                         100, 0);
    face(&policy, after_opener - 1, 0, BK7259_EXPRESSION_SURPRISED, false);
    face(&policy, after_opener, 0, BK7259_EXPRESSION_LISTENING, false);
    CHECK(bk7259_expression_policy_mcp(&policy, BK7259_EXPRESSION_SURPRISED));
    bk7259_expression_policy_displayed(&policy, 500);
    face(&policy, 1299, 0, BK7259_EXPRESSION_SURPRISED, false);
    face(&policy, 1300, 0, BK7259_EXPRESSION_LISTENING, false);
    ++groups;
}

static void test_monotonic_clock_wrap(void)
{
    bk7259_expression_policy_t policy;
    uint32_t start = UINT32_MAX - 100;
    (void)conversation(&policy, MYBOT_LCD_INDICATOR_THINKING, start, 250);
    face(&policy, start + 549U, 250, BK7259_EXPRESSION_SURPRISED, false);
    face(&policy, start + 550U, 250, BK7259_EXPRESSION_THINKING, false);
    face(&policy, start + 2999U, 250, BK7259_EXPRESSION_THINKING, false);
    face(&policy, start + 3000U, 250, BK7259_EXPRESSION_PROCESSING, false);

    (void)conversation(&policy, MYBOT_LCD_INDICATOR_LISTENING,
                        UINT32_MAX - 10000, 250);
    CHECK(bk7259_expression_policy_mcp(&policy, BK7259_EXPRESSION_HAPPY));
    face(&policy, start, 250, BK7259_EXPRESSION_HAPPY, true);
    bk7259_expression_policy_displayed(&policy, start);
    face(&policy, start + 2249U, 250, BK7259_EXPRESSION_HAPPY, false);
    face(&policy, start + 2250U, 250, BK7259_EXPRESSION_LISTENING, false);
    ++groups;
}

int main(void)
{
    test_workflow_and_opener();
    test_phases_and_thinking_duration();
    test_frequent_ordinary_refreshes();
    test_unrendered_phase_edges();
    test_mcp_holds_and_invalid_requests();
    test_mcp_replacement_and_same_name_replay();
    test_mcp_restores_latest_phase();
    test_local_persistence_and_mcp_to_local();
    test_leaving_conversation_cancels_foreground();
    test_cancel_and_reattach();
    test_pending_mcp_display_failure();
    test_disabled_transitions();
    test_monotonic_clock_wrap();
    printf("expression policy: %u groups, %u assertions passed\n", groups, assertions);
    return EXIT_SUCCESS;
}
