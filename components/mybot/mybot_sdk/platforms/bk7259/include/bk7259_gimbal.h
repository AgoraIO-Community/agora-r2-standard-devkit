/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BK7259_GIMBAL_H_
#define BK7259_GIMBAL_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BK7259_GIMBAL_SCENE_HAPPY = 0,
    BK7259_GIMBAL_SCENE_SAD,
    BK7259_GIMBAL_SCENE_ANGRY,
    BK7259_GIMBAL_SCENE_SURPRISED,
    BK7259_GIMBAL_SCENE_COUNT,
} bk7259_gimbal_scene_t;

typedef enum {
    BK7259_GIMBAL_PHASE_NEUTRAL = 0,
    BK7259_GIMBAL_PHASE_LISTENING,
    BK7259_GIMBAL_PHASE_THINKING,
    BK7259_GIMBAL_PHASE_SPEAKING,
} bk7259_gimbal_phase_t;

enum {
    BK7259_GIMBAL_BUSY = -2,
    BK7259_GIMBAL_NOT_READY = -4,
    BK7259_GIMBAL_MOTION_DISABLED = -5,
};

typedef struct {
    bool motion_enabled;
    bool charge_enabled;
    bool pending;
    bool save_failed;
} bk7259_gimbal_settings_t;

typedef enum {
    BK7259_GIMBAL_STATS_NORMAL = 0,
    BK7259_GIMBAL_STATS_HOME,
    BK7259_GIMBAL_STATS_GROUP_COUNT,
} bk7259_gimbal_stats_group_t;

typedef struct {
    uint32_t reached, rejected, expired, cancelled, pending;
} bk7259_gimbal_arrival_counts_t;

typedef struct {
    bk7259_gimbal_arrival_counts_t arrival;
    uint32_t tx;
    uint32_t tx_failed;
    uint32_t accepted;
    uint32_t rejected;
    uint32_t pending;
    uint32_t expired;
    uint32_t overflow;
    uint32_t estimated;
    uint32_t completion_fallback;
} bk7259_gimbal_stats_counts_t;

typedef enum {
    BK7259_GIMBAL_TEST_NONE,
    BK7259_GIMBAL_TEST_OPENING,
    BK7259_GIMBAL_TEST_LISTENING,
    BK7259_GIMBAL_TEST_THINKING,
    BK7259_GIMBAL_TEST_SPEAKING,
    BK7259_GIMBAL_TEST_NEUTRAL,
    BK7259_GIMBAL_TEST_HANGUP,
    BK7259_GIMBAL_TEST_HAPPY,
    BK7259_GIMBAL_TEST_SAD,
    BK7259_GIMBAL_TEST_ANGRY,
    BK7259_GIMBAL_TEST_SURPRISED,
    BK7259_GIMBAL_TEST_STAGE_COUNT
} bk7259_gimbal_test_stage_t;

typedef enum {
    BK7259_GIMBAL_TEST_IDLE,
    BK7259_GIMBAL_TEST_SCHEDULED,
    BK7259_GIMBAL_TEST_RUNNING,
    BK7259_GIMBAL_TEST_COMPLETED,
    BK7259_GIMBAL_TEST_INTERRUPTED
} bk7259_gimbal_test_state_t;

#define BK7259_GIMBAL_LIMIT_STEP_COUNT 17U

typedef enum {
    BK7259_GIMBAL_LIMIT_NOT_RUN,
    BK7259_GIMBAL_LIMIT_PENDING,
    BK7259_GIMBAL_LIMIT_REACHED,
    BK7259_GIMBAL_LIMIT_TIMEOUT,
    BK7259_GIMBAL_LIMIT_REJECTED,
    BK7259_GIMBAL_LIMIT_TX_FAILED,
    BK7259_GIMBAL_LIMIT_CANCELLED,
} bk7259_gimbal_limit_result_t;

typedef struct {
    int16_t pan, pitch; /* Absolute targets in tenths of a degree. */
    uint16_t pan_pps, pitch_pps;
    uint16_t ack_ms, arrival_ms;
    uint8_t result; /* bk7259_gimbal_limit_result_t */
    uint8_t tx;
    bool has_ack, has_arrival;
} bk7259_gimbal_limit_step_t;

typedef struct {
    uint32_t tx, accepted, rejected, expired, overflow, tx_failed;
    bk7259_gimbal_arrival_counts_t arrival;
    uint32_t motion_pairs;
    uint32_t min_motion_gap_ms;
} bk7259_gimbal_test_counts_t;

#define BK7259_GIMBAL_STATS_NORMAL_WINDOW_MS 250U
#define BK7259_GIMBAL_ARRIVAL_MAX_MS 2000U
#define BK7259_GIMBAL_STATS_HOME_WINDOW_MS 900U
#define BK7259_GIMBAL_TX_GUARD_MS 40U
#define BK7259_GIMBAL_MOTION_PERIOD_MS 100U
#define BK7259_GIMBAL_STATS_POWER_WINDOW_MS 250U

typedef enum {
    BK7259_GIMBAL_POWER_STOP = 0,
    BK7259_GIMBAL_POWER_REQUEST,
    BK7259_GIMBAL_POWER_GROUP_COUNT,
} bk7259_gimbal_power_group_t;

typedef struct {
    uint32_t tx, tx_failed, accepted, rejected, pending, expired;
} bk7259_gimbal_power_counts_t;

typedef struct {
    bk7259_gimbal_power_counts_t groups[BK7259_GIMBAL_POWER_GROUP_COUNT];
    uint32_t e6_00;
    uint32_t e6_02;
    uint32_t bad_reply;
    uint32_t late;
    uint32_t unmatched;
    uint32_t suppressed;
    uint32_t latency_samples;
    uint32_t latency_min_ms;
    uint32_t latency_max_ms;
    uint32_t latency_median_twice_ms;
    uint64_t latency_total_ms;
} bk7259_gimbal_power_stats_t;

