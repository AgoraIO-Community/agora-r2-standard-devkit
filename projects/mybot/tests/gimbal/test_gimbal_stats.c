/* SPDX-License-Identifier: Apache-2.0 */
#include "bk7259_gimbal_stats.h"

#include <assert.h>
#include <stdio.h>

static void test_ack_codes_and_fallback(void)
{
    bk7259_gimbal_stats_t stats = {0};
    bk7259_gimbal_stats_snapshot_t snapshot;
    bk7259_gimbal_stats_reset(&stats, 100);
    bk7259_gimbal_stats_tx(&stats, 110, BK7259_GIMBAL_STATS_NORMAL, 20, 0);
    bk7259_gimbal_stats_reply(&stats, 112, 0, 0, 0);
    bk7259_gimbal_stats_reply(&stats, 120, 6, 20, 0);
    bk7259_gimbal_stats_tx(&stats, 140, BK7259_GIMBAL_STATS_NORMAL, 21, 1);
    bk7259_gimbal_stats_reply(&stats, 146, 6, 21, 1);
    bk7259_gimbal_stats_reply(&stats, 147, 0, 0, 0);
    bk7259_gimbal_stats_tx(&stats, 160, BK7259_GIMBAL_STATS_NORMAL, 40, 0);
    bk7259_gimbal_stats_reply(&stats, 167, 1, 0, 0);
    bk7259_gimbal_stats_tx(&stats, 180, BK7259_GIMBAL_STATS_NORMAL, 50, 0);
    bk7259_gimbal_stats_reply(&stats, 186, 2, 0, 0);
    bk7259_gimbal_stats_tx(&stats, 210, BK7259_GIMBAL_STATS_HOME, 0, 0);
    bk7259_gimbal_stats_reply(&stats, 218, 4, 0, 0);
    bk7259_gimbal_stats_tx_failed(&stats, BK7259_GIMBAL_STATS_HOME);
    bk7259_gimbal_stats_bad_reply(&stats);
    bk7259_gimbal_stats_reply(&stats, 220, 0xff, 0, 0);
    bk7259_gimbal_stats_snapshot(&stats, 225, &snapshot);

    const bk7259_gimbal_stats_counts_t *normal = &snapshot.groups[BK7259_GIMBAL_STATS_NORMAL];
    const bk7259_gimbal_stats_counts_t *home = &snapshot.groups[BK7259_GIMBAL_STATS_HOME];
    assert(normal->tx == 4 && normal->accepted == 2 && normal->rejected == 2);
    assert(normal->pending == 0 && normal->completion_fallback == 1 && normal->estimated == 0);
    assert(home->tx == 1 && home->accepted == 1 && home->tx_failed == 1 && home->estimated == 0);
    assert(snapshot.e1_00 == 2 && snapshot.e1_01 == 1 && snapshot.e1_02 == 1);
    assert(snapshot.e1_04 == 1 && snapshot.e1_06 == 2);
    assert(snapshot.unmatched == 0 && snapshot.bad_reply == 2);
    assert(snapshot.latency_samples == 4 && snapshot.latency_min_ms == 2);
    assert(snapshot.latency_max_ms == 8 && snapshot.latency_total_ms == 23);
    assert(snapshot.window_ms == 125);
}

static void test_repeated_target_and_fifo_estimate(void)
{
    bk7259_gimbal_stats_t stats = {0};
    bk7259_gimbal_stats_snapshot_t snapshot;
    bk7259_gimbal_stats_reset(&stats, 100);
    bk7259_gimbal_stats_tx(&stats, 100, BK7259_GIMBAL_STATS_NORMAL, 50, 0);
    bk7259_gimbal_stats_reply(&stats, 101, 0, 0, 0);
    bk7259_gimbal_stats_tx(&stats, 400, BK7259_GIMBAL_STATS_NORMAL, 50, 0);
    bk7259_gimbal_stats_reply(&stats, 401, 6, 50, 0);
    bk7259_gimbal_stats_tx(&stats, 405, BK7259_GIMBAL_STATS_HOME, 0, 0);
    bk7259_gimbal_stats_reply(&stats, 406, 0, 0, 0);
    bk7259_gimbal_stats_reply(&stats, 407, 6, 0, 0);
    bk7259_gimbal_stats_snapshot(&stats, 407, &snapshot);

    const bk7259_gimbal_stats_counts_t *normal = &snapshot.groups[BK7259_GIMBAL_STATS_NORMAL];
    const bk7259_gimbal_stats_counts_t *home = &snapshot.groups[BK7259_GIMBAL_STATS_HOME];
    assert(snapshot.ambiguous == 1 && snapshot.latency_samples == 1);
    assert(normal->accepted == 2 && normal->estimated == 1 && normal->completion_fallback == 0);
    assert(home->accepted == 1 && home->estimated == 0 && home->completion_fallback == 1);
}

