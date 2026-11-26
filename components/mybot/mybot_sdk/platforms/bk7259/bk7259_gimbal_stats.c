/* SPDX-License-Identifier: Apache-2.0 */
#include "bk7259_gimbal_stats.h"

#include <stddef.h>
#include <string.h>

#define POWER_PROBE_CONTROL 2U

static uint32_t response_window(uint8_t group)
{
    return group == BK7259_GIMBAL_STATS_HOME ? BK7259_GIMBAL_STATS_HOME_WINDOW_MS
                                            : BK7259_GIMBAL_STATS_NORMAL_WINDOW_MS;
}

static bk7259_gimbal_test_counts_t *test_counts_for(bk7259_gimbal_stats_t *stats,
                                                   bk7259_gimbal_test_stage_t stage)
{
    if (stage == BK7259_GIMBAL_TEST_NONE || (unsigned int)stage >= BK7259_GIMBAL_TEST_STAGE_COUNT)
        return NULL;
    return &stats->snapshot.test_counts[stage];
}

static void expire_motion_request(bk7259_gimbal_stats_t *stats,
                                  bk7259_gimbal_stats_request_t *request)
{
    if (!request->previous_run) {
        bk7259_gimbal_stats_counts_t *counts = &stats->snapshot.groups[request->group];
        --counts->pending;
        ++counts->expired;
        bk7259_gimbal_test_counts_t *test_counts = test_counts_for(stats, request->test_stage);
        if (test_counts)
            ++test_counts->expired;
    }
    request->state = BK7259_GIMBAL_STATS_EXPIRED;
}

static void expire_power_request(bk7259_gimbal_stats_t *stats)
{
    bk7259_gimbal_stats_power_request_t *request = &stats->power_request;
    if (!request->previous_run && request->control != POWER_PROBE_CONTROL) {
        bk7259_gimbal_power_counts_t *counts = &stats->snapshot.power.groups[request->control];
        --counts->pending;
        ++counts->expired;
    }
    stats->expired_power_request = *request;
    request->pending = false;
}

static void advance_power(bk7259_gimbal_stats_t *stats, uint32_t now)
{
    if (stats->power_request.pending &&
        now - stats->power_request.sent_at > BK7259_GIMBAL_STATS_POWER_WINDOW_MS)
        expire_power_request(stats);
    /* Leave an additional reply window for recognizing a possible late E6
     * when another command was already sent. */
    if (stats->expired_power_request.pending &&
        now - stats->expired_power_request.sent_at > 2U * BK7259_GIMBAL_STATS_POWER_WINDOW_MS)
        stats->expired_power_request.pending = false;
}

void bk7259_gimbal_stats_tick(bk7259_gimbal_stats_t *stats, uint32_t now)
{
    stats->snapshot.window_ms = now - stats->started_at;
    advance_power(stats, now);
    if (now - stats->overflow_at > BK7259_GIMBAL_STATS_HOME_WINDOW_MS)
        stats->overflow_guard = false;

    size_t kept = 0;
    for (size_t i = 0; i < stats->used; ++i) {
        bk7259_gimbal_stats_request_t request = stats->requests[i];
        uint32_t age = now - request.sent_at;
        if (request.state == BK7259_GIMBAL_STATS_PENDING && age > response_window(request.group))
            expire_motion_request(stats, &request);
        /* Keep completed/expired targets for one bounded UART reply horizon.
         * A matching old target could otherwise masquerade as a new 06. */
        if (age <= BK7259_GIMBAL_STATS_HOME_WINDOW_MS)
            stats->requests[kept++] = request;
    }
    stats->used = (uint8_t)kept;
}

void bk7259_gimbal_stats_reset(bk7259_gimbal_stats_t *stats, uint32_t now)
{
    bk7259_gimbal_stats_tick(stats, now);
    for (size_t i = 0; i < stats->used; ++i) {
        bk7259_gimbal_stats_request_t *request = &stats->requests[i];
        request->previous_run = true;
        request->test_stage = BK7259_GIMBAL_TEST_NONE;
    }
    memset(&stats->snapshot, 0, sizeof(stats->snapshot));
    memset(stats->latency_histogram, 0, sizeof(stats->latency_histogram));
    memset(stats->power_latency_histogram, 0, sizeof(stats->power_latency_histogram));
    stats->power_request.previous_run = true;
    stats->expired_power_request.previous_run = true;
    stats->test_stage = BK7259_GIMBAL_TEST_NONE;
    stats->has_test_motion_tx = false;
    stats->started_at = now;
}

