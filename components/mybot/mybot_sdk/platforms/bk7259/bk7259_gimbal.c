/* SPDX-License-Identifier: Apache-2.0 */
#include "bk7259_gimbal.h"
#include "bk7259_gimbal_prefs.h"
#include "bk7259_gimbal_stats.h"
#include "bk7259_platform_log.h"

#include <api/aosl.h>
#include <bk_private/bk_uart.h>
#include <common/bk_err.h>
#include <driver/gpio.h>
#include <driver/uart.h>
#include "gpio_driver.h"
#include <math.h>
#include <os/os.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define TAG "bk7259_gimbal"
#define GIMBAL_UART UART_ID_2
#define GIMBAL_BAUD 115200U
#define FRAME_BYTES 200U
#define STACK_BYTES 4096U
#define RX_POLL_MS 5U
#define IDLE_WAIT_MS 20U
#define STOP_TIMEOUT_MS 3000U
#define TEST_START_DELAY_MS 10000U
#define TEST_HOME_TIMEOUT_MS 5000U
#define LIMIT_DWELL_MS 500U
#define LIMIT_PPS 1000U
#define LIMIT_DIAGONAL_PITCH_PPS 250U
#define HOME_PERIOD_MS 1000U
#define HOME_RESPONSE_MS 900U
#define HOME_GUARD_MS 250U
#define CHARGE_PERIOD_MS 2000U
#define CHARGE_ACK_TIMEOUT_MS BK7259_GIMBAL_STATS_POWER_WINDOW_MS
#define CHARGE_EXPIRY_MS 3000U
#define LINK_LOSS_MS (CHARGE_EXPIRY_MS + CHARGE_ACK_TIMEOUT_MS)
#define CONTROL_PERIOD_MS BK7259_GIMBAL_MOTION_PERIOD_MS
#define ARRIVAL_MARGIN_MS 100U
#define CENTER_TIME_MS 280U
#define PAN_LIMIT 800
#define PITCH_UP_LIMIT (-200)
#define PITCH_DOWN_LIMIT 100
#define DIALOGUE_MAX_STEP 40
#define MOTION_BLEND_MS 240U
#define MOTION_LIMIT_TENTHS_PER_S 1600U
#define LISTENING_INTERVAL_MS 3500U
#define LISTENING_LEG_MS 560U
#define THINKING_TURN_MS 700U
#define SPEAKING_PERIOD_MS 1250U
#define EB521_MOVE 0x61
#define EB521_CHARGE 0x66
#define EB521_MOVE_REPLY 0xe1
#define EB521_CHARGE_REPLY 0xe6
#define EB521_PROBE_CONTROL 2U

typedef enum {
    MOTION_NONE,
    MOTION_DIALOGUE,
    MOTION_SCENE,
    MOTION_TARGET,
} motion_kind_t;

typedef struct {
    motion_kind_t kind;
    uint32_t revision;
    uint32_t started_at;
    union {
        bk7259_gimbal_scene_t scene;
        struct {
            int16_t pan;
            int16_t pitch;
        } target;
    };
} motion_request_t;

typedef struct {
    bool prepared;
    bool accepting;
    bool stopping;
    bool ready;
    bool conversation;
    bool opening_pending;
    bool disable_home_pending;
    bk7259_gimbal_settings_t settings;
    bk7259_gimbal_stats_t stats;
    bool stats_has_tx;
    struct {
        bk7259_gimbal_test_state_t state;
        bk7259_gimbal_test_stage_t stage;
        uint32_t due_at;
        uint32_t expected_home;
        unsigned limit_index;
        uint32_t limit_sent_at;
        bk7259_gimbal_stats_snapshot_t result;
    } test;
    uint32_t settings_revision;
    bool charge_changed;
    bool charge_locked;
    bool charge_off_pending;
    bool pending_probe;
    bool probe_running;
    bool uart_owned;
    bool tx_mapped;
    bool rx_mapped;
    bool aosl_owned;
    size_t next_command;
    uint32_t home_ticket;
    uint32_t centered_ticket;
    uint32_t charge_off_ticket;
    uint32_t charge_off_completed;
    int charge_off_result;
    bk7259_gimbal_phase_t phase;
    uint32_t phase_at;
    motion_request_t motion;
    beken_thread_t worker;
} gimbal_context_t;

typedef struct {
    int16_t start;
    int16_t target;
    uint16_t ms_per_degree;
} pose_axis_t;

/* UART, parser and trajectory timing belong to the worker. */
typedef struct {
    uint8_t rx[FRAME_BYTES];
    size_t used;
    uint32_t home_ticket;
    uint32_t home_due;
    uint32_t home_sent_at;
    uint32_t home_fence_at;
    bool home_waiting;
    bool home_accepts_short_reply;
    bool home_needs_fence;
    bool home_has_fence;
    bool home_sent;
    bool disable_home_sent;
    bool sent_ordinary_motion;
    bool confirmed_at_center;
    bool recent_ordinary_zero;
    uint32_t ordinary_zero_at;

    uint32_t charge_due;
    uint32_t last_tx_return_at;
    uint8_t last_tx_command;
    uint32_t charge_sent_at;
    uint32_t last_link_rx_at;
    uint32_t last_on_at;
    uint32_t stats_tick_at;
    uint32_t charge_wire_ticket;
    uint32_t probe_rx_bytes;
    uint8_t charge_wire_control;
    bool charge_waiting;
    bool on_reply_uncertain;
    bool off_reply_uncertain;
    bool charge_confirmed;
    bool probe_echo;

    uint32_t motion_revision;
    uint32_t motion_due;
    uint32_t path_elapsed_ms;
    int16_t path_start_pan;
    int16_t path_start_pitch;
    uint8_t path_segment;
    bool opening;
    bool target_attempted;
    bool attempted_point;
    bool has_motion_attempt;
    uint32_t motion_attempt_at;
    int16_t planned_pan;
    int16_t planned_pitch;
    bool waiting_motion_ack;
    uint32_t motion_arrival_window_ms;
    pose_axis_t pose_pan;
    pose_axis_t pose_pitch;
    uint32_t pose_started_at;
    uint32_t blend_at;
    uint32_t blend_ms;
    int16_t blend_pan;
    int16_t blend_pitch;
} gimbal_io_t;

static gimbal_context_t s_gimbal;
/* The worker owns UART until exit is observed. These notifications remain
 * available across shutdown retries and another platform preparation. */
static beken_mutex_t s_lock;
static beken_semaphore_t s_wake;
static beken_semaphore_t s_done;
static beken_semaphore_t s_centered;

static const uart_config_t k_uart_config = {
    .baud_rate = GIMBAL_BAUD,
    .data_bits = UART_DATA_8_BITS,
    .parity = UART_PARITY_NONE,
    .stop_bits = UART_STOP_BITS_1,
    .flow_ctrl = UART_FLOWCTRL_DISABLE,
    .src_clk = UART_SCLK_XTAL_26M,
    .rx_dma_en = UART_DMA_DISABLE,
    .tx_dma_en = UART_DMA_DISABLE,
};

static void unlock(void)
{
    if (rtos_unlock_mutex(&s_lock) != BK_OK)
        MYBOT_LOGE(TAG, "mailbox unlock failed");
}

static void notify(beken_semaphore_t *event)
{
    /* These live binary semaphores already represent a wake when full. */
    (void)rtos_set_semaphore(event);
}

static bool due(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static bool mailbox_initialized(void)
{
    return __atomic_load_n(&s_lock, __ATOMIC_ACQUIRE) != NULL;
}

static uint16_t crc16(const uint8_t *bytes, size_t count)
{
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < count; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc & 1U) ? (crc >> 1) ^ 0xa001U : crc >> 1;
    }
    return crc;
}

static int16_t read_i16(const uint8_t *bytes)
{
    return (int16_t)((uint16_t)bytes[0] | (uint16_t)bytes[1] << 8);
}

static void write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

/* At FIFO-empty, one byte can still be shifting out. Budget that byte plus
 * the new frame at 8N1, rounded up, with one extra tick for clock phase. */
static unsigned tx_tail_ms(uint8_t command)
{
    unsigned bytes = command == EB521_MOVE ? 14U : 7U;
    return ((bytes + 1U) * 10U * 1000U + GIMBAL_BAUD - 1U) / GIMBAL_BAUD + 1U;
}

static uint32_t bus_idle_ms(const gimbal_io_t *io, uint32_t now)
{
    uint32_t elapsed = now - io->last_tx_return_at;
    unsigned tail = tx_tail_ms(io->last_tx_command);
    return elapsed > tail ? elapsed - tail : 0;
}

static bool heartbeat_enabled(void)
{
    return !s_gimbal.charge_locked ||
           (s_gimbal.settings.motion_enabled && !s_gimbal.ready && !s_gimbal.stopping);
}

/* Check before consuming any queued work or advancing a motion deadline. */
static bool tx_ready(const gimbal_io_t *io, uint8_t command)
{
    uint32_t now = rtos_get_time();
    if (io->last_tx_command && command != io->last_tx_command &&
        bus_idle_ms(io, now) < BK7259_GIMBAL_TX_GUARD_MS)
        return false;
    if (command == EB521_MOVE && !io->charge_waiting) {
        uint32_t reserved_until = now + tx_tail_ms(EB521_MOVE) + BK7259_GIMBAL_TX_GUARD_MS;
        if (s_gimbal.charge_off_pending || s_gimbal.pending_probe ||
            (heartbeat_enabled() && (s_gimbal.charge_changed || due(reserved_until, io->charge_due))))
            return false;
    }
    return bk_uart_is_tx_over(GIMBAL_UART);
}

/* The single worker has secured the UART for this write. */
static bool send_frame(gimbal_io_t *io, uint8_t command, const uint8_t *payload, size_t length)
{
    uint8_t frame[14] = {0x03, command, 0, (uint8_t)length};
    if (length > sizeof(frame) - 6)
        return false;
    memcpy(frame + 4, payload, length);
    write_u16(frame + 4 + length, crc16(frame, length + 4));
    uint32_t started_at = rtos_get_time();
    if (bk_uart_write_bytes(GIMBAL_UART, frame, (uint32_t)length + 6U) != BK_OK) {
        MYBOT_LOGW(TAG, "UART write failed: command=0x%02x", (unsigned)command);
        return false;
    }
    uint32_t returned_at = rtos_get_time();
    if (s_gimbal.stats_has_tx && io->last_tx_command != command) {
        bk7259_gimbal_stats_snapshot_t *stats = &s_gimbal.stats.snapshot;
        uint32_t idle = bus_idle_ms(io, started_at);
        uint32_t *pairs = command == EB521_MOVE ? &stats->charge_to_move_pairs : &stats->move_to_charge_pairs;
        uint32_t *minimum = command == EB521_MOVE ? &stats->min_charge_to_move_idle_ms : &stats->min_move_to_charge_idle_ms;
        if (*pairs == 0 || idle < *minimum)
            *minimum = idle;
        ++*pairs;
    }
    io->last_tx_return_at = returned_at;
    io->last_tx_command = command;
    s_gimbal.stats_has_tx = true;
    return true;
}