static void test_serial_stream_is_not_estimated(void)
{
    bk7259_gimbal_stats_t stats = {0};
    bk7259_gimbal_stats_snapshot_t snapshot;
    bk7259_gimbal_stats_reset(&stats, 100);
    for (int16_t i = 0; i < 45; ++i) {
        uint32_t sent_at = 100U + (uint32_t)i * 40U;
        bk7259_gimbal_stats_tx(&stats, sent_at, BK7259_GIMBAL_STATS_NORMAL, i, 0);
        bk7259_gimbal_stats_reply(&stats, sent_at + 8, 0, 0, 0);
    }
    bk7259_gimbal_stats_snapshot(&stats, 2200, &snapshot);
    const bk7259_gimbal_stats_counts_t *normal = &snapshot.groups[BK7259_GIMBAL_STATS_NORMAL];
    assert(normal->tx == 45 && normal->accepted == 45 && normal->estimated == 0);
    assert(normal->pending == 0 && normal->expired == 0);
    assert(snapshot.latency_samples == 45 && snapshot.latency_total_ms == 45 * 8);
    assert(snapshot.latency_min_ms == 8 && snapshot.latency_max_ms == 8);
    assert(snapshot.latency_median_twice_ms == 16);
}

static void test_two_pending_short_replies_are_estimated(void)
{
    bk7259_gimbal_stats_t stats = {0};
    bk7259_gimbal_stats_snapshot_t snapshot;
    bk7259_gimbal_stats_reset(&stats, 100);
    bk7259_gimbal_stats_tx(&stats, 100, BK7259_GIMBAL_STATS_NORMAL, 20, 0);
    bk7259_gimbal_stats_tx(&stats, 140, BK7259_GIMBAL_STATS_NORMAL, 40, 0);
    bk7259_gimbal_stats_reply(&stats, 145, 0, 0, 0);
    bk7259_gimbal_stats_reply(&stats, 150, 4, 0, 0);
    bk7259_gimbal_stats_snapshot(&stats, 151, &snapshot);
    const bk7259_gimbal_stats_counts_t *normal = &snapshot.groups[BK7259_GIMBAL_STATS_NORMAL];
    assert(normal->accepted == 2 && normal->estimated == 2 && normal->pending == 0);
    assert(snapshot.latency_samples == 0 && snapshot.latency_median_twice_ms == 0);
}

static void test_completion_fallback_preserves_late_short_ambiguity(void)
{
    bk7259_gimbal_stats_t stats = {0};
    bk7259_gimbal_stats_snapshot_t snapshot;
    bk7259_gimbal_stats_reset(&stats, 100);
    bk7259_gimbal_stats_tx(&stats, 100, BK7259_GIMBAL_STATS_NORMAL, 20, 0);
    bk7259_gimbal_stats_reply(&stats, 120, 6, 20, 0);
    bk7259_gimbal_stats_tx(&stats, 140, BK7259_GIMBAL_STATS_NORMAL, 40, 0);
    bk7259_gimbal_stats_reply(&stats, 145, 0, 0, 0);
    bk7259_gimbal_stats_snapshot(&stats, 146, &snapshot);
    const bk7259_gimbal_stats_counts_t *normal = &snapshot.groups[BK7259_GIMBAL_STATS_NORMAL];
    assert(normal->accepted == 1 && normal->pending == 1 && snapshot.ambiguous == 1);
    bk7259_gimbal_stats_reply(&stats, 150, 6, 40, 0);
    bk7259_gimbal_stats_snapshot(&stats, 151, &snapshot);
    assert(normal->accepted == 2 && normal->completion_fallback == 2 && normal->estimated == 0);
    assert(normal->pending == 0 && snapshot.latency_samples == 0);
}

static void test_expired_short_reply_keeps_new_request_pending(void)
{
    const uint8_t replies[] = {0, 1, 2, 4};
    for (unsigned int i = 0; i < sizeof(replies) / sizeof(replies[0]); ++i) {
        bk7259_gimbal_stats_t stats = {0};
        bk7259_gimbal_stats_snapshot_t snapshot;
        bk7259_gimbal_stats_reset(&stats, 100);
        bk7259_gimbal_stats_tx(&stats, 100, BK7259_GIMBAL_STATS_NORMAL, 20, 0);
        bk7259_gimbal_stats_tx(&stats, 400, BK7259_GIMBAL_STATS_HOME, 0, 0);
        bk7259_gimbal_stats_reply(&stats, 405, 0, 0, 0);
        bk7259_gimbal_stats_snapshot(&stats, 406, &snapshot);
        const bk7259_gimbal_stats_counts_t *home = &snapshot.groups[BK7259_GIMBAL_STATS_HOME];
        assert(snapshot.groups[BK7259_GIMBAL_STATS_NORMAL].expired == 1);
        assert(home->accepted == 0 && home->rejected == 0 && home->pending == 1);
        assert(snapshot.ambiguous == 1 && snapshot.latency_samples == 0);

        uint8_t code = replies[i];
        bk7259_gimbal_stats_reply(&stats, 410, code, 0, 0);
        bk7259_gimbal_stats_snapshot(&stats, 411, &snapshot);
        bool rejected = code == 1 || code == 2;
        assert(home->accepted == !rejected && home->rejected == rejected);
        assert(home->pending == 0 && home->estimated == 1 && snapshot.latency_samples == 0);
    }
}