void bk7259_gimbal_stats_stage(bk7259_gimbal_stats_t *stats, bk7259_gimbal_test_stage_t stage)
{
    if (stats->test_stage != stage)
        stats->has_test_motion_tx = false;
    stats->test_stage = stage;
}

static void record_test_motion_gap(bk7259_gimbal_stats_t *stats,
                                   bk7259_gimbal_test_counts_t *counts,
                                   bk7259_gimbal_stats_group_t group, uint32_t now)
{
    if (!counts || group != BK7259_GIMBAL_STATS_NORMAL) {
        stats->has_test_motion_tx = false;
        return;
    }
    if (stats->has_test_motion_tx) {
        uint32_t gap = now - stats->last_test_motion_tx_at;
        if (!counts->motion_pairs || gap < counts->min_motion_gap_ms)
            counts->min_motion_gap_ms = gap;
        ++counts->motion_pairs;
    }
    stats->last_test_motion_tx_at = now;
    stats->has_test_motion_tx = true;
}

void bk7259_gimbal_stats_tx(bk7259_gimbal_stats_t *stats, uint32_t now,
                           bk7259_gimbal_stats_group_t group, int16_t pan, int16_t pitch)
{
    bk7259_gimbal_stats_tick(stats, now);
    bk7259_gimbal_stats_counts_t *counts = &stats->snapshot.groups[group];
    bk7259_gimbal_test_counts_t *test_counts = test_counts_for(stats, stats->test_stage);
    ++counts->tx;
    if (test_counts)
        ++test_counts->tx;
    record_test_motion_gap(stats, test_counts, group, now);
    bool overlapping = stats->overflow_guard;
    for (size_t i = 0; i < stats->used; ++i) {
        bk7259_gimbal_stats_request_t *previous = &stats->requests[i];
        uint32_t age = now - previous->sent_at;
        /* RX is processed before the next ordinary submission. Settlement
         * can include the deadline tick, but the next send closes that window. */
        if (group == BK7259_GIMBAL_STATS_NORMAL && previous->group == group &&
            previous->state == BK7259_GIMBAL_STATS_PENDING &&
            age == BK7259_GIMBAL_STATS_NORMAL_WINDOW_MS) {
            expire_motion_request(stats, previous);
        }
        bool awaits_short_reply = previous->state == BK7259_GIMBAL_STATS_PENDING ||
                                  (previous->state == BK7259_GIMBAL_STATS_ACCEPTED_BY_COMPLETION &&
                                   !previous->short_uncertain);
        if (!previous->previous_run && awaits_short_reply &&
            age <= response_window(previous->group)) {
            previous->overlapping = true;
            overlapping = true;
        }
    }
    if (stats->used == BK7259_GIMBAL_STATS_CAPACITY) {
        ++counts->overflow;
        if (test_counts)
            ++test_counts->overflow;
        stats->overflow_guard = true;
        stats->overflow_at = now;
        return;
    }
    stats->requests[stats->used++] = (bk7259_gimbal_stats_request_t){
        .sent_at = now,
        .pan = pan,
        .pitch = pitch,
        .test_stage = stats->test_stage,
        .group = (uint8_t)group,
        .state = BK7259_GIMBAL_STATS_PENDING,
        .overlapping = overlapping,
    };
    ++counts->pending;
}

void bk7259_gimbal_stats_tx_failed(bk7259_gimbal_stats_t *stats, bk7259_gimbal_stats_group_t group)
{
    ++stats->snapshot.groups[group].tx_failed;
    bk7259_gimbal_test_counts_t *test_counts = test_counts_for(stats, stats->test_stage);
    if (test_counts)
        ++test_counts->tx_failed;
}

static bk7259_gimbal_stats_request_t *first_pending(bk7259_gimbal_stats_t *stats)
{
    for (size_t i = 0; i < stats->used; ++i) {
        if (stats->requests[i].state == BK7259_GIMBAL_STATS_PENDING)
            return &stats->requests[i];
    }
    return NULL;
}

static bk7259_gimbal_stats_request_t *match_short_reply(bk7259_gimbal_stats_t *stats)
{
    bk7259_gimbal_stats_request_t *request = first_pending(stats);
    /* An untagged reply may still belong to a predecessor. Keep the newer
     * candidate eligible for subsequent replies, but record the ambiguity
     * so it cannot propagate to another request if this one expires. */
    bk7259_gimbal_stats_request_t *limit = request ? request : stats->requests + stats->used;
    for (bk7259_gimbal_stats_request_t *previous = stats->requests; previous < limit; ++previous) {
        bool possible_short_reply = previous->state == BK7259_GIMBAL_STATS_EXPIRED ||
                                    previous->state == BK7259_GIMBAL_STATS_ACCEPTED_BY_COMPLETION;
        if (possible_short_reply && !previous->short_uncertain) {
            previous->short_uncertain = true;
            if (request)
                request->short_uncertain = true;
            if (previous->previous_run)
                ++stats->snapshot.suppressed;
            else if (request || previous->state == BK7259_GIMBAL_STATS_EXPIRED)
                ++stats->snapshot.ambiguous;
            return NULL;
        }
    }
    if (!request) {
        ++stats->snapshot.unmatched;
        return NULL;
    }
    if (stats->overflow_guard) {
        ++stats->snapshot.ambiguous;
        return NULL;
    }
    return request;
}