static bool write_move(gimbal_io_t *io, bk7259_gimbal_stats_group_t group, int16_t pan, int16_t pitch,
                       uint16_t pan_speed, uint16_t pitch_speed, uint32_t arrival_window_ms)
{
    if (pan < -900 || pan > 900 || pitch < -300 || pitch > 150)
        return false;
    uint8_t payload[8];
    write_u16(payload, (uint16_t)pan);
    write_u16(payload + 2, (uint16_t)pitch);
    write_u16(payload + 4, pan_speed);
    write_u16(payload + 6, pitch_speed);
    if (!send_frame(io, EB521_MOVE, payload, sizeof(payload))) {
        bk7259_gimbal_stats_tx_failed(&s_gimbal.stats, group);
        return false;
    }
    bk7259_gimbal_stats_tx(&s_gimbal.stats, io->last_tx_return_at, group, pan, pitch, arrival_window_ms);
    return true;
}

static bool send_move(gimbal_io_t *io, bk7259_gimbal_stats_group_t group, int16_t pan, int16_t pitch,
                      uint16_t pan_speed, uint16_t pitch_speed, uint32_t arrival_window_ms)
{
    if (pan < -PAN_LIMIT || pan > PAN_LIMIT || pitch < PITCH_UP_LIMIT || pitch > PITCH_DOWN_LIMIT)
        return false;
    return write_move(io, group, pan, pitch, pan_speed, pitch_speed, arrival_window_ms);
}

static bool send_home(gimbal_io_t *io)
{
    return send_move(io, BK7259_GIMBAL_STATS_HOME, 0, 0, 1000, 1000, HOME_RESPONSE_MS);
}

/* Drop expired explicit actions at every gate transition. The phase remains
 * available as the latest live intent, without replaying its opening. */
static void start_home(void)
{
    s_gimbal.ready = false;
    s_gimbal.opening_pending = false;
    s_gimbal.motion.kind = s_gimbal.conversation ? MOTION_DIALOGUE : MOTION_NONE;
    ++s_gimbal.motion.revision;
    ++s_gimbal.home_ticket;
}

/* All test and motion state is serialized by the UART mailbox lock. */
static bool test_active(void)
{
    return s_gimbal.test.state == BK7259_GIMBAL_TEST_SCHEDULED ||
           s_gimbal.test.state == BK7259_GIMBAL_TEST_RUNNING;
}

static bool limit_running(void)
{
    return s_gimbal.test.result.limit_test && s_gimbal.test.state == BK7259_GIMBAL_TEST_RUNNING;
}

static bk7259_gimbal_limit_step_t *limit_step(void)
{
    return &s_gimbal.stats.snapshot.limit_steps[s_gimbal.test.limit_index];
}

static void finish_test(bk7259_gimbal_test_state_t state, uint32_t now)
{
    if (!test_active())
        return;
    if (s_gimbal.test.state == BK7259_GIMBAL_TEST_RUNNING) {
        if (state == BK7259_GIMBAL_TEST_INTERRUPTED) {
            bk7259_gimbal_stats_cancel_test_arrivals(&s_gimbal.stats, now);
            if (limit_running() && limit_step()->result == BK7259_GIMBAL_LIMIT_PENDING) {
                bool expired = (int32_t)(now - s_gimbal.test.due_at) > 0;
                limit_step()->result = expired ? BK7259_GIMBAL_LIMIT_TIMEOUT : BK7259_GIMBAL_LIMIT_CANCELLED;
                bk7259_gimbal_stats_cancel_arrival(&s_gimbal.stats, s_gimbal.test.limit_sent_at);
            }
        }
        bk7259_gimbal_stats_snapshot(&s_gimbal.stats, now, &s_gimbal.test.result);
        bk7259_gimbal_stats_stage(&s_gimbal.stats, BK7259_GIMBAL_TEST_NONE);
        s_gimbal.conversation = false;
        s_gimbal.opening_pending = false;
        s_gimbal.motion.kind = MOTION_NONE;
        ++s_gimbal.motion.revision;
    }
    s_gimbal.test.state = state;
    s_gimbal.test.result.test_state = state;
    s_gimbal.test.result.test_stage = s_gimbal.test.stage;
}

static void recover_home(uint32_t now)
{
    finish_test(BK7259_GIMBAL_TEST_INTERRUPTED, now);
    start_home();
}

static int update_dialogue(bool active, bk7259_gimbal_phase_t phase, uint32_t now)
{
    bool started = active && !s_gimbal.conversation;
    bool ended = !active && (s_gimbal.conversation || s_gimbal.ready);
    bool changed = active && (phase != s_gimbal.phase || s_gimbal.motion.kind != MOTION_DIALOGUE);
    if (started || ended || changed) {
        s_gimbal.conversation = active;
        s_gimbal.phase = phase;
        s_gimbal.phase_at = now;
        if (ended) {
            if (s_gimbal.ready) {
                start_home();
            } else {
                s_gimbal.motion.kind = MOTION_NONE;
                s_gimbal.opening_pending = false;
            }
        } else {
            s_gimbal.motion.kind = MOTION_DIALOGUE;
            s_gimbal.motion.started_at = s_gimbal.phase_at;
            ++s_gimbal.motion.revision;
            /* A real phase supersedes any unsent opening samples. */
            s_gimbal.opening_pending = started && s_gimbal.ready && phase == BK7259_GIMBAL_PHASE_NEUTRAL;
        }
        notify(&s_wake);
    }
    int result = 0;
    if (!s_gimbal.settings.motion_enabled)
        result = BK7259_GIMBAL_MOTION_DISABLED;
    else if (active && !s_gimbal.ready)
        result = BK7259_GIMBAL_NOT_READY;
    return result;
}

static void queue_scene(bk7259_gimbal_scene_t scene, uint32_t now)
{
    s_gimbal.opening_pending = false;
    s_gimbal.motion.kind = MOTION_SCENE;
    s_gimbal.motion.scene = scene;
    s_gimbal.motion.started_at = now;
    ++s_gimbal.motion.revision;
}

static void cancel_superseded_motion(gimbal_io_t *io, uint32_t now)
{
    if (io->waiting_motion_ack && io->motion_revision != s_gimbal.motion.revision) {
        bk7259_gimbal_stats_tick(&s_gimbal.stats, now);
        bk7259_gimbal_stats_cancel_arrival(&s_gimbal.stats, io->motion_attempt_at);
        io->waiting_motion_ack = false;
    }
}

static void sync_home(gimbal_io_t *io, uint32_t now)
{
    cancel_superseded_motion(io, now);
    if (io->home_ticket == s_gimbal.home_ticket)
        return;
    if (io->home_waiting) {
        bk7259_gimbal_stats_tick(&s_gimbal.stats, now);
        bk7259_gimbal_stats_cancel_arrival(&s_gimbal.stats, io->home_sent_at);
    }
    io->home_ticket = s_gimbal.home_ticket;
    io->home_waiting = false;
    io->home_sent = false;
    io->home_needs_fence = io->sent_ordinary_motion;
    io->home_has_fence = false;
    /* A short 04 has no position. After ordinary movements, wait for a
     * later round trip before trusting an untagged immediate response. */
    io->home_fence_at = now + (io->home_needs_fence ? HOME_GUARD_MS : 0);
    io->home_due = io->home_fence_at;
    io->waiting_motion_ack = false;
}

static int16_t estimate_axis(const pose_axis_t *axis, uint32_t elapsed)
{
    int delta = axis->target - axis->start;
    uint32_t travel = (uint32_t)abs(delta) * axis->ms_per_degree;
    if (elapsed >= (travel + 9U) / 10U)
        return axis->target;

    /* The duration check bounds this multiplication and the result stays
     * between the start and target. Angles are measured in tenths of a degree. */
    int progress = (int)(elapsed * 10U / axis->ms_per_degree);
    return (int16_t)(axis->start + (delta < 0 ? -progress : progress));
}

static void estimate_pose(const gimbal_io_t *io, uint32_t now, int16_t *pan, int16_t *pitch)
{
    uint32_t elapsed = now - io->pose_started_at;
    *pan = estimate_axis(&io->pose_pan, elapsed);
    *pitch = estimate_axis(&io->pose_pitch, elapsed);
}

/* Each submitted absolute target replaces the previous device trajectory.
 * The estimate follows the protocol speed from the preceding estimate. */
static void record_pose_command(gimbal_io_t *io, uint32_t now, int16_t target_pan,
                                int16_t target_pitch, uint16_t pan_pps, uint16_t pitch_pps)
{
    int16_t pan, pitch;
    estimate_pose(io, now, &pan, &pitch);
    io->pose_pan = (pose_axis_t){pan, target_pan, (uint16_t)(5000U / pan_pps)};
    io->pose_pitch = (pose_axis_t){pitch, target_pitch, (uint16_t)(5000U / pitch_pps)};
    io->pose_started_at = now;
}

static void await_motion_arrival(gimbal_io_t *io, int16_t pan, int16_t pitch,
                                 uint16_t pan_pps, uint16_t pitch_pps, uint32_t window)
{
    record_pose_command(io, io->last_tx_return_at, pan, pitch, pan_pps, pitch_pps);
    io->motion_attempt_at = io->last_tx_return_at;
    io->has_motion_attempt = true;
    io->motion_arrival_window_ms = window;
    io->waiting_motion_ack = true;
    io->sent_ordinary_motion = true;
    io->confirmed_at_center = false;
    if (pan == 0 && pitch == 0) {
        io->recent_ordinary_zero = true;
        io->ordinary_zero_at = io->last_tx_return_at;
    }
}