static void test_ambiguous_pending_expiry_does_not_absorb_again(void)
{
    bk7259_gimbal_stats_t stats = {0};
    bk7259_gimbal_stats_snapshot_t snapshot;
    bk7259_gimbal_stats_reset(&stats, 100);
    bk7259_gimbal_stats_tx(&stats, 100, BK7259_GIMBAL_STATS_NORMAL, 20, 0);
    bk7259_gimbal_stats_tx(&stats, 400, BK7259_GIMBAL_STATS_NORMAL, 40, 0);
    bk7259_gimbal_stats_reply(&stats, 405, 0, 0, 0);
    bk7259_gimbal_stats_tx(&stats, 700, BK7259_GIMBAL_STATS_NORMAL, 60, 0);
    bk7259_gimbal_stats_reply(&stats, 705, 4, 0, 0);
    bk7259_gimbal_stats_snapshot(&stats, 706, &snapshot);
    const bk7259_gimbal_stats_counts_t *normal = &snapshot.groups[BK7259_GIMBAL_STATS_NORMAL];
    assert(normal->accepted == 1 && normal->expired == 2 && normal->pending == 0);
    assert(snapshot.ambiguous == 1 && snapshot.latency_samples == 1);
    assert(snapshot.latency_total_ms == 5);
}

static void test_timeout_and_timer_wrap(void)
{
    bk7259_gimbal_stats_t stats = {0};
    bk7259_gimbal_stats_snapshot_t snapshot;
    const uint32_t start = UINT32_MAX - 100U;
    bk7259_gimbal_stats_reset(&stats, start);
    bk7259_gimbal_stats_tx(&stats, start, BK7259_GIMBAL_STATS_NORMAL, 30, 0);
    bk7259_gimbal_stats_tx(&stats, start, BK7259_GIMBAL_STATS_HOME, 0, 0);
    bk7259_gimbal_stats_snapshot(&stats, start + 250U, &snapshot);
    assert(snapshot.groups[BK7259_GIMBAL_STATS_NORMAL].pending == 1);

    bk7259_gimbal_stats_reply(&stats, start + 251U, 0, 0, 0);
    bk7259_gimbal_stats_reply(&stats, start + 900U, 6, 0, 0);
    bk7259_gimbal_stats_snapshot(&stats, start + 900U, &snapshot);
    assert(snapshot.window_ms == 900U && snapshot.ambiguous == 1);
    assert(snapshot.groups[BK7259_GIMBAL_STATS_NORMAL].expired == 1);
    assert(snapshot.groups[BK7259_GIMBAL_STATS_HOME].accepted == 1);
    assert(snapshot.latency_samples == 0);
}

static void test_reset_excludes_old_replies(void)
{
    bk7259_gimbal_stats_t stats = {0};
    bk7259_gimbal_stats_snapshot_t snapshot;
    bk7259_gimbal_stats_reset(&stats, 100);
    bk7259_gimbal_stats_tx(&stats, 101, BK7259_GIMBAL_STATS_HOME, 0, 0);
    bk7259_gimbal_stats_reset(&stats, 110);
    bk7259_gimbal_stats_tx(&stats, 111, BK7259_GIMBAL_STATS_NORMAL, 10, 0);
    bk7259_gimbal_stats_reply(&stats, 112, 0, 0, 0);
    bk7259_gimbal_stats_reply(&stats, 113, 6, 0, 0);
    bk7259_gimbal_stats_snapshot(&stats, 113, &snapshot);
    assert(snapshot.suppressed == 2 && snapshot.groups[BK7259_GIMBAL_STATS_NORMAL].pending == 1);
    assert(snapshot.groups[BK7259_GIMBAL_STATS_NORMAL].accepted == 0);
    bk7259_gimbal_stats_reply(&stats, 114, 4, 0, 0);
    bk7259_gimbal_stats_tx(&stats, 150, BK7259_GIMBAL_STATS_NORMAL, 20, 0);
    bk7259_gimbal_stats_reply(&stats, 153, 0, 0, 0);
    bk7259_gimbal_stats_snapshot(&stats, 154, &snapshot);
    assert(snapshot.suppressed == 2 && snapshot.e1_06 == 1 && snapshot.e1_00 == 2);
    assert(snapshot.groups[BK7259_GIMBAL_STATS_HOME].tx == 0);
    assert(snapshot.groups[BK7259_GIMBAL_STATS_NORMAL].accepted == 2);
    assert(snapshot.groups[BK7259_GIMBAL_STATS_NORMAL].estimated == 1);
    assert(snapshot.latency_samples == 1 && snapshot.latency_total_ms == 3);

    bk7259_gimbal_stats_reset(&stats, 155);
    bk7259_gimbal_stats_tx(&stats, 156, BK7259_GIMBAL_STATS_NORMAL, 30, 0);
    bk7259_gimbal_stats_reply(&stats, 159, 0, 0, 0);
    bk7259_gimbal_stats_snapshot(&stats, 159, &snapshot);
    assert(snapshot.suppressed == 0 && snapshot.latency_samples == 1);
    assert(snapshot.groups[BK7259_GIMBAL_STATS_NORMAL].accepted == 1);
}