typedef struct {
    bk7259_gimbal_test_state_t test_state;
    bk7259_gimbal_test_stage_t test_stage;
    bool limit_test;
    bk7259_gimbal_limit_step_t limit_steps[BK7259_GIMBAL_LIMIT_STEP_COUNT];
    bk7259_gimbal_test_counts_t test_counts[BK7259_GIMBAL_TEST_STAGE_COUNT];
    bk7259_gimbal_stats_counts_t groups[BK7259_GIMBAL_STATS_GROUP_COUNT];
    bk7259_gimbal_power_stats_t power;
    uint32_t arrival_latency_samples;
    uint32_t arrival_latency_min_ms;
    uint32_t arrival_latency_max_ms;
    uint32_t arrival_latency_median_twice_ms;
    uint64_t arrival_latency_total_ms;
    uint32_t window_ms;
    uint32_t center_confirmed;
    uint32_t move_to_charge_pairs;
    uint32_t charge_to_move_pairs;
    uint32_t min_move_to_charge_idle_ms;
    uint32_t min_charge_to_move_idle_ms;
    uint32_t e1_00;
    uint32_t e1_01;
    uint32_t e1_02;
    uint32_t e1_04;
    uint32_t e1_06;
    uint32_t bad_reply;
    uint32_t unmatched;
    uint32_t ambiguous;
    uint32_t suppressed;
    uint32_t latency_samples;
    uint32_t latency_min_ms;
    uint32_t latency_max_ms;
    /* Double the exact median to preserve the half millisecond for even counts. */
    uint32_t latency_median_twice_ms;
    uint64_t latency_total_ms;
} bk7259_gimbal_stats_snapshot_t;

#if defined(CONFIG_MYBOT_GIMBAL) && CONFIG_MYBOT_GIMBAL
/* Product-owned UART2 worker. Preparation does not wait for motor homing. */
int bk7259_gimbal_prepare(void);
/* Publish dialogue activity and phase. Duplicate phases preserve timing.
 * While centering or disabled the driver retains the phase and reports
 * NOT_READY or MOTION_DISABLED, respectively. */
int bk7259_gimbal_update_dialogue(bool active, bk7259_gimbal_phase_t phase);
/* Queue the next of four absolute targets or a scene, replacing older motion.
 * Returns 0 queued, -1 unavailable, NOT_READY while centering, or
 * MOTION_DISABLED when the motion setting is off. */
int bk7259_gimbal_send_next_command(void);
int bk7259_gimbal_request(bk7259_gimbal_scene_t scene);
/* Queue a link diagnostic. Returns 0 queued, -2 pending, or -1 unavailable. */
int bk7259_gimbal_request_probe(void);
/* Read or reset the in-memory UART motion counters. Return 0 on success. */
int bk7259_gimbal_get_stats(bk7259_gimbal_stats_snapshot_t *out);
int bk7259_gimbal_reset_stats(void);
int bk7259_gimbal_schedule_test(void);
int bk7259_gimbal_schedule_limit_test(void);
/* Cancel motion and wait for confirmed center. Skips centering while disabled. */
int bk7259_gimbal_center_wait(uint32_t timeout_ms);
/* Publish requested policies without waiting for flash or UART. Switching
 * motion off queues one final home without requiring an ACK. Restoring an
 * already-disabled setting at boot does not move the servos. A repeated
 * request retries a failed save. get_settings snapshots the latest selection. */
int bk7259_gimbal_request_motion_enabled(bool enabled);
int bk7259_gimbal_set_charging(bool enabled);
int bk7259_gimbal_get_settings(bk7259_gimbal_settings_t *settings);
/* Disable charging for product shutdown and wait for an explicit stop ACK
 * after any enable attempt. Enabling is blocked until platform preparation. */
int bk7259_gimbal_stop_charging_wait(uint32_t timeout_ms);
/* A failed worker-exit wait retains UART and its resources for a later retry. */
int bk7259_gimbal_shutdown(void);
#else
static inline int bk7259_gimbal_prepare(void) { return 0; }
static inline int bk7259_gimbal_update_dialogue(bool active, bk7259_gimbal_phase_t phase)
{ (void)active; (void)phase; return 0; }
static inline int bk7259_gimbal_send_next_command(void) { return -1; }
static inline int bk7259_gimbal_request_probe(void) { return -1; }
static inline int bk7259_gimbal_get_stats(bk7259_gimbal_stats_snapshot_t *out)
{ (void)out; return -1; }
static inline int bk7259_gimbal_reset_stats(void) { return -1; }
static inline int bk7259_gimbal_schedule_test(void) { return -1; }
static inline int bk7259_gimbal_schedule_limit_test(void) { return -1; }
static inline int bk7259_gimbal_request(bk7259_gimbal_scene_t scene)
{ (void)scene; return 0; }
static inline int bk7259_gimbal_center_wait(uint32_t timeout_ms)
{ (void)timeout_ms; return 0; }
static inline int bk7259_gimbal_request_motion_enabled(bool enabled)
{ (void)enabled; return -1; }
static inline int bk7259_gimbal_set_charging(bool enabled)
{ (void)enabled; return -1; }
static inline int bk7259_gimbal_get_settings(bk7259_gimbal_settings_t *settings)
{ (void)settings; return -1; }
static inline int bk7259_gimbal_stop_charging_wait(uint32_t timeout_ms)
{ (void)timeout_ms; return 0; }
static inline int bk7259_gimbal_shutdown(void) { return 0; }
#endif

#ifdef __cplusplus
}
#endif
#endif /* BK7259_GIMBAL_H_ */