static bk7259_gimbal_stats_request_t *match_completion(bk7259_gimbal_stats_t *stats,
                                                     int16_t pan, int16_t pitch)
{
    bk7259_gimbal_stats_request_t *match = NULL;
    for (size_t i = 0; i < stats->used; ++i) {
        bk7259_gimbal_stats_request_t *request = &stats->requests[i];
        if (request->pan != pan || request->pitch != pitch)
            continue;
        if (match || stats->overflow_guard) {
            ++stats->snapshot.ambiguous;
            return NULL;
        }
        match = request;
    }
    if (!match) {
        ++stats->snapshot.unmatched;
        return NULL;
    }
    if (match->previous_run && match->state != BK7259_GIMBAL_STATS_PENDING) {
        ++stats->snapshot.suppressed;
        return NULL;
    }
    /* A normal transition from accepted to completed is observed in raw 06
     * without counting the request again or reporting an unknown reply. */
    if (match->state == BK7259_GIMBAL_STATS_ACCEPTED ||
        match->state == BK7259_GIMBAL_STATS_ACCEPTED_BY_COMPLETION)
        return NULL;
    if (match->state != BK7259_GIMBAL_STATS_PENDING) {
        ++stats->snapshot.unmatched;
        return NULL;
    }
    return match;
}

static void record_latency(bk7259_gimbal_stats_t *stats, uint32_t elapsed)
{
    bk7259_gimbal_stats_snapshot_t *snapshot = &stats->snapshot;
    if (!snapshot->latency_samples || elapsed < snapshot->latency_min_ms)
        snapshot->latency_min_ms = elapsed;
    if (elapsed > snapshot->latency_max_ms)
        snapshot->latency_max_ms = elapsed;
    ++snapshot->latency_samples;
    snapshot->latency_total_ms += elapsed;
    ++stats->latency_histogram[elapsed];
}

const bk7259_gimbal_stats_request_t *bk7259_gimbal_stats_reply(
    bk7259_gimbal_stats_t *stats, uint32_t now, uint8_t code, int16_t pan, int16_t pitch)
{
    bk7259_gimbal_stats_tick(stats, now);
    switch (code) {
    case 0: ++stats->snapshot.e1_00; break;
    case 1: ++stats->snapshot.e1_01; break;
    case 2: ++stats->snapshot.e1_02; break;
    case 4: ++stats->snapshot.e1_04; break;
    case 6: ++stats->snapshot.e1_06; break;
    default:
        bk7259_gimbal_stats_bad_reply(stats);
        return NULL;
    }

    bk7259_gimbal_stats_request_t *request =
        code == 6 ? match_completion(stats, pan, pitch) : match_short_reply(stats);
    if (!request)
        return NULL;

    bool rejected = code == 1 || code == 2;
    if (rejected)
        request->state = BK7259_GIMBAL_STATS_REJECTED;
    else if (code == 6)
        request->state = BK7259_GIMBAL_STATS_ACCEPTED_BY_COMPLETION;
    else
        request->state = BK7259_GIMBAL_STATS_ACCEPTED;
    if (request->previous_run) {
        ++stats->snapshot.suppressed;
        return request;
    }
    bk7259_gimbal_stats_counts_t *counts = &stats->snapshot.groups[request->group];
    bk7259_gimbal_test_counts_t *test_counts = test_counts_for(stats, request->test_stage);
    --counts->pending;
    if (test_counts) {
        if (rejected)
            ++test_counts->rejected;
        else
            ++test_counts->accepted;
    }
    if (rejected) {
        ++counts->rejected;
    } else if (code == 6) {
        ++counts->accepted;
        ++counts->completion_fallback;
    } else {
        ++counts->accepted;
    }
    /* A unique completion can attribute success by its coordinates even
     * when short responses from overlapping requests cannot be separated. */
    if (code == 6)
        return request;
    if (request->overlapping || request->short_uncertain)
        ++counts->estimated;
    else
        record_latency(stats, now - request->sent_at);
    return request;
}