static void test_fast_stream_recovers_after_expiry(void)
{
    bk7259_gimbal_stats_t stats = {0};
    bk7259_gimbal_stats_snapshot_t snapshot;
    bk7259_gimbal_stats_reset(&stats, 100);
    bk7259_gimbal_stats_tx(&stats, 100, BK7259_GIMBAL_STATS_NORMAL, 0, 0);
    bk7259_gimbal_stats_tick(&stats, 400);
    for (unsigned int i = 0; i < 25; ++i) {
        uint32_t sent_at = 400U + i * 40U;
        bk7259_gimbal_stats_tx(&stats, sent_at, BK7259_GIMBAL_STATS_NORMAL, (int16_t)i, 1);
        bk7259_gimbal_stats_reply(&stats, sent_at + 2, 0, 0, 0);
    }
    bk7259_gimbal_stats_snapshot(&stats, 1700, &snapshot);
    const bk7259_gimbal_stats_counts_t *normal = &snapshot.groups[BK7259_GIMBAL_STATS_NORMAL];
    assert(normal->tx == 26 && normal->accepted == 24 && normal->expired == 2);
    assert(normal->pending == 0 && snapshot.ambiguous == 1);
}

static void test_capacity_conservatively_rejects_matches(void)
{
    bk7259_gimbal_stats_t stats = {0};
    bk7259_gimbal_stats_snapshot_t snapshot;
    bk7259_gimbal_stats_reset(&stats, 100);
    bk7259_gimbal_stats_stage(&stats, BK7259_GIMBAL_TEST_SAD);
    for (unsigned int i = 0; i < BK7259_GIMBAL_STATS_CAPACITY; ++i)
        bk7259_gimbal_stats_tx(&stats, 100, BK7259_GIMBAL_STATS_NORMAL, (int16_t)i, 0);
    bk7259_gimbal_stats_stage(&stats, BK7259_GIMBAL_TEST_ANGRY);
    bk7259_gimbal_stats_tx(&stats, 100, BK7259_GIMBAL_STATS_NORMAL, BK7259_GIMBAL_STATS_CAPACITY, 0);
    bk7259_gimbal_stats_reply(&stats, 120, 0, 0, 0);
    bk7259_gimbal_stats_reply(&stats, 130, 6, 4, 0);
    bk7259_gimbal_stats_snapshot(&stats, 131, &snapshot);
    const bk7259_gimbal_stats_counts_t *normal = &snapshot.groups[BK7259_GIMBAL_STATS_NORMAL];
    assert(normal->tx == BK7259_GIMBAL_STATS_CAPACITY + 1U);
    assert(normal->pending == BK7259_GIMBAL_STATS_CAPACITY && normal->overflow == 1);
    assert(normal->accepted == 0 && snapshot.ambiguous == 2);
    assert(snapshot.test_counts[BK7259_GIMBAL_TEST_SAD].tx == BK7259_GIMBAL_STATS_CAPACITY);
    assert(snapshot.test_counts[BK7259_GIMBAL_TEST_SAD].overflow == 0);
    assert(snapshot.test_counts[BK7259_GIMBAL_TEST_ANGRY].tx == 1);
    assert(snapshot.test_counts[BK7259_GIMBAL_TEST_ANGRY].overflow == 1);

    bk7259_gimbal_stats_tx(&stats, 1001, BK7259_GIMBAL_STATS_NORMAL, 1, 0);
    bk7259_gimbal_stats_reply(&stats, 1006, 0, 0, 0);
    bk7259_gimbal_stats_snapshot(&stats, 1006, &snapshot);
    assert(normal->pending == 0 && normal->expired == BK7259_GIMBAL_STATS_CAPACITY);
    assert(normal->accepted == 1 && snapshot.latency_samples == 1);
    assert(snapshot.test_counts[BK7259_GIMBAL_TEST_SAD].expired == BK7259_GIMBAL_STATS_CAPACITY);
    assert(snapshot.test_counts[BK7259_GIMBAL_TEST_ANGRY].expired == 0);
}