static uint32_t arrival_window(unsigned pan_distance, unsigned pitch_distance,
                               uint16_t pan_pps, uint16_t pitch_pps)
{
    uint32_t pan_ms = (pan_distance * (5000U / pan_pps) + 9U) / 10U;
    uint32_t pitch_ms = (pitch_distance * (5000U / pitch_pps) + 9U) / 10U;
    uint32_t window = (pan_ms > pitch_ms ? pan_ms : pitch_ms) + ARRIVAL_MARGIN_MS;
    if (window < BK7259_GIMBAL_STATS_NORMAL_WINDOW_MS)
        return BK7259_GIMBAL_STATS_NORMAL_WINDOW_MS;
    return window < BK7259_GIMBAL_ARRIVAL_MAX_MS ? window : BK7259_GIMBAL_ARRIVAL_MAX_MS;
}

static void confirm_home(gimbal_io_t *io)
{
    ++s_gimbal.stats.snapshot.center_confirmed;
    s_gimbal.ready = true;
    s_gimbal.centered_ticket = s_gimbal.home_ticket;
    io->home_waiting = false;
    io->sent_ordinary_motion = false;
    io->confirmed_at_center = true;
    io->recent_ordinary_zero = false;
    io->pose_pan = (pose_axis_t){0};
    io->pose_pitch = (pose_axis_t){0};
    io->has_motion_attempt = false;
    io->waiting_motion_ack = false;
    /* Timing starts here after returning from an offline interval. */
    s_gimbal.phase_at = rtos_get_time();
    io->pose_started_at = s_gimbal.phase_at;
    notify(&s_centered);
    MYBOT_LOGI(TAG, "gimbal center confirmed");
}

static void on_move_reply(gimbal_io_t *io, const uint8_t *frame, size_t size, uint32_t now)
{
    uint8_t code = frame[4];
    bool complete = size == 11 && code == 6;
    bool immediate = size == 7 && (code == 0 || code == 1 || code == 2 || code == 4);
    if (!complete && !immediate) {
        bk7259_gimbal_stats_bad_reply(&s_gimbal.stats);
        return;
    }
    int16_t pan = complete ? read_i16(frame + 5) : 0;
    int16_t pitch = complete ? read_i16(frame + 7) : 0;
    bool allow_home_arrival = io->disable_home_sent ||
        (io->home_waiting && io->home_ticket == s_gimbal.home_ticket &&
         (complete ? !io->recent_ordinary_zero : io->home_accepts_short_reply));
    const bk7259_gimbal_stats_request_t *matched =
        bk7259_gimbal_stats_reply(&s_gimbal.stats, now, code, pan, pitch, allow_home_arrival);
    if (!s_gimbal.settings.motion_enabled)
        return;
    if (limit_running() && matched && matched->group == BK7259_GIMBAL_STATS_NORMAL &&
        matched->sent_at == s_gimbal.test.limit_sent_at && limit_step()->result == BK7259_GIMBAL_LIMIT_PENDING) {
        bk7259_gimbal_limit_step_t *step = limit_step();
        uint16_t elapsed = (uint16_t)(now - matched->sent_at);
        bool unique_short = immediate && !matched->overlapping && !matched->short_uncertain;
        if ((unique_short || (code == 4 && matched->arrived)) && !step->has_ack) {
            step->ack_ms = elapsed;
            step->has_ack = true;
        }
        if (matched->arrived && (code == 4 || complete)) {
            step->arrival_ms = elapsed;
            step->has_arrival = true;
            step->result = BK7259_GIMBAL_LIMIT_REACHED;
            s_gimbal.test.due_at = now + LIMIT_DWELL_MS;
        } else if (unique_short && (code == 1 || code == 2)) {
            step->result = BK7259_GIMBAL_LIMIT_REJECTED;
            recover_home(now);
            return;
        }
    }
    if (code == 1) {
        io->home_waiting = false;
        if (s_gimbal.ready) {
            MYBOT_LOGW(TAG, "EB521 boot demonstration detected, centering again");
            recover_home(now);
        }
        return;
    }
    io->last_link_rx_at = now;
    if (!matched)
        return;
    if (matched->group == BK7259_GIMBAL_STATS_NORMAL && io->waiting_motion_ack &&
        matched->sent_at == io->motion_attempt_at) {
        if (matched->arrived && (code == 4 || complete)) {
            io->waiting_motion_ack = false;
            io->confirmed_at_center = matched->pan == 0 && matched->pitch == 0;
            io->pose_pan = (pose_axis_t){.start = matched->pan, .target = matched->pan};
            io->pose_pitch = (pose_axis_t){.start = matched->pitch, .target = matched->pitch};
        } else if (code == 2 && !matched->overlapping && !matched->short_uncertain) {
            io->waiting_motion_ack = false;
            io->pose_pan.target = io->pose_pan.start;
            io->pose_pitch.target = io->pose_pitch.start;
        }
    } else if (matched->group == BK7259_GIMBAL_STATS_HOME && matched->arrived &&
               matched->sent_at == io->home_sent_at && allow_home_arrival &&
               !s_gimbal.ready && io->home_waiting && (code == 4 || complete)) {
        confirm_home(io);
    }
}

static void finish_charge_off(uint32_t ticket, int result)
{
    s_gimbal.charge_off_pending = false;
    s_gimbal.charge_off_completed = ticket;
    s_gimbal.charge_off_result = result;
    notify(&s_wake);
}

static void on_charge_reply(gimbal_io_t *io, uint8_t code, uint32_t now)
{
    bk7259_gimbal_stats_power_reply(&s_gimbal.stats, now, code);
    if (!io->charge_waiting || now - io->charge_sent_at > CHARGE_ACK_TIMEOUT_MS)
        return;
    uint8_t control = io->charge_wire_control;
    if (control == EB521_PROBE_CONTROL) {
        if (code == 2) {
            io->charge_waiting = false;
            s_gimbal.probe_running = false;
            MYBOT_LOGI(TAG, "UART probe E6/02 verified: rx_bytes=%lu echo=%s",
                       (unsigned long)io->probe_rx_bytes, io->probe_echo ? "seen" : "none");
        }
        return;
    }
    /* 02 is the diagnostic acknowledgement. A delayed diagnostic response
     * must never satisfy a legal charging request. */
    if (code != 0)
        return;

    io->charge_waiting = false;
    io->last_link_rx_at = now;
    if (io->home_ticket == s_gimbal.home_ticket && due(io->charge_sent_at, io->home_fence_at))
        io->home_has_fence = true;
    if (control == 1) {
        if (!io->charge_confirmed && !io->off_reply_uncertain) {
            io->charge_confirmed = true;
            MYBOT_LOGI(TAG, "charging enabled");
        }
    } else {
        bool on_expired = now - io->last_on_at >= CHARGE_EXPIRY_MS;
        int result = io->on_reply_uncertain && !on_expired ? -1 : 0;
        if (on_expired)
            io->on_reply_uncertain = false;
        if (io->charge_wire_ticket && s_gimbal.charge_off_pending &&
            io->charge_wire_ticket == s_gimbal.charge_off_ticket)
            finish_charge_off(io->charge_wire_ticket, result);
        if (result == 0 && io->charge_confirmed)
            MYBOT_LOGI(TAG, "charging stopped");
        io->charge_confirmed = false;
    }
}

/* Parse only V1.2 reply sizes, and retain a possible partial frame. A flood
 * consumes at most one 200-byte UART read and one buffer scan per poll. */
static void drain_frames(gimbal_io_t *io, uint32_t now)
{
    size_t previous_bytes = io->used;
    if (uart_read_ready(GIMBAL_UART) == BK_OK) {
        size_t capacity = sizeof(io->rx) - io->used;
        int count = bk_uart_read_bytes(GIMBAL_UART, io->rx + io->used, (uint32_t)capacity, 0);
        if (count < 0 || (size_t)count > capacity) {
            MYBOT_LOGW(TAG, "UART receive failed");
            return;
        }
        io->used += (size_t)count;
        if (s_gimbal.probe_running)
            io->probe_rx_bytes += (unsigned)count;
    }

    size_t keep = io->used;
    size_t pos = 0;
    while (pos < io->used) {
        const uint8_t *frame = io->rx + pos;
        size_t remaining = io->used - pos;
        if (frame[0] != 3) {
            ++pos;
            continue;
        }
        if (remaining == 1) {
            if (keep == io->used)
                keep = pos;
            break;
        }
        bool move = frame[1] == EB521_MOVE_REPLY;
        bool charge = frame[1] == EB521_CHARGE_REPLY;
        bool echo = s_gimbal.probe_running && frame[1] == EB521_CHARGE;
        if (!move && !charge && !echo) {
            ++pos;
            continue;
        }
        size_t length = remaining >= 4 ? (size_t)frame[2] * 256U + frame[3] + 6U : 0;
        if (length != 0 && length != 7 && !(move && length == 11)) {
            if (move && pos + 4 > previous_bytes)
                bk7259_gimbal_stats_bad_reply(&s_gimbal.stats);
            if (charge && pos + 4 > previous_bytes)
                bk7259_gimbal_stats_power_bad_reply(&s_gimbal.stats);
            ++pos;
            continue;
        }
        if (!length || remaining < length) {
            if (keep == io->used)
                keep = pos;
            ++pos;
            continue;
        }
        uint16_t crc = (uint16_t)frame[length - 2] | (uint16_t)frame[length - 1] << 8;
        if (crc16(frame, length - 2) != crc) {
            if (move && pos + length > previous_bytes)
                bk7259_gimbal_stats_bad_reply(&s_gimbal.stats);
            if (charge && pos + length > previous_bytes)
                bk7259_gimbal_stats_power_bad_reply(&s_gimbal.stats);
            ++pos;
            continue;
        }
        if (move)
            on_move_reply(io, frame, length, now);
        else if (charge)
            on_charge_reply(io, frame[4], now);
        else if (frame[4] == EB521_PROBE_CONTROL)
            io->probe_echo = true;
        pos += length;
        keep = io->used;
    }
    io->used -= keep;
    memmove(io->rx, io->rx + keep, io->used);
}

