/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BK7259_GIMBAL_STATS_H_
#define BK7259_GIMBAL_STATS_H_

#include "bk7259_gimbal.h"

#define BK7259_GIMBAL_STATS_CAPACITY 48U

typedef enum {
    BK7259_GIMBAL_STATS_PENDING,
    BK7259_GIMBAL_STATS_ACCEPTED,
    BK7259_GIMBAL_STATS_ACCEPTED_BY_COMPLETION,
    BK7259_GIMBAL_STATS_REJECTED,
    BK7259_GIMBAL_STATS_EXPIRED,
} bk7259_gimbal_stats_request_state_t;

typedef struct {
    uint32_t sent_at;
    uint16_t arrival_window_ms;
    bool arrival_pending;
    bool arrived;
    int16_t pan;
    int16_t pitch;
    bk7259_gimbal_test_stage_t test_stage;
    uint8_t group;
    uint8_t state;
    bool overlapping;
    bool previous_run;
    /* A short reply may already belong here. Pending requests still match
     * future replies, but must not absorb another reply after expiry. */
    bool short_uncertain;
} bk7259_gimbal_stats_request_t;

typedef struct {
    uint32_t sent_at;
    uint8_t control;
    bool pending;
    bool previous_run;
    bool latency_uncertain;
} bk7259_gimbal_stats_power_request_t;

/* Owned by the UART worker or accessed under its mailbox lock. */
typedef struct {
    bk7259_gimbal_stats_snapshot_t snapshot;
    uint32_t arrival_histogram[BK7259_GIMBAL_ARRIVAL_MAX_MS + 1U];
    uint32_t latency_histogram[BK7259_GIMBAL_STATS_HOME_WINDOW_MS + 1U];
    uint32_t power_latency_histogram[BK7259_GIMBAL_STATS_POWER_WINDOW_MS + 1U];
    bk7259_gimbal_stats_power_request_t power_request;
    bk7259_gimbal_stats_power_request_t expired_power_request;
    bk7259_gimbal_stats_request_t requests[BK7259_GIMBAL_STATS_CAPACITY];
    uint8_t used;
    uint32_t started_at;
    uint32_t overflow_at;
    uint32_t overflow_window_ms;
    bk7259_gimbal_test_stage_t test_stage;
    uint32_t last_test_motion_tx_at;
    bool has_test_motion_tx;
    bool overflow_guard;
} bk7259_gimbal_stats_t;

void bk7259_gimbal_stats_reset(bk7259_gimbal_stats_t *stats, uint32_t now);
/* Select the test stage for the next send attempt. Outstanding requests keep
 * the stage they were sent in, even after switching or leaving the test. */
void bk7259_gimbal_stats_stage(bk7259_gimbal_stats_t *stats, bk7259_gimbal_test_stage_t stage);
void bk7259_gimbal_stats_tick(bk7259_gimbal_stats_t *stats, uint32_t now);
void bk7259_gimbal_stats_tx(bk7259_gimbal_stats_t *stats, uint32_t now,
                           bk7259_gimbal_stats_group_t group, int16_t pan, int16_t pitch,
                           uint32_t arrival_window_ms);
void bk7259_gimbal_stats_cancel_arrival(bk7259_gimbal_stats_t *stats, uint32_t sent_at);
void bk7259_gimbal_stats_cancel_test_arrivals(bk7259_gimbal_stats_t *stats, uint32_t now);
void bk7259_gimbal_stats_expire_arrival(bk7259_gimbal_stats_t *stats, uint32_t sent_at);
void bk7259_gimbal_stats_tx_failed(bk7259_gimbal_stats_t *stats, bk7259_gimbal_stats_group_t group);
/* A match is valid until the next statistics operation. */
const bk7259_gimbal_stats_request_t *bk7259_gimbal_stats_reply(
    bk7259_gimbal_stats_t *stats, uint32_t now, uint8_t code, int16_t pan, int16_t pitch,
    bool allow_home_arrival);
void bk7259_gimbal_stats_bad_reply(bk7259_gimbal_stats_t *stats);
/* Track diagnostic control 02 for reply correlation without counting it. */
void bk7259_gimbal_stats_power_tx(bk7259_gimbal_stats_t *stats, uint32_t now,
                                 uint8_t control, bool sent);
void bk7259_gimbal_stats_power_reply(bk7259_gimbal_stats_t *stats, uint32_t now, uint8_t code);
void bk7259_gimbal_stats_power_bad_reply(bk7259_gimbal_stats_t *stats);
void bk7259_gimbal_stats_snapshot(bk7259_gimbal_stats_t *stats, uint32_t now,
                                 bk7259_gimbal_stats_snapshot_t *output);

#endif