static void test_results_follow_transmit_stage(void)
{
    bk7259_gimbal_stats_t stats = {0};
    bk7259_gimbal_stats_snapshot_t snapshot;
    bk7259_gimbal_stats_reset(&stats, 100);
    bk7259_gimbal_stats_stage(&stats, BK7259_GIMBAL_TEST_OPENING);
    bk7259_gimbal_stats_tx(&stats, 100, BK7259_GIMBAL_STATS_NORMAL, 10, 0);
    bk7259_gimbal_stats_stage(&stats, BK7259_GIMBAL_TEST_HAPPY);
    bk7259_gimbal_stats_tx(&stats, 114, BK7259_GIMBAL_STATS_HOME, 0, 0);
    bk7259_gimbal_stats_reply(&stats, 115, 0, 0, 0);
    bk7259_gimbal_stats_reply(&stats, 125, 2, 0, 0);
    bk7259_gimbal_stats_tx(&stats, 130, BK7259_GIMBAL_STATS_HOME, 1, 0);
    bk7259_gimbal_stats_tx(&stats, 131, BK7259_GIMBAL_STATS_NORMAL, 20, 0);
    bk7259_gimbal_stats_tx_failed(&stats, BK7259_GIMBAL_STATS_HOME);

    bk7259_gimbal_stats_stage(&stats, BK7259_GIMBAL_TEST_NONE);
    bk7259_gimbal_stats_tx(&stats, 140, BK7259_GIMBAL_STATS_NORMAL, 30, 0);
    bk7259_gimbal_stats_reply(&stats, 150, 6, 1, 0);
    bk7259_gimbal_stats_reply(&stats, 160, 6, 30, 0);
    bk7259_gimbal_stats_tx_failed(&stats, BK7259_GIMBAL_STATS_NORMAL);
    bk7259_gimbal_stats_snapshot(&stats, 400, &snapshot);

    const bk7259_gimbal_test_counts_t *opening = &snapshot.test_counts[BK7259_GIMBAL_TEST_OPENING];
    const bk7259_gimbal_test_counts_t *happy = &snapshot.test_counts[BK7259_GIMBAL_TEST_HAPPY];
    const bk7259_gimbal_test_counts_t *outside = &snapshot.test_counts[BK7259_GIMBAL_TEST_NONE];
    assert(opening->tx == 1 && opening->accepted == 1);
    assert(opening->rejected == 0 && opening->expired == 0 && opening->tx_failed == 0);
    assert(happy->tx == 3 && happy->accepted == 1 && happy->rejected == 1);
    assert(happy->expired == 1 && happy->overflow == 0 && happy->tx_failed == 1);
    assert(outside->tx == 0 && outside->accepted == 0 && outside->tx_failed == 0);
    assert(snapshot.groups[BK7259_GIMBAL_STATS_NORMAL].accepted == 2);
    assert(snapshot.groups[BK7259_GIMBAL_STATS_NORMAL].tx_failed == 1);
}

static void test_stage_reset_excludes_previous_requests(void)
{
    bk7259_gimbal_stats_t stats = {0};
    bk7259_gimbal_stats_snapshot_t snapshot;
    bk7259_gimbal_stats_reset(&stats, 100);
    bk7259_gimbal_stats_stage(&stats, BK7259_GIMBAL_TEST_OPENING);
    bk7259_gimbal_stats_tx(&stats, 101, BK7259_GIMBAL_STATS_HOME, 0, 0);
    bk7259_gimbal_stats_reset(&stats, 102);
    bk7259_gimbal_stats_tx_failed(&stats, BK7259_GIMBAL_STATS_NORMAL);
    bk7259_gimbal_stats_stage(&stats, BK7259_GIMBAL_TEST_OPENING);
    bk7259_gimbal_stats_tx(&stats, 103, BK7259_GIMBAL_STATS_NORMAL, 50, 0);
    bk7259_gimbal_stats_reply(&stats, 104, 6, 0, 0);
    bk7259_gimbal_stats_reply(&stats, 105, 0, 0, 0);
    bk7259_gimbal_stats_reply(&stats, 106, 0, 0, 0);
    bk7259_gimbal_stats_snapshot(&stats, 400, &snapshot);

    const bk7259_gimbal_test_counts_t *opening = &snapshot.test_counts[BK7259_GIMBAL_TEST_OPENING];
    assert(opening->tx == 1 && opening->accepted == 1);
    assert(opening->rejected == 0 && opening->expired == 0 && opening->tx_failed == 0);
    assert(snapshot.suppressed == 2);
}