static void service_charge(gimbal_io_t *io, uint32_t now)
{
    if (io->charge_waiting) {
        if (now - io->charge_sent_at < CHARGE_ACK_TIMEOUT_MS)
            return;
        io->charge_waiting = false;
        if (io->charge_wire_control == EB521_PROBE_CONTROL) {
            s_gimbal.probe_running = false;
            MYBOT_LOGW(TAG, "UART probe timeout: rx_bytes=%lu echo=%s",
                       (unsigned long)io->probe_rx_bytes, io->probe_echo ? "seen" : "none");
        } else {
            if (io->charge_wire_control == 1)
                io->on_reply_uncertain = true;
            else
                io->off_reply_uncertain = true;
            if (io->charge_wire_ticket && s_gimbal.charge_off_pending &&
                io->charge_wire_ticket == s_gimbal.charge_off_ticket)
                finish_charge_off(io->charge_wire_ticket, -1);
        }
    }

    bool stop = s_gimbal.charge_off_pending;
    bool heartbeat = heartbeat_enabled() && (s_gimbal.charge_changed || due(now, io->charge_due));
    if (!heartbeat && !stop && !s_gimbal.pending_probe)
        return;
    if (!tx_ready(io, EB521_CHARGE))
        return;

    bool charge_enabled = s_gimbal.settings.charge_enabled && !s_gimbal.charge_locked;
    uint8_t control = stop ? 0 : heartbeat ? (uint8_t)charge_enabled : EB521_PROBE_CONTROL;
    uint32_t ticket = stop ? s_gimbal.charge_off_ticket : 0;
    if (control == EB521_PROBE_CONTROL) {
        s_gimbal.pending_probe = false;
        s_gimbal.probe_running = true;
        io->probe_echo = false;
        io->probe_rx_bytes = 0;
    } else {
        s_gimbal.charge_changed = control != (uint8_t)charge_enabled;
    }

    io->charge_wire_control = control;
    io->charge_wire_ticket = ticket;
    io->charge_waiting = send_frame(io, EB521_CHARGE, &control, 1);
    io->charge_sent_at = io->charge_waiting ? io->last_tx_return_at : rtos_get_time();
    /* Start each periodic interval at this write; a delayed send must not
     * bring the following renewal closer. Probes do not renew charging. */
    if (control != EB521_PROBE_CONTROL)
        io->charge_due = io->charge_sent_at + CHARGE_PERIOD_MS;
    bk7259_gimbal_stats_power_tx(&s_gimbal.stats, io->charge_sent_at, control, io->charge_waiting);
    if (!io->charge_waiting) {
        s_gimbal.probe_running = false;
        if (stop)
            finish_charge_off(ticket, -1);
    } else if (control == 1) {
        io->last_on_at = io->charge_sent_at;
    } else if (control == EB521_PROBE_CONTROL) {
        MYBOT_LOGI(TAG, "UART probe sent: 03 66 00 01 02 FE D9");
    }
}

static uint16_t choose_speed(unsigned int delta, uint32_t duration_ms)
{
    if (delta == 0)
        return 20;
    /* EB521 uses floor(5000 / PPS) milliseconds per degree. Invert
     * that integer conversion to find the slowest speed that fits. */
    uint32_t max_ms_per_degree = duration_ms * 10U / delta;
    uint32_t pps = 5000U / (max_ms_per_degree + 1U) + 1U;
    return pps <= 1000U ? (uint16_t)pps : 1000U;
}

static float smooth(float u)
{
    if (u < 0) u = 0;
    if (u > 1) u = 1;
    return u * u * u * (u * (u * 6.0f - 15.0f) + 10.0f);
}

static int16_t angle_units(float degrees)
{
    float units = degrees * 10.0f;
    return (int16_t)(units >= 0 ? units + 0.5f : units - 0.5f);
}

typedef enum { PATH_OPENING, PATH_ANGRY, PATH_SURPRISED } path_kind_t;

typedef struct {
    int16_t pan;
    int16_t pitch;
    uint16_t duration_ms;
} path_point_t;

static const path_point_t k_opening_path[] = {
    {35, 0, 250}, {0, 0, 250},
};
static const path_point_t k_angry_path[] = {
    {80, 0, 300}, {-80, 0, 500}, {0, 0, 410},
};
static const path_point_t k_surprised_path[] = {
    {300, 0, 800}, {300, 0, 300}, {0, 0, 650},
};

static const path_point_t *path_points(path_kind_t kind, size_t *count)
{
    switch (kind) {
    case PATH_OPENING:
        *count = sizeof(k_opening_path) / sizeof(k_opening_path[0]);
        return k_opening_path;
    case PATH_ANGRY:
        *count = sizeof(k_angry_path) / sizeof(k_angry_path[0]);
        return k_angry_path;
    case PATH_SURPRISED:
        *count = sizeof(k_surprised_path) / sizeof(k_surprised_path[0]);
        return k_surprised_path;
    }
    *count = 0;
    return NULL;
}

static uint32_t scene_duration(bk7259_gimbal_scene_t scene)
{
    if (scene == BK7259_GIMBAL_SCENE_HAPPY)
        return 2000;
    if (scene == BK7259_GIMBAL_SCENE_SAD)
        return 3000;

    size_t count;
    const path_point_t *points = path_points(
        scene == BK7259_GIMBAL_SCENE_ANGRY ? PATH_ANGRY : PATH_SURPRISED, &count);
    uint32_t duration = 0;
    for (size_t i = 0; i < count; ++i)
        duration += points[i].duration_ms;
    return duration;
}

static void scene_sample(bk7259_gimbal_scene_t scene, uint32_t time,
                         int16_t *pan, int16_t *pitch)
{
    *pan = 0;
    *pitch = 0;
    if (scene == BK7259_GIMBAL_SCENE_HAPPY) {
        float fade_in = smooth((float)time / MOTION_BLEND_MS);
        float fade_out = smooth((float)(2000U - time) / MOTION_BLEND_MS);
        *pan = angle_units(6.0f * fade_in * fade_out *
                           sinf(2.0f * 3.14159265359f * 0.8f * time / 1000.0f));
    } else if (scene == BK7259_GIMBAL_SCENE_SAD) {
        *pitch = angle_units(10.0f * smooth(time / 3000.0f));
    }
}

static void service_home(gimbal_io_t *io, uint32_t now)
{
    /* E1 does not identify its request. Drain replies to the one-shot home
     * before sending a new home that can unlock motion on re-enable. */
    if (io->disable_home_sent && now - io->home_sent_at <= HOME_RESPONSE_MS)
        return;
    io->disable_home_sent = false;
    if (!due(now, io->home_due) || !tx_ready(io, EB521_MOVE))
        return;
    io->home_due = now + HOME_PERIOD_MS;
    /* Drop a stale partial motion reply at the new home boundary. Keep other
     * fragments because a charge reply can arrive in parallel with motion. */
    if (io->used >= 2 && io->rx[1] == EB521_MOVE_REPLY)
        io->used = 0;
    if (send_home(io)) {
        io->home_waiting = true;
        io->home_sent_at = io->last_tx_return_at;
        io->home_accepts_short_reply = !io->home_needs_fence || io->home_has_fence || io->home_sent;
        io->home_sent = true;
    } else {
        io->home_waiting = false;
    }
}

static void reset_path(gimbal_io_t *io, int16_t pan, int16_t pitch)
{
    io->path_elapsed_ms = 0;
    io->path_start_pan = pan;
    io->path_start_pitch = pitch;
    io->path_segment = 0;
}

static uint32_t path_segment_duration(const gimbal_io_t *io, const path_point_t *point)
{
    unsigned int pan_distance = (unsigned int)abs(point->pan - io->path_start_pan);
    unsigned int pitch_distance = (unsigned int)abs(point->pitch - io->path_start_pitch);
    unsigned int distance = pan_distance > pitch_distance ? pan_distance : pitch_distance;
    /* At 18 ms/degree, quintic peak speed stays near 104 degrees/s even
     * when the new path starts far from its first target. */
    uint32_t minimum = (distance * 18U + 9U) / 10U;
    return minimum > point->duration_ms ? minimum : point->duration_ms;
}

static bool path_target(const gimbal_io_t *io, path_kind_t kind,
                        int16_t *pan, int16_t *pitch, uint32_t *interval)
{
    size_t count;
    const path_point_t *points = path_points(kind, &count);
    if (io->path_segment >= count)
        return false;

    const path_point_t *point = &points[io->path_segment];
    uint32_t duration = path_segment_duration(io, point);
    uint32_t remaining = duration - io->path_elapsed_ms;
    bool hold = io->path_start_pan == point->pan && io->path_start_pitch == point->pitch;
    /* Merge a short remainder into the last sample so it ends exactly on
     * the keyframe while keeping at least 100 ms between submissions. */
    *interval = hold || remaining < 2U * CONTROL_PERIOD_MS ? remaining : CONTROL_PERIOD_MS;
    float eased = smooth((float)(io->path_elapsed_ms + *interval) / duration);
    *pan = angle_units((io->path_start_pan + (point->pan - io->path_start_pan) * eased) / 10.0f);
    *pitch = angle_units((io->path_start_pitch + (point->pitch - io->path_start_pitch) * eased) / 10.0f);
    return true;
}

/* Consume each planned point once, including a failed local write. */
static void advance_path(gimbal_io_t *io, path_kind_t kind, uint32_t interval)
{
    size_t count;
    const path_point_t *points = path_points(kind, &count);
    if (io->path_segment >= count)
        return;

    const path_point_t *point = &points[io->path_segment];
    uint32_t remaining = path_segment_duration(io, point) - io->path_elapsed_ms;
    if (interval < remaining) {
        io->path_elapsed_ms += interval;
        return;
    }
    ++io->path_segment;
    io->path_elapsed_ms = 0;
    io->path_start_pan = point->pan;
    io->path_start_pitch = point->pitch;
}

static void scene_target(bk7259_gimbal_scene_t scene, uint32_t elapsed,
                         int16_t *pan, int16_t *pitch, uint32_t *interval)
{
    uint32_t duration = scene_duration(scene);
    *interval = CONTROL_PERIOD_MS;
    if (elapsed < duration) {
        uint32_t sample = elapsed + CONTROL_PERIOD_MS;
        if (sample > duration)
            sample = duration;
        scene_sample(scene, sample, pan, pitch);
        return;
    }
    scene_sample(scene, duration, pan, pitch);
    unsigned int distance = (unsigned int)abs(*pan);
    if ((unsigned int)abs(*pitch) > distance)
        distance = (unsigned int)abs(*pitch);
    uint32_t return_ms = CENTER_TIME_MS + distance * 13U / 10U;
    uint32_t tail = elapsed - duration + CONTROL_PERIOD_MS;
    float remaining = 1.0f - smooth((float)tail / return_ms);
    *pan = angle_units(*pan * remaining / 10.0f);
    *pitch = angle_units(*pitch * remaining / 10.0f);
}