void bk7259_gimbal_stats_bad_reply(bk7259_gimbal_stats_t *stats)
{
    ++stats->snapshot.bad_reply;
}

void bk7259_gimbal_stats_power_tx(bk7259_gimbal_stats_t *stats, uint32_t now,
                                 uint8_t control, bool sent)
{
    if (control > POWER_PROBE_CONTROL)
        return;
    advance_power(stats, now);
    if (control != POWER_PROBE_CONTROL) {
        bk7259_gimbal_power_counts_t *counts = &stats->snapshot.power.groups[control];
        if (sent) {
            ++counts->tx;
            ++counts->pending;
        } else {
            ++counts->tx_failed;
        }
    }
    if (!sent)
        return;
    if (stats->power_request.pending)
        expire_power_request(stats);
    stats->power_request = (bk7259_gimbal_stats_power_request_t){
        .sent_at = now,
        .control = control,
        .pending = true,
    };
}

void bk7259_gimbal_stats_power_bad_reply(bk7259_gimbal_stats_t *stats)
{
    if (!stats->power_request.pending || stats->power_request.control != POWER_PROBE_CONTROL)
        ++stats->snapshot.power.bad_reply;
}

static void record_power_latency(bk7259_gimbal_stats_t *stats, uint32_t elapsed)
{
    bk7259_gimbal_power_stats_t *power = &stats->snapshot.power;
    if (!power->latency_samples || elapsed < power->latency_min_ms)
        power->latency_min_ms = elapsed;
    if (elapsed > power->latency_max_ms)
        power->latency_max_ms = elapsed;
    ++power->latency_samples;
    power->latency_total_ms += elapsed;
    ++stats->power_latency_histogram[elapsed];
}

void bk7259_gimbal_stats_power_reply(bk7259_gimbal_stats_t *stats, uint32_t now, uint8_t code)
{
    advance_power(stats, now);
    if (code != 0 && code != 2) {
        bk7259_gimbal_stats_power_bad_reply(stats);
        return;
    }

    bk7259_gimbal_power_stats_t *power = &stats->snapshot.power;
    bk7259_gimbal_stats_power_request_t *previous = &stats->expired_power_request;
    bk7259_gimbal_stats_power_request_t *request = &stats->power_request;
    if (previous->pending && (previous->control != POWER_PROBE_CONTROL || code == 2)) {
        previous->pending = false;
        if (previous->control != POWER_PROBE_CONTROL) {
            if (previous->previous_run)
                ++power->suppressed;
            else
                ++power->late;
        }
        /* An old E6 has no identifier. Keep the current request open without
         * treating a subsequent reply as a latency sample for it. */
        request->latency_uncertain = request->pending;
        return;
    }
    if (!request->pending) {
        ++power->unmatched;
        return;
    }
    if (request->control == POWER_PROBE_CONTROL) {
        if (code == 2)
            request->pending = false;
        return;
    }
    request->pending = false;
    if (request->previous_run) {
        ++power->suppressed;
        return;
    }

    bk7259_gimbal_power_counts_t *counts = &power->groups[request->control];
    --counts->pending;
    if (code == 0) {
        ++counts->accepted;
        ++power->e6_00;
    } else {
        ++counts->rejected;
        ++power->e6_02;
    }
    if (!request->latency_uncertain)
        record_power_latency(stats, now - request->sent_at);
}

static uint32_t latency_median_twice(const uint32_t *histogram, uint32_t samples,
                                     uint32_t min_ms, uint32_t max_ms)
{
    if (!samples)
        return 0;
    uint32_t lower_rank = (samples - 1U) / 2U;
    uint32_t upper_rank = samples / 2U;
    uint32_t median = 0;
    uint64_t seen = 0;
    for (uint32_t ms = min_ms; ms <= max_ms; ++ms) {
        uint64_t next = seen + histogram[ms];
        if (seen <= lower_rank && lower_rank < next)
            median = ms;
        if (upper_rank < next)
            return median + ms;
        seen = next;
    }
    return median;
}

void bk7259_gimbal_stats_snapshot(bk7259_gimbal_stats_t *stats, uint32_t now,
                                 bk7259_gimbal_stats_snapshot_t *output)
{
    bk7259_gimbal_stats_tick(stats, now);
    *output = stats->snapshot;
    output->latency_median_twice_ms = latency_median_twice(stats->latency_histogram,
        output->latency_samples, output->latency_min_ms, output->latency_max_ms);
    bk7259_gimbal_power_stats_t *power = &output->power;
    power->latency_median_twice_ms = latency_median_twice(stats->power_latency_histogram,
        power->latency_samples, power->latency_min_ms, power->latency_max_ms);
}