static void test_stage_motion_gap_uses_consecutive_successful_motion(void)
{
    bk7259_gimbal_stats_t stats = {0};
    bk7259_gimbal_stats_snapshot_t snapshot;
    const uint32_t start = UINT32_MAX - 20U;
    bk7259_gimbal_stats_reset(&stats, start);
    bk7259_gimbal_stats_stage(&stats, BK7259_GIMBAL_TEST_OPENING);
    bk7259_gimbal_stats_tx(&stats, start, BK7259_GIMBAL_STATS_NORMAL, 10, 0);
    bk7259_gimbal_stats_power_tx(&stats, start + 3, 1, true);
    bk7259_gimbal_stats_tx_failed(&stats, BK7259_GIMBAL_STATS_NORMAL);
    bk7259_gimbal_stats_stage(&stats, BK7259_GIMBAL_TEST_OPENING);
    bk7259_gimbal_stats_tx(&stats, start + 10, BK7259_GIMBAL_STATS_NORMAL, 20, 0);
    bk7259_gimbal_stats_tx(&stats, start + 15, BK7259_GIMBAL_STATS_NORMAL, 25, 0);
    bk7259_gimbal_stats_tx(&stats, start + 20, BK7259_GIMBAL_STATS_HOME, 0, 0);
    bk7259_gimbal_stats_tx(&stats, start + 22, BK7259_GIMBAL_STATS_NORMAL, 30, 0);
    bk7259_gimbal_stats_tx_failed(&stats, BK7259_GIMBAL_STATS_HOME);
    bk7259_gimbal_stats_power_reply(&stats, start + 25, 0);
    bk7259_gimbal_stats_tx(&stats, start + 32, BK7259_GIMBAL_STATS_NORMAL, 35, 0);
    bk7259_gimbal_stats_snapshot(&stats, start + 32, &snapshot);
    const bk7259_gimbal_test_counts_t *opening = &snapshot.test_counts[BK7259_GIMBAL_TEST_OPENING];
    const bk7259_gimbal_test_counts_t *listening = &snapshot.test_counts[BK7259_GIMBAL_TEST_LISTENING];
    assert(opening->tx == 6 && opening->tx_failed == 2);
    assert(opening->motion_pairs == 3 && opening->min_motion_gap_ms == 5);

    bk7259_gimbal_stats_stage(&stats, BK7259_GIMBAL_TEST_LISTENING);
    bk7259_gimbal_stats_tx(&stats, start + 35, BK7259_GIMBAL_STATS_NORMAL, 40, 0);
    bk7259_gimbal_stats_tx(&stats, start + 42, BK7259_GIMBAL_STATS_NORMAL, 45, 0);
    bk7259_gimbal_stats_stage(&stats, BK7259_GIMBAL_TEST_OPENING);
    bk7259_gimbal_stats_tx(&stats, start + 48, BK7259_GIMBAL_STATS_NORMAL, 50, 0);
    bk7259_gimbal_stats_tx(&stats, start + 49, BK7259_GIMBAL_STATS_NORMAL, 51, 0);
    bk7259_gimbal_stats_stage(&stats, BK7259_GIMBAL_TEST_NONE);
    bk7259_gimbal_stats_tx(&stats, start + 50, BK7259_GIMBAL_STATS_NORMAL, 52, 0);
    bk7259_gimbal_stats_snapshot(&stats, start + 50, &snapshot);
    assert(opening->motion_pairs == 4 && opening->min_motion_gap_ms == 1);
    assert(listening->motion_pairs == 1 && listening->min_motion_gap_ms == 7);
    assert(snapshot.test_counts[BK7259_GIMBAL_TEST_NONE].motion_pairs == 0);

    bk7259_gimbal_stats_reset(&stats, start + 52);
    bk7259_gimbal_stats_stage(&stats, BK7259_GIMBAL_TEST_OPENING);
    bk7259_gimbal_stats_tx(&stats, start + 53, BK7259_GIMBAL_STATS_NORMAL, 60, 0);
    bk7259_gimbal_stats_snapshot(&stats, start + 53, &snapshot);
    assert(opening->motion_pairs == 0 && opening->min_motion_gap_ms == 0);
    bk7259_gimbal_stats_tx(&stats, start + 53, BK7259_GIMBAL_STATS_NORMAL, 61, 0);
    bk7259_gimbal_stats_tx(&stats, start + 55, BK7259_GIMBAL_STATS_NORMAL, 62, 0);
    bk7259_gimbal_stats_snapshot(&stats, start + 55, &snapshot);
    assert(opening->motion_pairs == 2 && opening->min_motion_gap_ms == 0);
}