static int16_t step_toward(int16_t current, int16_t target, int step)
{
    if (target - current > step)
        return current + step;
    if (current - target > step)
        return current - step;
    return target;
}

static int16_t clamp_angle(int16_t angle, int16_t minimum, int16_t maximum)
{
    if (angle < minimum)
        return minimum;
    if (angle > maximum)
        return maximum;
    return angle;
}

static void start_blend(gimbal_io_t *io, const motion_request_t *request, uint32_t now,
                        int16_t pan, int16_t pitch)
{
    io->blend_at = now;
    io->blend_ms = MOTION_BLEND_MS;
    io->blend_pan = pan;
    io->blend_pitch = pitch;
    if (request->kind == MOTION_TARGET) {
        unsigned int pan_distance = (unsigned int)abs(request->target.pan - pan);
        unsigned int pitch_distance = (unsigned int)abs(request->target.pitch - pitch);
        unsigned int distance = pan_distance > pitch_distance ? pan_distance : pitch_distance;
        /* Quintic peak rate is 1.875 times average. Longer moves get extra
         * time so the motor can follow the transition without a final jump. */
        uint32_t duration = distance * 18U / 10U;
        if (duration > io->blend_ms)
            io->blend_ms = duration;
    }
}

static bool submit_planned_move(gimbal_io_t *io, int16_t pan, int16_t pitch,
                                int16_t estimated_pan, int16_t estimated_pitch,
                                uint32_t interval, unsigned int rate)
{
    unsigned int pan_distance = (unsigned int)abs(pan - estimated_pan);
    unsigned int pitch_distance = (unsigned int)abs(pitch - estimated_pitch);
    unsigned int distance = pan_distance > pitch_distance ? pan_distance : pitch_distance;
    /* EB521 executes an integer number of milliseconds per degree. Round
     * that bound up so a catch-up move also respects the configured rate. */
    uint32_t min_ms_per_degree = (10000U + rate - 1U) / rate;
    uint32_t travel_ms = (distance * min_ms_per_degree + 9U) / 10U;
    if (travel_ms < interval)
        travel_ms = interval;
    uint16_t pan_speed = choose_speed(pan_distance, travel_ms);
    uint16_t pitch_speed = choose_speed(pitch_distance, travel_ms);
    uint32_t window = arrival_window(pan_distance, pitch_distance, pan_speed, pitch_speed);
    bool submitted = send_move(io, BK7259_GIMBAL_STATS_NORMAL, pan, pitch, pan_speed, pitch_speed, window);
    uint32_t attempted_at = submitted ? io->last_tx_return_at : rtos_get_time();
    io->motion_due = attempted_at + (submitted ? travel_ms : interval);
    if (submitted) {
        await_motion_arrival(io, pan, pitch, pan_speed, pitch_speed, window);
    } else {
        io->has_motion_attempt = true;
        io->motion_attempt_at = attempted_at;
    }
    return submitted;
}

/* Thinking has one endpoint. Listening starts at center and alternates down
 * and back; its next nod stays anchored to the start of the listening phase. */
static void service_dialogue_leg(gimbal_io_t *io, uint32_t now, bool changed,
                                 int16_t estimated_pan, int16_t estimated_pitch)
{
    bool thinking = s_gimbal.phase == BK7259_GIMBAL_PHASE_THINKING;
    bool down = !thinking && !changed && io->planned_pitch == 0;
    int16_t pan = thinking ? 120 : 0;
    int16_t pitch = down ? 30 : 0;
    uint32_t duration = thinking ? THINKING_TURN_MS : LISTENING_LEG_MS;

    /* A phase takeover can find the previous goal still moving away from
     * the estimated pose. Send the new endpoint even if that pose is zero. */
    if (thinking || !changed || estimated_pan || estimated_pitch ||
        io->pose_pan.target || io->pose_pitch.target)
        submit_planned_move(io, pan, pitch, estimated_pan, estimated_pitch, duration,
                            DIALOGUE_MAX_STEP * 1000U / CONTROL_PERIOD_MS);
    io->planned_pan = pan;
    io->planned_pitch = pitch;
    io->target_attempted = thinking;
    if (down)
        io->motion_due = now + CONTROL_PERIOD_MS;
    else if (!thinking)
        io->motion_due = now + LISTENING_INTERVAL_MS - ((now - s_gimbal.phase_at) % LISTENING_INTERVAL_MS);
}

static void service_motion(gimbal_io_t *io, uint32_t now)
{
    cancel_superseded_motion(io, now);
    /* RX has already been drained this turn. On expiry the trajectory
     * continues from its next sample; the previous frame is never retried. */
    if (io->waiting_motion_ack) {
        if (now - io->motion_attempt_at < io->motion_arrival_window_ms)
            return;
        bk7259_gimbal_stats_expire_arrival(&s_gimbal.stats, io->motion_attempt_at);
        io->waiting_motion_ack = false;
    }
    const motion_request_t *request = &s_gimbal.motion;
    bool changed = io->motion_revision != request->revision;
    if (request->kind == MOTION_NONE ||
        (!changed && (io->target_attempted || !due(now, io->motion_due))) ||
        (io->has_motion_attempt && now - io->motion_attempt_at < CONTROL_PERIOD_MS) ||
        !tx_ready(io, EB521_MOVE))
        return;

    /* Sample only once the new revision actually has a transmit slot. The
     * last command can still move the axes while we wait for either guard. */
    now = rtos_get_time();
    int16_t estimated_pan, estimated_pitch;
    estimate_pose(io, now, &estimated_pan, &estimated_pitch);
    if (changed) {
        io->motion_revision = request->revision;
        io->target_attempted = false;
        io->attempted_point = false;
        io->planned_pan = estimated_pan;
        io->planned_pitch = estimated_pitch;
        io->opening = s_gimbal.opening_pending;
        reset_path(io, estimated_pan, estimated_pitch);
        start_blend(io, request, now, estimated_pan, estimated_pitch);
    }

    uint32_t interval = CONTROL_PERIOD_MS;
    int16_t pan = 0, pitch = 0;
    bool scene_returned = false;
    bool keyed = false;
    path_kind_t path = PATH_OPENING;
    uint32_t elapsed = now - request->started_at;
    switch (request->kind) {
    case MOTION_TARGET:
        pan = request->target.pan;
        pitch = request->target.pitch;
        break;
    case MOTION_SCENE:
        if (request->scene == BK7259_GIMBAL_SCENE_ANGRY || request->scene == BK7259_GIMBAL_SCENE_SURPRISED) {
            path = request->scene == BK7259_GIMBAL_SCENE_ANGRY ? PATH_ANGRY : PATH_SURPRISED;
            keyed = path_target(io, path, &pan, &pitch, &interval);
            if (!keyed) {
                start_home();
                return;
            }
        } else {
            scene_target(request->scene, elapsed, &pan, &pitch, &interval);
            scene_returned = elapsed >= scene_duration(request->scene) && pan == 0 && pitch == 0;
        }
        break;
    case MOTION_DIALOGUE:
        if (s_gimbal.phase == BK7259_GIMBAL_PHASE_THINKING || s_gimbal.phase == BK7259_GIMBAL_PHASE_LISTENING) {
            service_dialogue_leg(io, now, changed, estimated_pan, estimated_pitch);
            return;
        }
        if (io->opening) {
            keyed = path_target(io, path, &pan, &pitch, &interval);
            if (!keyed) {
                io->opening = false;
                s_gimbal.opening_pending = false;
                start_blend(io, request, now, estimated_pan, estimated_pitch);
            }
        }
        if (!keyed) {
            if (s_gimbal.phase == BK7259_GIMBAL_PHASE_SPEAKING) {
                uint32_t phase_ms = (now - s_gimbal.phase_at) % SPEAKING_PERIOD_MS;
                pan = angle_units(6.0f * sinf(2.0f * 3.14159265359f * phase_ms / SPEAKING_PERIOD_MS));
            } else {
                interval = IDLE_WAIT_MS;
            }
        }
        break;
    case MOTION_NONE:
        return;
    }

    bool explicit_target = request->kind == MOTION_TARGET;
    uint32_t blending = now - io->blend_at;
    if (!keyed && blending < io->blend_ms) {
        float phase = (float)blending / io->blend_ms;
        float eased = smooth(phase);
        pan = angle_units((io->blend_pan + (pan - io->blend_pan) * eased) / 10.0f);
        pitch = angle_units((io->blend_pitch + (pitch - io->blend_pitch) * eased) / 10.0f);
        if (interval > CONTROL_PERIOD_MS)
            interval = CONTROL_PERIOD_MS;
    }
    pan = clamp_angle(pan, -PAN_LIMIT, PAN_LIMIT);
    pitch = clamp_angle(pitch, PITCH_UP_LIMIT, PITCH_DOWN_LIMIT);
    if (!keyed && (pan != io->planned_pan || pitch != io->planned_pitch) && interval > CONTROL_PERIOD_MS)
        interval = CONTROL_PERIOD_MS;
    if (interval < CONTROL_PERIOD_MS)
        interval = CONTROL_PERIOD_MS;
    if (!keyed) {
        int maximum_step = (int)(MOTION_LIMIT_TENTHS_PER_S * CONTROL_PERIOD_MS / 1000U);
        if (request->kind == MOTION_DIALOGUE)
            maximum_step = DIALOGUE_MAX_STEP;
        pan = step_toward(io->planned_pan, pan, maximum_step);
        pitch = step_toward(io->planned_pitch, pitch, maximum_step);
    }
    if (scene_returned && pan == 0 && pitch == 0) {
        start_home();
        return;
    }
    bool terminal_target = explicit_target && pan == request->target.pan && pitch == request->target.pitch;
    bool same_point = pan == io->planned_pan && pitch == io->planned_pitch;
    bool last_goal_matches = pan == io->pose_pan.target && pitch == io->pose_pitch.target;
    io->planned_pan = pan;
    io->planned_pitch = pitch;
    io->motion_due = now + interval;
    if (keyed)
        advance_path(io, path, interval);
    /* A plateau after a failed sample must not implicitly resend it. On a
     * takeover, send a hold at the estimated pose if the old goal differs. */
    if (!terminal_target && same_point && (io->attempted_point || last_goal_matches))
        return;

    unsigned int rate = request->kind == MOTION_DIALOGUE && !keyed
                        ? DIALOGUE_MAX_STEP * 1000U / CONTROL_PERIOD_MS : MOTION_LIMIT_TENTHS_PER_S;
    io->attempted_point = true;
    io->target_attempted = terminal_target;
    bool submitted = submit_planned_move(io, pan, pitch, estimated_pan, estimated_pitch, interval, rate);
    if (terminal_target) {
        if (submitted)
            MYBOT_LOGI(TAG, "absolute movement sent: pan=%d pitch=%d", pan, pitch);
        else
            MYBOT_LOGW(TAG, "absolute target not submitted: pan=%d pitch=%d", pan, pitch);
    }
}