static void test_latency_median_uses_only_isolated_short_replies(void)
{
    bk7259_gimbal_stats_t stats = {0};
    bk7259_gimbal_stats_snapshot_t snapshot;
    bk7259_gimbal_stats_reset(&stats, 100);
    bk7259_gimbal_stats_snapshot(&stats, 100, &snapshot);
    assert(snapshot.latency_samples == 0 && snapshot.latency_median_twice_ms == 0);

    bk7259_gimbal_stats_tx(&stats, 101, BK7259_GIMBAL_STATS_NORMAL, 1, 0);
    bk7259_gimbal_stats_reply(&stats, 109, 0, 0, 0);
    bk7259_gimbal_stats_snapshot(&stats, 109, &snapshot);
    assert(snapshot.latency_median_twice_ms == 16);

    bk7259_gimbal_stats_tx(&stats, 1100, BK7259_GIMBAL_STATS_NORMAL, 2, 0);
    bk7259_gimbal_stats_reply(&stats, 1103, 0, 0, 0);
    bk7259_gimbal_stats_snapshot(&stats, 1103, &snapshot);
    assert(snapshot.latency_median_twice_ms == 11);

    bk7259_gimbal_stats_tx(&stats, 2100, BK7259_GIMBAL_STATS_NORMAL, 3, 0);
    bk7259_gimbal_stats_reply(&stats, 2108, 0, 0, 0);
    bk7259_gimbal_stats_snapshot(&stats, 2108, &snapshot);
    assert(snapshot.latency_median_twice_ms == 16);

    bk7259_gimbal_stats_tx(&stats, 3100, BK7259_GIMBAL_STATS_NORMAL, 4, 0);
    bk7259_gimbal_stats_reply(&stats, 3100, 4, 0, 0);
    bk7259_gimbal_stats_snapshot(&stats, 3100, &snapshot);
    assert(snapshot.latency_median_twice_ms == 11);

    bk7259_gimbal_stats_tx(&stats, 4100, BK7259_GIMBAL_STATS_HOME, 0, 0);
    bk7259_gimbal_stats_reply(&stats, 4100 + BK7259_GIMBAL_STATS_HOME_WINDOW_MS, 4, 0, 0);
    bk7259_gimbal_stats_tx(&stats, 5100, BK7259_GIMBAL_STATS_NORMAL, 5, 0);
    bk7259_gimbal_stats_reply(&stats, 5100 + BK7259_GIMBAL_STATS_NORMAL_WINDOW_MS + 1U, 0, 0, 0);
    bk7259_gimbal_stats_snapshot(&stats, 5400, &snapshot);
    assert(snapshot.latency_samples == 5 && snapshot.latency_min_ms == 0);
    assert(snapshot.latency_max_ms == 900 && snapshot.latency_total_ms == 919);
    assert(snapshot.latency_median_twice_ms == 16);
    assert(snapshot.groups[BK7259_GIMBAL_STATS_NORMAL].expired == 1);

    bk7259_gimbal_stats_reset(&stats, 5400);
    bk7259_gimbal_stats_snapshot(&stats, 5400, &snapshot);
    assert(snapshot.latency_samples == 0 && snapshot.latency_median_twice_ms == 0);
    bk7259_gimbal_stats_tx(&stats, 6100, BK7259_GIMBAL_STATS_HOME, 0, 0);
    bk7259_gimbal_stats_reply(&stats, 6100 + BK7259_GIMBAL_STATS_HOME_WINDOW_MS, 4, 0, 0);
    bk7259_gimbal_stats_snapshot(&stats, 7000, &snapshot);
    assert(snapshot.latency_samples == 1 && snapshot.latency_median_twice_ms == 1800);
}

static void test_power_counts_and_latency(void)
{
    bk7259_gimbal_stats_t stats = {0};
    bk7259_gimbal_stats_snapshot_t snapshot;
    bk7259_gimbal_stats_reset(&stats, 100);
    bk7259_gimbal_stats_power_tx(&stats, 100, 1, true);
    bk7259_gimbal_stats_power_reply(&stats, 108, 0);
    bk7259_gimbal_stats_power_tx(&stats, 200, 0, true);
    bk7259_gimbal_stats_power_reply(&stats, 213, 0);
    bk7259_gimbal_stats_snapshot(&stats, 214, &snapshot);
    assert(snapshot.power.latency_median_twice_ms == 21);

    bk7259_gimbal_stats_power_tx(&stats, 300, 1, true);
    bk7259_gimbal_stats_power_reply(&stats, 312, 2);
    bk7259_gimbal_stats_power_tx(&stats, 400, 0, false);
    bk7259_gimbal_stats_power_tx(&stats, 500, 2, true);
    bk7259_gimbal_stats_power_bad_reply(&stats);
    bk7259_gimbal_stats_power_reply(&stats, 505, 2);
    bk7259_gimbal_stats_power_tx(&stats, 506, 2, false);
    bk7259_gimbal_stats_power_tx(&stats, 600, 1, true);
    bk7259_gimbal_stats_power_bad_reply(&stats);
    bk7259_gimbal_stats_snapshot(&stats, 850, &snapshot);
    assert(snapshot.power.groups[BK7259_GIMBAL_POWER_REQUEST].pending == 1);
    bk7259_gimbal_stats_power_reply(&stats, 870, 0);
    bk7259_gimbal_stats_snapshot(&stats, 900, &snapshot);

    const bk7259_gimbal_power_stats_t *power = &snapshot.power;
    const bk7259_gimbal_power_counts_t *stop = &power->groups[BK7259_GIMBAL_POWER_STOP];
    const bk7259_gimbal_power_counts_t *request = &power->groups[BK7259_GIMBAL_POWER_REQUEST];
    assert(stop->tx == 1 && stop->tx_failed == 1 && stop->accepted == 1);
    assert(stop->pending == 0 && stop->expired == 0 && stop->rejected == 0);
    assert(request->tx == 3 && request->accepted == 1 && request->rejected == 1);
    assert(request->pending == 0 && request->expired == 1 && request->tx_failed == 0);
    assert(power->e6_00 == 2 && power->e6_02 == 1 && power->bad_reply == 1);
    assert(power->late == 1 && power->unmatched == 0);
    assert(power->latency_samples == 3 && power->latency_min_ms == 8);
    assert(power->latency_max_ms == 13 && power->latency_total_ms == 33);
    assert(power->latency_median_twice_ms == 24 && snapshot.latency_samples == 0);
}