/* Timed dialogue stages drive the same mailbox transitions as live dialogue.
 * The hangup and each scene advance on their own center ticket. */
static void enter_test_stage(bk7259_gimbal_test_stage_t stage, uint32_t now)
{
    static const struct {
        bk7259_gimbal_phase_t phase;
        unsigned duration_ms;
    } dialogue[] = {
        [BK7259_GIMBAL_TEST_OPENING]   = {BK7259_GIMBAL_PHASE_NEUTRAL, 900},
        [BK7259_GIMBAL_TEST_LISTENING] = {BK7259_GIMBAL_PHASE_LISTENING, 5000},
        [BK7259_GIMBAL_TEST_THINKING]  = {BK7259_GIMBAL_PHASE_THINKING, 6000},
        [BK7259_GIMBAL_TEST_SPEAKING]  = {BK7259_GIMBAL_PHASE_SPEAKING, 4000},
        [BK7259_GIMBAL_TEST_NEUTRAL]   = {BK7259_GIMBAL_PHASE_NEUTRAL, 1000},
    };
    static const bk7259_gimbal_scene_t scenes[] = {
        BK7259_GIMBAL_SCENE_HAPPY, BK7259_GIMBAL_SCENE_SAD,
        BK7259_GIMBAL_SCENE_ANGRY, BK7259_GIMBAL_SCENE_SURPRISED,
    };
    s_gimbal.test.stage = stage;
    bk7259_gimbal_stats_stage(&s_gimbal.stats, stage);
    if (stage < BK7259_GIMBAL_TEST_HANGUP) {
        update_dialogue(true, dialogue[stage].phase, now);
        s_gimbal.test.due_at = now + dialogue[stage].duration_ms;
        return;
    }
    s_gimbal.test.expected_home = s_gimbal.home_ticket + 1U;
    s_gimbal.test.due_at = now + TEST_HOME_TIMEOUT_MS;
    if (stage == BK7259_GIMBAL_TEST_HANGUP) {
        update_dialogue(false, BK7259_GIMBAL_PHASE_NEUTRAL, now);
    } else {
        bk7259_gimbal_scene_t scene = scenes[stage - BK7259_GIMBAL_TEST_HAPPY];
        queue_scene(scene, now);
        s_gimbal.test.due_at += scene_duration(scene);
    }
}

/* Preserve the UART guard across a statistics reset. The first new frame
 * starts a fresh pair on the reporting side only. */
static void reset_stats_window(uint32_t now)
{
    bk7259_gimbal_stats_reset(&s_gimbal.stats, now);
    s_gimbal.stats_has_tx = false;
}

/* Absolute angles in tenths of a degree. On a corner-to-corner stroke,
 * 180 degrees of pan at 1000 PPS and 45 degrees of pitch at 250 PPS both take 900 ms. */
static const struct {
    int16_t pan, pitch;
    uint16_t pitch_pps;
} k_limit_targets[BK7259_GIMBAL_LIMIT_STEP_COUNT] = {
    {0, -300, LIMIT_PPS}, {0, 0, LIMIT_PPS},
    {0,  150, LIMIT_PPS}, {0, 0, LIMIT_PPS},
    {-900, 0, LIMIT_PPS}, {0, 0, LIMIT_PPS},
    { 900, 0, LIMIT_PPS}, {0, 0, LIMIT_PPS},
    {-900, -300, LIMIT_PPS}, { 900,  150, LIMIT_DIAGONAL_PITCH_PPS},
    { 900, -300, LIMIT_PPS}, {-900,  150, LIMIT_DIAGONAL_PITCH_PPS},
    { 900,  150, LIMIT_PPS}, {-900, -300, LIMIT_DIAGONAL_PITCH_PPS},
    {-900,  150, LIMIT_PPS}, { 900, -300, LIMIT_DIAGONAL_PITCH_PPS},
    {0, 0, LIMIT_PPS},
};

static void service_limit_test(gimbal_io_t *io, uint32_t now)
{
    bk7259_gimbal_limit_step_t *step = limit_step();
    if (step->result == BK7259_GIMBAL_LIMIT_PENDING) {
        /* RX has already been drained, including replies at the deadline. */
        if ((int32_t)(now - s_gimbal.test.due_at) <= 0)
            return;
        step->result = BK7259_GIMBAL_LIMIT_TIMEOUT;
        bk7259_gimbal_stats_expire_arrival(&s_gimbal.stats, s_gimbal.test.limit_sent_at);
        recover_home(now);
        return;
    }
    if (step->result == BK7259_GIMBAL_LIMIT_REACHED) {
        if (!due(now, s_gimbal.test.due_at))
            return;
        if (s_gimbal.test.limit_index + 1U == BK7259_GIMBAL_LIMIT_STEP_COUNT) {
            finish_test(BK7259_GIMBAL_TEST_COMPLETED, now);
            return;
        }
        ++s_gimbal.test.limit_index;
        step = limit_step();
    }
    if (!tx_ready(io, EB521_MOVE))
        return;
    /* Each step begins at a confirmed endpoint, including the initial center. */
    unsigned pan_distance = (unsigned)abs(step->pan - io->pose_pan.target);
    unsigned pitch_distance = (unsigned)abs(step->pitch - io->pose_pitch.target);
    uint32_t window = arrival_window(pan_distance, pitch_distance, step->pan_pps, step->pitch_pps);
    if (!write_move(io, BK7259_GIMBAL_STATS_NORMAL, step->pan, step->pitch,
                    step->pan_pps, step->pitch_pps, window)) {
        step->result = BK7259_GIMBAL_LIMIT_TX_FAILED;
        recover_home(rtos_get_time());
        return;
    }
    step->tx = 1;
    step->result = BK7259_GIMBAL_LIMIT_PENDING;
    s_gimbal.test.limit_sent_at = io->last_tx_return_at;
    s_gimbal.test.due_at = io->last_tx_return_at + window;
    io->motion_revision = s_gimbal.motion.revision;
    await_motion_arrival(io, step->pan, step->pitch, step->pan_pps, step->pitch_pps, window);
}

static void service_test(gimbal_io_t *io, uint32_t now)
{
    if (!test_active())
        return;
    if (s_gimbal.test.state == BK7259_GIMBAL_TEST_SCHEDULED) {
        if (s_gimbal.test.expected_home) {
            if (!s_gimbal.ready) {
                if (due(now, s_gimbal.test.due_at))
                    finish_test(BK7259_GIMBAL_TEST_INTERRUPTED, now);
                return;
            }
        } else if (!due(now, s_gimbal.test.due_at)) {
            return;
        }
        if (!s_gimbal.ready || !s_gimbal.accepting) {
            finish_test(BK7259_GIMBAL_TEST_INTERRUPTED, now);
            return;
        }
        if (s_gimbal.test.result.limit_test && !io->confirmed_at_center) {
            start_home();
            s_gimbal.test.expected_home = s_gimbal.home_ticket;
            s_gimbal.test.due_at = now + TEST_HOME_TIMEOUT_MS;
            return;
        }
        reset_stats_window(now);
        s_gimbal.test.state = BK7259_GIMBAL_TEST_RUNNING;
        if (s_gimbal.test.result.limit_test) {
            s_gimbal.stats.snapshot.limit_test = true;
            memcpy(s_gimbal.stats.snapshot.limit_steps, s_gimbal.test.result.limit_steps,
                   sizeof(s_gimbal.stats.snapshot.limit_steps));
        } else {
            enter_test_stage(BK7259_GIMBAL_TEST_OPENING, now);
        }
        return;
    }
    if (limit_running()) {
        service_limit_test(io, now);
        return;
    }
    bk7259_gimbal_test_stage_t stage = s_gimbal.test.stage;
    if (stage < BK7259_GIMBAL_TEST_HANGUP) {
        if (!due(now, s_gimbal.test.due_at))
            return;
        if (stage == BK7259_GIMBAL_TEST_OPENING && s_gimbal.opening_pending) {
            if (due(now, s_gimbal.test.due_at + TEST_HOME_TIMEOUT_MS))
                recover_home(now);
            return;
        }
    } else if (!s_gimbal.ready || s_gimbal.centered_ticket != s_gimbal.test.expected_home) {
        if (due(now, s_gimbal.test.due_at)) {
            finish_test(BK7259_GIMBAL_TEST_INTERRUPTED, now);
            if (s_gimbal.ready)
                start_home();
        }
        return;
    }
    if (stage == BK7259_GIMBAL_TEST_SURPRISED) {
        bk7259_gimbal_stats_tick(&s_gimbal.stats, now);
        if (!s_gimbal.stats.snapshot.groups[BK7259_GIMBAL_STATS_NORMAL].pending &&
            !s_gimbal.stats.snapshot.groups[BK7259_GIMBAL_STATS_NORMAL].arrival.pending &&
            !s_gimbal.stats.snapshot.groups[BK7259_GIMBAL_STATS_HOME].pending &&
            !s_gimbal.stats.snapshot.groups[BK7259_GIMBAL_STATS_HOME].arrival.pending &&
            !s_gimbal.stats.snapshot.power.groups[BK7259_GIMBAL_POWER_STOP].pending &&
            !s_gimbal.stats.snapshot.power.groups[BK7259_GIMBAL_POWER_REQUEST].pending)
            finish_test(BK7259_GIMBAL_TEST_COMPLETED, now);
        return;
    }
    enter_test_stage((bk7259_gimbal_test_stage_t)(stage + 1), now);
}