static void test_power_previous_reply_cannot_settle_new_request(void)
{
    bk7259_gimbal_stats_t stats = {0};
    bk7259_gimbal_stats_snapshot_t snapshot;
    bk7259_gimbal_stats_reset(&stats, 100);
    bk7259_gimbal_stats_power_tx(&stats, 100, 1, true);
    bk7259_gimbal_stats_reset(&stats, 105);
    bk7259_gimbal_stats_power_reply(&stats, 110, 0);
    bk7259_gimbal_stats_power_tx(&stats, 200, 1, true);
    bk7259_gimbal_stats_power_tx(&stats, 451, 0, true);
    bk7259_gimbal_stats_power_reply(&stats, 455, 0);
    bk7259_gimbal_stats_snapshot(&stats, 456, &snapshot);
    const bk7259_gimbal_power_stats_t *power = &snapshot.power;
    assert(power->suppressed == 1 && power->late == 1 && power->e6_00 == 0);
    assert(power->groups[BK7259_GIMBAL_POWER_STOP].pending == 1);
    assert(power->groups[BK7259_GIMBAL_POWER_REQUEST].tx == 1);
    assert(power->groups[BK7259_GIMBAL_POWER_REQUEST].expired == 1);
    bk7259_gimbal_stats_power_reply(&stats, 460, 0);
    bk7259_gimbal_stats_snapshot(&stats, 461, &snapshot);
    assert(power->groups[BK7259_GIMBAL_POWER_STOP].accepted == 1);
    assert(power->latency_samples == 0);

    bk7259_gimbal_stats_power_tx(&stats, 500, 2, true);
    bk7259_gimbal_stats_power_tx(&stats, 751, 1, true);
    bk7259_gimbal_stats_power_reply(&stats, 755, 2);
    bk7259_gimbal_stats_snapshot(&stats, 756, &snapshot);
    assert(power->groups[BK7259_GIMBAL_POWER_REQUEST].rejected == 0);
    assert(power->groups[BK7259_GIMBAL_POWER_REQUEST].pending == 1 && power->e6_02 == 0);
    bk7259_gimbal_stats_power_reply(&stats, 760, 0);
    bk7259_gimbal_stats_snapshot(&stats, 761, &snapshot);
    assert(power->groups[BK7259_GIMBAL_POWER_REQUEST].accepted == 1);
    assert(power->latency_samples == 0);
}

static void test_power_latency_window_and_wrap(void)
{
    bk7259_gimbal_stats_t stats = {0};
    bk7259_gimbal_stats_snapshot_t snapshot;
    const uint32_t start = UINT32_MAX - 100U;
    bk7259_gimbal_stats_reset(&stats, start);
    bk7259_gimbal_stats_power_tx(&stats, start, 1, true);
    bk7259_gimbal_stats_power_reply(&stats, start + BK7259_GIMBAL_STATS_POWER_WINDOW_MS, 0);
    bk7259_gimbal_stats_power_tx(&stats, start + 1000U, 0, true);
    bk7259_gimbal_stats_power_reply(&stats, start + 1000U, 0);
    bk7259_gimbal_stats_snapshot(&stats, start + 1001U, &snapshot);
    const bk7259_gimbal_power_stats_t *power = &snapshot.power;
    assert(power->latency_min_ms == 0 && power->latency_max_ms == 250);
    assert(power->latency_samples == 2 && power->latency_total_ms == 250);
    assert(power->latency_median_twice_ms == 250);

    bk7259_gimbal_stats_reset(&stats, start + 1010U);
    bk7259_gimbal_stats_snapshot(&stats, start + 1011U, &snapshot);
    assert(power->latency_samples == 0 && power->latency_median_twice_ms == 0);
    assert(power->groups[BK7259_GIMBAL_POWER_STOP].accepted == 0);
}

int main(void)
{
    test_ack_codes_and_fallback();
    test_repeated_target_and_fifo_estimate();
    test_serial_stream_is_not_estimated();
    test_two_pending_short_replies_are_estimated();
    test_completion_fallback_preserves_late_short_ambiguity();
    test_expired_short_reply_keeps_new_request_pending();
    test_ambiguous_pending_expiry_does_not_absorb_again();
    test_timeout_and_timer_wrap();
    test_reset_excludes_old_replies();
    test_fast_stream_recovers_after_expiry();
    test_capacity_conservatively_rejects_matches();
    test_results_follow_transmit_stage();
    test_stage_reset_excludes_previous_requests();
    test_stage_motion_gap_uses_consecutive_successful_motion();
    test_latency_median_uses_only_isolated_short_replies();
    test_power_counts_and_latency();
    test_power_previous_reply_cannot_settle_new_request();
    test_power_latency_window_and_wrap();
    puts("PASS: EB521 response statistics, overlap, expiry, reset, and capacity");
    return 0;
}