static void worker(void *arg)
{
    (void)arg;
    gimbal_io_t io = {0};
    io.charge_due = rtos_get_time();
    io.last_link_rx_at = io.charge_due;
    for (;;) {
        if (rtos_lock_mutex(&s_lock) != BK_OK)
            break;
        uint32_t now = rtos_get_time();
        if (io.recent_ordinary_zero && now - io.ordinary_zero_at > HOME_RESPONSE_MS)
            io.recent_ordinary_zero = false;
        if (now - io.stats_tick_at >= 1000U) {
            io.stats_tick_at = now;
            bk7259_gimbal_stats_tick(&s_gimbal.stats, now);
        }
        sync_home(&io, now);
        drain_frames(&io, now);
        if (s_gimbal.disable_home_pending && tx_ready(&io, EB521_MOVE)) {
            s_gimbal.disable_home_pending = false;
            if (send_home(&io)) {
                io.disable_home_sent = true;
                io.home_sent_at = io.last_tx_return_at;
                MYBOT_LOGI(TAG, "servo control disabled: center command sent");
            }
        }
        service_charge(&io, now);
        now = rtos_get_time();
        if (s_gimbal.ready && now - io.last_link_rx_at >= LINK_LOSS_MS) {
            MYBOT_LOGW(TAG, "EB521 replies lost, centering again");
            recover_home(now);
        }
        if (s_gimbal.stopping && !s_gimbal.charge_off_pending && !s_gimbal.settings.pending) {
            unlock();
            break;
        }
        service_test(&io, now);
        sync_home(&io, now);
        if (!s_gimbal.stopping && s_gimbal.settings.motion_enabled) {
            if (s_gimbal.ready && !limit_running()) {
                service_motion(&io, now);
            } else if (!s_gimbal.ready) {
                io.opening = false;
                service_home(&io, now);
            }
        }
        bool motion_work = s_gimbal.settings.motion_enabled &&
                           (!s_gimbal.ready || s_gimbal.motion.kind != MOTION_NONE || limit_running());
        unsigned wait = io.charge_waiting || motion_work ? RX_POLL_MS : IDLE_WAIT_MS;
        bk7259_gimbal_settings_t settings = s_gimbal.settings;
        uint32_t settings_revision = s_gimbal.settings_revision;
        unlock();
        if (settings.pending) {
            int result = bk7259_gimbal_prefs_save(settings.motion_enabled, settings.charge_enabled);
            if (rtos_lock_mutex(&s_lock) != BK_OK)
                break;
            if (settings_revision == s_gimbal.settings_revision) {
                s_gimbal.settings.pending = false;
                s_gimbal.settings.save_failed = result < 0;
            }
            unlock();
            if (result < 0)
                MYBOT_LOGW(TAG, "gimbal settings save failed");
            continue;
        }
        bk_err_t ret = rtos_get_semaphore(&s_wake, wait);
        if (ret != BK_OK && ret != kTimeoutErr)
            break;
    }
    if (rtos_lock_mutex(&s_lock) == BK_OK) {
        finish_test(BK7259_GIMBAL_TEST_INTERRUPTED, rtos_get_time());
        s_gimbal.accepting = false;
        s_gimbal.stopping = true;
        s_gimbal.pending_probe = false;
        s_gimbal.probe_running = false;
        if (s_gimbal.settings.pending) {
            s_gimbal.settings.pending = false;
            s_gimbal.settings.save_failed = true;
        }
        if (s_gimbal.charge_off_pending)
            finish_charge_off(s_gimbal.charge_off_ticket, -1);
        unlock();
    }
    notify(&s_done);
    rtos_delete_thread(NULL);
}

static int release_uart(void)
{
    if (s_gimbal.uart_owned) {
        if (bk_uart_deinit(GIMBAL_UART) != BK_OK)
            return -1;
        s_gimbal.uart_owned = false;
    }
    if (s_gimbal.tx_mapped) {
        if (gpio_dev_unmap_by_func(GPIO_DEV_UART2_TXD) != BK_OK)
            return -1;
        s_gimbal.tx_mapped = false;
    }
    if (s_gimbal.rx_mapped) {
        if (gpio_dev_unmap_by_func(GPIO_DEV_UART2_RXD) != BK_OK)
            return -1;
        s_gimbal.rx_mapped = false;
    }
    return 0;
}

int bk7259_gimbal_prepare(void)
{
    if (!mailbox_initialized()) {
        beken_mutex_t lock = NULL;
        if (rtos_init_mutex(&lock) != BK_OK)
            return -1;
        /* Settings reads may come from the UI before platform preparation. */
        __atomic_store_n(&s_lock, lock, __ATOMIC_RELEASE);
    }
    if ((!s_wake && rtos_init_semaphore(&s_wake, 1) != BK_OK) ||
        (!s_done && rtos_init_semaphore(&s_done, 1) != BK_OK) ||
        (!s_centered && rtos_init_semaphore(&s_centered, 1) != BK_OK))
        return -1;
    if (rtos_lock_mutex(&s_lock) != BK_OK)
        return -1;
    if (s_gimbal.prepared) {
        bool active = s_gimbal.accepting && !s_gimbal.stopping;
        unlock();
        return active ? 0 : -1;
    }
    unlock();
    /* A previous unsuccessful initialization may still own UART/GPIO. */
    if (release_uart() < 0)
        return -1;
    if (s_gimbal.aosl_owned)
        aosl_dtor();
    if (rtos_lock_mutex(&s_lock) != BK_OK)
        return -1;
    memset(&s_gimbal, 0, sizeof(s_gimbal));
    reset_stats_window(rtos_get_time());
    unlock();
    bk7259_gimbal_settings_t settings = {0};
    settings.save_failed = bk7259_gimbal_prefs_load(&settings.motion_enabled,
                                                   &settings.charge_enabled) < 0;
    while (rtos_get_semaphore(&s_done, 0) == BK_OK) { }
    while (rtos_get_semaphore(&s_wake, 0) == BK_OK) { }
    while (rtos_get_semaphore(&s_centered, 0) == BK_OK) { }
    if (bk_uart_init(GIMBAL_UART, &k_uart_config) != BK_OK)
        return -1;
    s_gimbal.uart_owned = true;
    if (bk_uart_disable_rx_interrupt(GIMBAL_UART) != BK_OK ||
        bk_uart_disable_sw_fifo(GIMBAL_UART) != BK_OK)
        goto failed;
    if (gpio_dev_map_by_func(GPIO_DEV_UART2_TXD) != BK_OK)
        goto failed;
    s_gimbal.tx_mapped = true;
    if (gpio_dev_map_by_func(GPIO_DEV_UART2_RXD) != BK_OK)
        goto failed;
    s_gimbal.rx_mapped = true;
    /* Keep the logging runtime alive even when a failed product shutdown
     * drops its reference while this worker is retained for a later retry. */
    aosl_ctor();
    s_gimbal.aosl_owned = true;
    if (rtos_lock_mutex(&s_lock) != BK_OK)
        goto failed;
    s_gimbal.home_ticket = 1;
    s_gimbal.settings = settings;
    bk_err_t created = rtos_create_psram_thread(&s_gimbal.worker, BEKEN_APPLICATION_PRIORITY,
                                                "eb521", worker, STACK_BYTES, NULL);
    s_gimbal.prepared = created == BK_OK;
    s_gimbal.accepting = created == BK_OK;
    unlock();
    if (created != BK_OK)
        goto failed;
    MYBOT_LOGI(TAG, "UART2 ready: GPIO2 TX/GPIO3 RX, 115200 8N1");
    return 0;
failed:
    if (release_uart() < 0) {
        MYBOT_LOGE(TAG, "initialization cleanup failed; retaining UART resources");
    } else if (s_gimbal.aosl_owned) {
        aosl_dtor();
        s_gimbal.aosl_owned = false;
    }
    return -1;
}

int bk7259_gimbal_update_dialogue(bool active, bk7259_gimbal_phase_t phase)
{
    if (phase < BK7259_GIMBAL_PHASE_NEUTRAL || phase > BK7259_GIMBAL_PHASE_SPEAKING)
        return -1;
    if (!mailbox_initialized())
        return active ? -1 : 0;
    if (rtos_lock_mutex(&s_lock) != BK_OK)
        return -1;
    if (!s_gimbal.prepared || !s_gimbal.accepting || s_gimbal.charge_locked) {
        unlock();
        return active ? -1 : 0;
    }
    if (limit_running())
        recover_home(rtos_get_time());
    else if (test_active())
        finish_test(BK7259_GIMBAL_TEST_INTERRUPTED, rtos_get_time());
    int result = update_dialogue(active, phase, rtos_get_time());
    unlock();
    return result;
}

/* The caller holds the mailbox lock. */
static int admit_motion(void)
{
    if (!s_gimbal.prepared || !s_gimbal.accepting || s_gimbal.charge_locked || s_gimbal.stopping)
        return -1;
    if (!s_gimbal.settings.motion_enabled)
        return BK7259_GIMBAL_MOTION_DISABLED;
    if (test_active())
        return BK7259_GIMBAL_BUSY;
    return s_gimbal.ready ? 0 : BK7259_GIMBAL_NOT_READY;
}

int bk7259_gimbal_send_next_command(void)
{
    static const int16_t positions[][2] = {{-300, 0}, {0, 100}, {0, -200}, {100, 0}};
    if (!mailbox_initialized() || rtos_lock_mutex(&s_lock) != BK_OK)
        return -1;
    int result = admit_motion();
    if (result == 0) {
        size_t index = s_gimbal.next_command;
        s_gimbal.next_command = (index + 1U) % (sizeof(positions) / sizeof(positions[0]));
        s_gimbal.opening_pending = false;
        s_gimbal.motion.kind = MOTION_TARGET;
        s_gimbal.motion.target.pan = positions[index][0];
        s_gimbal.motion.target.pitch = positions[index][1];
        s_gimbal.motion.started_at = rtos_get_time();
        ++s_gimbal.motion.revision;
        notify(&s_wake);
    }
    unlock();
    return result;
}

int bk7259_gimbal_request(bk7259_gimbal_scene_t scene)
{
    if (scene < BK7259_GIMBAL_SCENE_HAPPY || scene >= BK7259_GIMBAL_SCENE_COUNT ||
        !mailbox_initialized() || rtos_lock_mutex(&s_lock) != BK_OK)
        return -1;
    int result = admit_motion();
    if (result == 0) {
        queue_scene(scene, rtos_get_time());
        notify(&s_wake);
    }
    unlock();
    return result;
}

int bk7259_gimbal_request_probe(void)
{
    if (!mailbox_initialized() || rtos_lock_mutex(&s_lock) != BK_OK)
        return -1;
    int result = 0;
    if (!s_gimbal.prepared || !s_gimbal.accepting || s_gimbal.stopping || s_gimbal.charge_locked) {
        result = -1;
    } else if (s_gimbal.pending_probe || s_gimbal.probe_running) {
        result = -2;
    } else {
        s_gimbal.pending_probe = true;
        notify(&s_wake);
    }
    unlock();
    return result;
}

static uint32_t request_center(void)
{
    s_gimbal.conversation = false;
    if (s_gimbal.ready) {
        start_home();
    } else {
        s_gimbal.motion.kind = MOTION_NONE;
        s_gimbal.opening_pending = false;
    }
    notify(&s_wake);
    return s_gimbal.home_ticket;
}

static int wait_for_center(uint32_t ticket, uint32_t timeout_ms)
{
    uint32_t start = rtos_get_time();
    for (;;) {
        if (rtos_lock_mutex(&s_lock) != BK_OK)
            return -1;
        bool done = (int32_t)(s_gimbal.centered_ticket - ticket) >= 0;
        bool stopped = s_gimbal.stopping;
        bool disabled = !s_gimbal.settings.motion_enabled;
        unlock();
        if (done || disabled)
            return 0;
        uint32_t elapsed = rtos_get_time() - start;
        if (stopped || elapsed >= timeout_ms)
            return -1;
        bk_err_t ret = rtos_get_semaphore(&s_centered, timeout_ms - elapsed);
        if (ret != BK_OK && ret != kTimeoutErr)
            return -1;
    }
}

int bk7259_gimbal_center_wait(uint32_t timeout_ms)
{
    if (!mailbox_initialized())
        return 0;
    if (rtos_lock_mutex(&s_lock) != BK_OK)
        return -1;
    if (!s_gimbal.prepared || !s_gimbal.settings.motion_enabled) {
        unlock();
        return 0;
    }
    finish_test(BK7259_GIMBAL_TEST_INTERRUPTED, rtos_get_time());
    uint32_t ticket = request_center();
    unlock();
    return wait_for_center(ticket, timeout_ms);
}

static uint32_t request_charge_off(void)
{
    if (!s_gimbal.charge_off_pending) {
        s_gimbal.charge_off_pending = true;
        ++s_gimbal.charge_off_ticket;
    }
    notify(&s_wake);
    return s_gimbal.charge_off_ticket;
}

/* The caller holds the mailbox lock. */
static bool settings_available(void)
{
    return s_gimbal.prepared && s_gimbal.accepting && !s_gimbal.stopping && !s_gimbal.charge_locked;
}

static void schedule_settings_save(void)
{
    s_gimbal.settings.pending = true;
    s_gimbal.settings.save_failed = false;
    ++s_gimbal.settings_revision;
    notify(&s_wake);
}

int bk7259_gimbal_request_motion_enabled(bool enabled)
{
    if (!mailbox_initialized() || rtos_lock_mutex(&s_lock) != BK_OK)
        return -1;
    if (!settings_available()) {
        unlock();
        return -1;
    }
    if (enabled != s_gimbal.settings.motion_enabled) {
        finish_test(BK7259_GIMBAL_TEST_INTERRUPTED, rtos_get_time());
        s_gimbal.settings.motion_enabled = enabled;
        s_gimbal.disable_home_pending = !enabled;
        start_home();
        notify(&s_centered);
        schedule_settings_save();
    } else if (s_gimbal.settings.save_failed) {
        schedule_settings_save();
    }
    unlock();
    return 0;
}

int bk7259_gimbal_set_charging(bool enabled)
{
    if (!mailbox_initialized() || rtos_lock_mutex(&s_lock) != BK_OK)
        return -1;
    if (!settings_available()) {
        unlock();
        return -1;
    }
    if (enabled != s_gimbal.settings.charge_enabled) {
        s_gimbal.settings.charge_enabled = enabled;
        s_gimbal.charge_changed = true;
        if (!enabled)
            (void)request_charge_off();
        schedule_settings_save();
    } else if (s_gimbal.settings.save_failed) {
        schedule_settings_save();
    }
    unlock();
    return 0;
}

int bk7259_gimbal_get_settings(bk7259_gimbal_settings_t *settings)
{
    if (!settings || !mailbox_initialized() || rtos_lock_mutex(&s_lock) != BK_OK)
        return -1;
    bool available = settings_available();
    if (available)
        *settings = s_gimbal.settings;
    unlock();
    return available ? 0 : -1;
}

int bk7259_gimbal_get_stats(bk7259_gimbal_stats_snapshot_t *out)
{
    if (!out || !mailbox_initialized() || rtos_lock_mutex(&s_lock) != BK_OK)
        return -1;
    bool prepared = s_gimbal.prepared;
    if (prepared) {
        if (s_gimbal.test.state == BK7259_GIMBAL_TEST_IDLE || s_gimbal.test.state == BK7259_GIMBAL_TEST_RUNNING)
            bk7259_gimbal_stats_snapshot(&s_gimbal.stats, rtos_get_time(), out);
        else
            *out = s_gimbal.test.result;
        out->test_state = s_gimbal.test.state;
        out->test_stage = s_gimbal.test.stage;
    }
    unlock();
    return prepared ? 0 : -1;
}

int bk7259_gimbal_reset_stats(void)
{
    if (!mailbox_initialized() || rtos_lock_mutex(&s_lock) != BK_OK)
        return -1;
    int result = -1;
    if (test_active()) {
        result = BK7259_GIMBAL_BUSY;
    } else if (s_gimbal.prepared) {
        reset_stats_window(rtos_get_time());
        memset(&s_gimbal.test, 0, sizeof(s_gimbal.test));
        result = 0;
    }
    unlock();
    return result;
}

static int schedule_test(bool limits)
{
    if (!mailbox_initialized() || rtos_lock_mutex(&s_lock) != BK_OK)
        return -1;
    int result = admit_motion();
    if (result == 0 && (s_gimbal.conversation || s_gimbal.motion.kind != MOTION_NONE))
        result = BK7259_GIMBAL_BUSY;
    if (result == 0) {
        memset(&s_gimbal.test, 0, sizeof(s_gimbal.test));
        if (limits) {
            s_gimbal.test.result.limit_test = true;
            for (unsigned i = 0; i < BK7259_GIMBAL_LIMIT_STEP_COUNT; ++i) {
                bk7259_gimbal_limit_step_t *step = &s_gimbal.test.result.limit_steps[i];
                step->pan = k_limit_targets[i].pan;
                step->pitch = k_limit_targets[i].pitch;
                step->pan_pps = LIMIT_PPS;
                step->pitch_pps = k_limit_targets[i].pitch_pps;
            }
        }
        s_gimbal.test.state = BK7259_GIMBAL_TEST_SCHEDULED;
        s_gimbal.test.due_at = rtos_get_time() + TEST_START_DELAY_MS;
        notify(&s_wake);
    }
    unlock();
    return result;
}

int bk7259_gimbal_schedule_test(void)
{
    return schedule_test(false);
}

int bk7259_gimbal_schedule_limit_test(void)
{
    return schedule_test(true);
}

static int wait_for_charge_off(uint32_t ticket, uint32_t timeout_ms)
{
    uint32_t start = rtos_get_time();
    for (;;) {
        if (rtos_lock_mutex(&s_lock) != BK_OK)
            return -1;
        bool done = (int32_t)(s_gimbal.charge_off_completed - ticket) >= 0 &&
                    !s_gimbal.settings.pending && !s_gimbal.disable_home_pending;
        int result = s_gimbal.charge_off_result;
        unlock();
        if (done)
            return result;
        uint32_t elapsed = rtos_get_time() - start;
        if (elapsed >= timeout_ms)
            return -1;
        uint32_t wait = timeout_ms - elapsed;
        if (rtos_delay_milliseconds(wait > IDLE_WAIT_MS ? IDLE_WAIT_MS : wait) != BK_OK)
            return -1;
    }
}

int bk7259_gimbal_stop_charging_wait(uint32_t timeout_ms)
{
    if (!mailbox_initialized())
        return 0;
    if (rtos_lock_mutex(&s_lock) != BK_OK)
        return -1;
    if (!s_gimbal.prepared) {
        unlock();
        return 0;
    }
    if (s_gimbal.stopping) {
        unlock();
        return -1;
    }
    finish_test(BK7259_GIMBAL_TEST_INTERRUPTED, rtos_get_time());
    s_gimbal.charge_locked = true;
    s_gimbal.pending_probe = false;
    s_gimbal.opening_pending = false;
    s_gimbal.conversation = false;
    s_gimbal.motion.kind = MOTION_NONE;
    ++s_gimbal.motion.revision;
    uint32_t ticket = request_charge_off();
    unlock();
    return wait_for_charge_off(ticket, timeout_ms);
}

int bk7259_gimbal_shutdown(void)
{
    if (!mailbox_initialized())
        return 0;
    if (rtos_lock_mutex(&s_lock) != BK_OK)
        return -1;
    bool prepared = s_gimbal.prepared;
    bool stopping = s_gimbal.stopping;
    s_gimbal.accepting = false;
    unlock();

    if (prepared && !stopping) {
        /* Centering and charging can progress together, but send no new
         * charging heartbeat while waiting for the worker to exit. */
        if (bk7259_gimbal_stop_charging_wait(CHARGE_ACK_TIMEOUT_MS * 3U) < 0)
            MYBOT_LOGW(TAG, "charge stop unconfirmed; EB521 will time out");
        if (bk7259_gimbal_center_wait(STOP_TIMEOUT_MS) < 0)
            MYBOT_LOGW(TAG, "centering unconfirmed before shutdown");
        if (rtos_lock_mutex(&s_lock) != BK_OK)
            return -1;
        s_gimbal.stopping = true;
        notify(&s_wake);
        unlock();
    }
    if (prepared) {
        if (rtos_get_semaphore(&s_done, STOP_TIMEOUT_MS) != BK_OK)
            return -1;
        if (rtos_lock_mutex(&s_lock) != BK_OK)
            return -1;
        s_gimbal.prepared = false;
        s_gimbal.worker = NULL;
        unlock();
    }
    if (release_uart() < 0)
        return -1;
    if (s_gimbal.aosl_owned) {
        aosl_dtor();
        s_gimbal.aosl_owned = false;
    }
    return 0;
}
