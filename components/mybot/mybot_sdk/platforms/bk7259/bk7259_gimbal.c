/* SPDX-License-Identifier: Apache-2.0 */
#include "bk7259_gimbal.h"
#include "bk7259_audio_dialogue.h"
#include "bk7259_gimbal_prefs.h"
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
#define FRAME_BYTES 200U
#define STACK_BYTES 4096U
#define RX_POLL_MS 5U
#define IDLE_WAIT_MS 20U
#define STOP_TIMEOUT_MS 3000U
#define HOME_PERIOD_MS 1000U
#define HOME_RESPONSE_MS 900U
#define HOME_GUARD_MS 250U
#define CHARGE_PERIOD_MS 1000U
#define CHARGE_ACK_TIMEOUT_MS 250U
#define LINK_LOSS_MS (3U * CHARGE_PERIOD_MS + CHARGE_ACK_TIMEOUT_MS)
#define CHARGE_EXPIRY_MS 3000U
#define MOVE_REPLY_LOSS_MS 1000U
#define MOVE_ACTIVE_GAP_MS 200U
#define CONTROL_PERIOD_MS 40U
#define CENTER_TIME_MS 280U
#define PAN_LIMIT 800
#define PITCH_UP_LIMIT (-200)
#define PITCH_DOWN_LIMIT 100
#define DIALOGUE_STEP_MS 60U
#define DIALOGUE_MAX_STEP 40
#define MOTION_BLEND_MS 240U
#define MOTION_LIMIT_TENTHS_PER_S 1600U
#define LISTENING_INTERVAL_MS 3500U
#define LISTENING_NOD_MS 1120U
#define THINKING_TURN_MS 700U
#define THINKING_HOLD_MS 3000U
#define WOBBLE_PERIOD_MS 1250U
#define PROCESSING_PERIOD_MS 3000U
#define SPEAKING_ENVELOPE_MS 80U
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

    uint32_t charge_due;
    uint32_t charge_sent_at;
    uint32_t last_link_rx_at;
    uint32_t last_on_at;
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
    uint32_t opening_at;
    bool opening;
    bool target_sent;
    bool motion_unanswered;
    uint32_t first_unanswered_at;
    uint32_t ordinary_sent_at;
    int16_t last_pan;
    int16_t last_pitch;
    int16_t previous_pan;
    int16_t previous_pitch;
    uint32_t blend_at;
    uint32_t blend_ms;
    int16_t blend_pan;
    int16_t blend_pitch;
    int16_t blend_pan_direction;
    int16_t blend_pitch_direction;
    float speaking_amplitude;
    uint32_t envelope_at;
} gimbal_io_t;

static gimbal_context_t s_gimbal;
/* The worker owns UART until exit is observed. These notifications remain
 * available across shutdown retries and another platform preparation. */
static beken_mutex_t s_lock;
static beken_semaphore_t s_wake;
static beken_semaphore_t s_done;
static beken_semaphore_t s_centered;

static const uart_config_t k_uart_config = {
    .baud_rate = 115200,
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

/* The single worker checks the 128-byte FIFO before making a short write.
 * Every protocol request is at most 14 bytes. */
static bool send_frame(uint8_t command, const uint8_t *payload, size_t length)
{
    uint8_t frame[14] = {0x03, command, 0, (uint8_t)length};
    if (length > sizeof(frame) - 6 || !bk_uart_is_tx_over(GIMBAL_UART))
        return false;
    memcpy(frame + 4, payload, length);
    write_u16(frame + 4 + length, crc16(frame, length + 4));
    if (bk_uart_write_bytes(GIMBAL_UART, frame, (uint32_t)length + 6U) != BK_OK) {
        MYBOT_LOGW(TAG, "UART write failed: command=0x%02x", (unsigned)command);
        return false;
    }
    return true;
}

static bool send_move(int16_t pan, int16_t pitch, uint16_t pan_speed, uint16_t pitch_speed)
{
    if (pan < -PAN_LIMIT || pan > PAN_LIMIT || pitch < PITCH_UP_LIMIT || pitch > PITCH_DOWN_LIMIT)
        return false;
    uint8_t payload[8];
    write_u16(payload, (uint16_t)pan);
    write_u16(payload + 2, (uint16_t)pitch);
    write_u16(payload + 4, pan_speed);
    write_u16(payload + 6, pitch_speed);
    return send_frame(EB521_MOVE, payload, sizeof(payload));
}

static bool send_home(void)
{
    return send_move(0, 0, 1000, 1000);
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

static void sync_home(gimbal_io_t *io, uint32_t now)
{
    if (io->home_ticket == s_gimbal.home_ticket)
        return;
    io->home_ticket = s_gimbal.home_ticket;
    io->home_waiting = false;
    io->home_sent = false;
    io->home_needs_fence = io->sent_ordinary_motion;
    io->home_has_fence = false;
    /* A short 04 has no position. After ordinary movements, wait for a
     * later round trip before trusting an untagged immediate response. */
    io->home_fence_at = now + (io->home_needs_fence ? HOME_GUARD_MS : 0);
    io->home_due = io->home_fence_at;
    io->motion_unanswered = false;
}

static void confirm_home(gimbal_io_t *io)
{
    s_gimbal.ready = true;
    s_gimbal.centered_ticket = s_gimbal.home_ticket;
    io->home_waiting = false;
    io->sent_ordinary_motion = false;
    io->last_pan = 0;
    io->last_pitch = 0;
    io->previous_pan = 0;
    io->previous_pitch = 0;
    io->ordinary_sent_at = 0;
    io->speaking_amplitude = 0;
    /* Timing starts here after returning from an offline interval. */
    s_gimbal.phase_at = rtos_get_time();
    notify(&s_centered);
    MYBOT_LOGI(TAG, "gimbal center confirmed");
}

static void on_move_reply(gimbal_io_t *io, const uint8_t *frame, size_t size, uint32_t now)
{
    if (!s_gimbal.settings.motion_enabled)
        return;
    uint8_t code = frame[4];
    if (size == 7 && code == 1) {
        io->home_waiting = false;
        if (s_gimbal.ready) {
            MYBOT_LOGW(TAG, "EB521 boot demonstration detected, centering again");
            start_home();
        }
        return;
    }
    bool complete = size == 11 && code == 6;
    bool immediate = size == 7 && (code == 0 || code == 2 || code == 4);
    if (!complete && !immediate)
        return;
    io->last_link_rx_at = now;
    io->motion_unanswered = false;
    if (s_gimbal.ready || io->home_ticket != s_gimbal.home_ticket ||
        !io->home_waiting || now - io->home_sent_at > HOME_RESPONSE_MS)
        return;
    if ((size == 7 && code == 4 && io->home_accepts_short_reply) ||
        (complete && read_i16(frame + 5) == 0 && read_i16(frame + 7) == 0))
        confirm_home(io);
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
    bool home_required = s_gimbal.settings.motion_enabled && !s_gimbal.ready && !s_gimbal.stopping;
    bool heartbeat = (!s_gimbal.charge_locked || home_required) &&
                     (s_gimbal.charge_changed || due(now, io->charge_due));
    if (!heartbeat && !stop && !s_gimbal.pending_probe)
        return;
    if (!bk_uart_is_tx_over(GIMBAL_UART))
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
        io->charge_due = now + CHARGE_PERIOD_MS;
    }

    io->charge_wire_control = control;
    io->charge_wire_ticket = ticket;
    io->charge_sent_at = now;
    io->charge_waiting = send_frame(EB521_CHARGE, &control, 1);
    if (!io->charge_waiting) {
        s_gimbal.probe_running = false;
        if (stop)
            finish_charge_off(ticket, -1);
    } else if (control == 1) {
        io->last_on_at = now;
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

static void dialogue_target(gimbal_io_t *io, uint32_t now, int16_t *pan, int16_t *pitch,
                            uint32_t *wait)
{
    const float turn = 2.0f * 3.14159265359f;
    uint32_t elapsed = now - s_gimbal.phase_at;
    switch (s_gimbal.phase) {
    case BK7259_GIMBAL_PHASE_LISTENING: {
        uint32_t cycle = elapsed % LISTENING_INTERVAL_MS;
        if (elapsed >= LISTENING_INTERVAL_MS && cycle < LISTENING_NOD_MS) {
            float fade_in = smooth((float)cycle / DIALOGUE_STEP_MS);
            float fade_out = smooth((float)(LISTENING_NOD_MS - cycle) / DIALOGUE_STEP_MS);
            *pitch = angle_units(3.0f * fade_in * fade_out * sinf(2.0f * turn * cycle / LISTENING_NOD_MS));
            *wait = CONTROL_PERIOD_MS;
        } else {
            *wait = LISTENING_INTERVAL_MS - cycle;
        }
        break;
    }
    case BK7259_GIMBAL_PHASE_THINKING:
        if (elapsed < THINKING_HOLD_MS) {
            *pan = angle_units(15.0f * smooth((float)elapsed / THINKING_TURN_MS));
            if (elapsed >= THINKING_TURN_MS)
                *wait = THINKING_HOLD_MS - elapsed;
        } else {
            uint32_t processing = elapsed - THINKING_HOLD_MS;
            float blend = smooth((float)processing / MOTION_BLEND_MS);
            float sway = 3.0f * sinf(turn * (processing % PROCESSING_PERIOD_MS) / PROCESSING_PERIOD_MS);
            *pan = angle_units(15.0f * (1.0f - blend) + sway * blend);
        }
        break;
    case BK7259_GIMBAL_PHASE_SPEAKING: {
        unsigned level = bk7259_audio_dialogue_level();
        if (level > 100)
            level = 100;
        uint32_t sample_ms = now - io->envelope_at;
        io->envelope_at = now;
        if (sample_ms > DIALOGUE_STEP_MS)
            sample_ms = DIALOGUE_STEP_MS;
        float desired = 0.06f * level;
        io->speaking_amplitude += (desired - io->speaking_amplitude) *
                                  (float)sample_ms / (sample_ms + SPEAKING_ENVELOPE_MS);
        *pan = angle_units(io->speaking_amplitude * sinf(turn * (elapsed % WOBBLE_PERIOD_MS) /
                                                         WOBBLE_PERIOD_MS));
        break;
    }
    case BK7259_GIMBAL_PHASE_NEUTRAL:
        *wait = IDLE_WAIT_MS;
        break;
    }
}

static uint32_t scene_duration(bk7259_gimbal_scene_t scene)
{
    static const uint16_t durations[] = {2000, 3000, 1200, 800};
    return durations[scene];
}

static void scene_sample(bk7259_gimbal_scene_t scene, uint32_t time,
                         int16_t *pan, int16_t *pitch)
{
    float angle = 0;
    *pitch = 0;
    if (scene == BK7259_GIMBAL_SCENE_HAPPY) {
        float fade_in = smooth((float)time / MOTION_BLEND_MS);
        float fade_out = smooth((float)(2000U - time) / MOTION_BLEND_MS);
        angle = 6.0f * fade_in * fade_out * sinf(2.0f * 3.14159265359f * 0.8f * time / 1000.0f);
    } else if (scene == BK7259_GIMBAL_SCENE_SAD) {
        *pitch = angle_units(10.0f * smooth(time / 3000.0f));
    } else if (scene == BK7259_GIMBAL_SCENE_ANGRY) {
        if (time <= 180)
            angle = 8.0f * smooth(time / 180.0f);
        else if (time <= 360)
            angle = 8.0f - 16.0f * smooth((time - 180) / 180.0f);
        else if (time <= 550)
            angle = -8.0f + 18.0f * smooth((time - 360) / 190.0f);
        else angle = 10.0f;
    } else if (scene == BK7259_GIMBAL_SCENE_SURPRISED) {
        if (time <= 380)
            angle = 30.0f * smooth(time / 380.0f);
        else if (time <= 500)
            angle = 30.0f - 5.0f * smooth((time - 380) / 120.0f);
        else angle = 25.0f;
    }
    *pan = angle_units(angle);
}

static uint32_t next_sample(bk7259_gimbal_scene_t scene, uint32_t time)
{
    uint32_t next = time + (scene == BK7259_GIMBAL_SCENE_SAD ? 50U : CONTROL_PERIOD_MS);
    static const uint16_t angry_keys[] = {180, 360, 550, 1200};
    static const uint16_t surprise_keys[] = {380, 500, 800};
    const uint16_t *keys = scene == BK7259_GIMBAL_SCENE_ANGRY ? angry_keys : surprise_keys;
    size_t count = scene == BK7259_GIMBAL_SCENE_ANGRY ? 4 : 3;
    if (scene == BK7259_GIMBAL_SCENE_ANGRY || scene == BK7259_GIMBAL_SCENE_SURPRISED) {
        for (size_t i = 0; i < count; ++i) {
            if (keys[i] <= time)
                continue;
            /* Keep the turn/end point without leaving a final segment shorter
             * than the minimum period. Merge that remainder into this one. */
            if (next >= keys[i] || keys[i] - next < CONTROL_PERIOD_MS)
                next = keys[i];
            break;
        }
    }
    uint32_t duration = scene_duration(scene);
    return next < duration ? next : duration;
}

static void service_home(gimbal_io_t *io, uint32_t now)
{
    /* E1 does not identify its request. Drain replies to the one-shot home
     * before sending a new home that can unlock motion on re-enable. */
    if (io->disable_home_sent && now - io->home_sent_at <= HOME_RESPONSE_MS)
        return;
    io->disable_home_sent = false;
    if (!due(now, io->home_due) || !bk_uart_is_tx_over(GIMBAL_UART))
        return;
    io->home_due = now + HOME_PERIOD_MS;
    /* Drop a stale partial motion reply at the new home boundary. Keep other
     * fragments because a charge reply can arrive in parallel with motion. */
    if (io->used >= 2 && io->rx[1] == EB521_MOVE_REPLY)
        io->used = 0;
    if (send_home()) {
        io->home_waiting = true;
        io->home_sent_at = now;
        io->home_accepts_short_reply = !io->home_needs_fence || io->home_has_fence || io->home_sent;
        io->home_sent = true;
    } else {
        io->home_waiting = false;
    }
}

static const float k_dialogue_opening[] = {0, 2.0f, 3.5f, 1.5f, -1.2f, 0};

static void scene_target(bk7259_gimbal_scene_t scene, uint32_t elapsed,
                         int16_t *pan, int16_t *pitch, uint32_t *interval)
{
    uint32_t duration = scene_duration(scene);
    if (elapsed < duration) {
        uint32_t sample = next_sample(scene, elapsed);
        *interval = sample - elapsed;
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

static void start_blend(gimbal_io_t *io, const motion_request_t *request, uint32_t now)
{
    io->blend_at = now;
    io->blend_ms = MOTION_BLEND_MS;
    io->blend_pan = io->last_pan;
    io->blend_pitch = io->last_pitch;
    io->blend_pan_direction = 0;
    io->blend_pitch_direction = 0;
    if (now - io->ordinary_sent_at <= 2U * DIALOGUE_STEP_MS) {
        io->blend_pan_direction = clamp_angle(io->last_pan - io->previous_pan, -8, 8);
        io->blend_pitch_direction = clamp_angle(io->last_pitch - io->previous_pitch, -8, 8);
    }
    if (request->kind == MOTION_TARGET) {
        unsigned int pan_distance = (unsigned int)abs(request->target.pan - io->last_pan);
        unsigned int pitch_distance = (unsigned int)abs(request->target.pitch - io->last_pitch);
        unsigned int distance = pan_distance > pitch_distance ? pan_distance : pitch_distance;
        /* Quintic peak rate is 1.875 times average. Longer moves get extra
         * time so the motor can follow the transition without a final jump. */
        uint32_t duration = distance * 18U / 10U;
        if (duration > io->blend_ms)
            io->blend_ms = duration;
    }
}

static int16_t blend_angle(int16_t from, int16_t to, int16_t direction, float phase, float eased)
{
    float rest = 1.0f - phase;
    float position = from + (to - from) * eased + direction * 4.0f * phase * rest * rest * rest;
    return (int16_t)(position >= 0 ? position + 0.5f : position - 0.5f);
}

static void service_motion(gimbal_io_t *io, uint32_t now)
{
    const motion_request_t *request = &s_gimbal.motion;
    if (io->motion_revision != request->revision) {
        io->motion_revision = request->revision;
        io->motion_due = now;
        io->target_sent = false;
        if (!io->opening || !s_gimbal.opening_pending)
            start_blend(io, request, now);
        io->speaking_amplitude = 0;
        io->envelope_at = now;
        if (s_gimbal.opening_pending && !io->opening) {
            io->opening = true;
            io->opening_at = now;
            if (io->last_pan == 0 && io->last_pitch == 0 &&
                io->blend_pan_direction == 0 && io->blend_pitch_direction == 0)
                io->blend_ms = 0;
        } else if (!s_gimbal.opening_pending) {
            io->opening = false;
        }
    }
    if (request->kind == MOTION_NONE || io->target_sent || !due(now, io->motion_due) ||
        !bk_uart_is_tx_over(GIMBAL_UART))
        return;

    uint32_t interval = CONTROL_PERIOD_MS;
    int16_t pan = 0, pitch = 0;
    bool scene_returned = false;
    uint32_t elapsed = now - request->started_at;
    switch (request->kind) {
    case MOTION_TARGET:
        pan = request->target.pan;
        pitch = request->target.pitch;
        break;
    case MOTION_SCENE:
        scene_target(request->scene, elapsed, &pan, &pitch, &interval);
        scene_returned = elapsed >= scene_duration(request->scene) && pan == 0 && pitch == 0;
        break;
    case MOTION_DIALOGUE: {
        interval = DIALOGUE_STEP_MS;
        uint32_t opening_time = now - io->opening_at;
        uint32_t opening_step = opening_time / DIALOGUE_STEP_MS;
        if (io->opening && opening_step + 1 < sizeof(k_dialogue_opening) / sizeof(k_dialogue_opening[0])) {
            float from = k_dialogue_opening[opening_step];
            float to = k_dialogue_opening[opening_step + 1];
            float progress = smooth((float)(opening_time % DIALOGUE_STEP_MS) / DIALOGUE_STEP_MS);
            pan = angle_units(from + (to - from) * progress);
            interval = CONTROL_PERIOD_MS;
        } else {
            io->opening = false;
            s_gimbal.opening_pending = false;
            dialogue_target(io, now, &pan, &pitch, &interval);
        }
        break;
    }
    case MOTION_NONE:
        return;
    }

    bool explicit_target = request->kind == MOTION_TARGET;
    uint32_t blending = now - io->blend_at;
    if (blending < io->blend_ms) {
        float phase = (float)blending / io->blend_ms;
        float eased = smooth(phase);
        pan = blend_angle(io->blend_pan, pan, io->blend_pan_direction, phase, eased);
        pitch = blend_angle(io->blend_pitch, pitch, io->blend_pitch_direction, phase, eased);
        if (interval > CONTROL_PERIOD_MS)
            interval = CONTROL_PERIOD_MS;
    }
    pan = clamp_angle(pan, -PAN_LIMIT, PAN_LIMIT);
    pitch = clamp_angle(pitch, PITCH_UP_LIMIT, PITCH_DOWN_LIMIT);
    if ((pan != io->last_pan || pitch != io->last_pitch) && interval > DIALOGUE_STEP_MS)
        interval = DIALOGUE_STEP_MS;
    int maximum_step = (int)(MOTION_LIMIT_TENTHS_PER_S * interval / 1000U);
    if (maximum_step == 0)
        maximum_step = 1;
    if (request->kind == MOTION_DIALOGUE && maximum_step > DIALOGUE_MAX_STEP)
        maximum_step = DIALOGUE_MAX_STEP;
    pan = step_toward(io->last_pan, pan, maximum_step);
    pitch = step_toward(io->last_pitch, pitch, maximum_step);
    if (scene_returned && pan == 0 && pitch == 0) {
        start_home();
        return;
    }
    bool terminal_target = explicit_target && pan == request->target.pan && pitch == request->target.pitch;
    io->motion_due = now + interval;
    if (!terminal_target && pan == io->last_pan && pitch == io->last_pitch)
        return;

    if (!io->motion_unanswered || now - io->ordinary_sent_at > MOVE_ACTIVE_GAP_MS) {
        io->motion_unanswered = true;
        io->first_unanswered_at = now;
    } else if (now - io->first_unanswered_at >= MOVE_REPLY_LOSS_MS) {
        MYBOT_LOGW(TAG, "EB521 movement replies lost, centering again");
        start_home();
        return;
    }

    uint16_t pan_speed = choose_speed((unsigned int)abs(pan - io->last_pan), interval);
    uint16_t pitch_speed = choose_speed((unsigned int)abs(pitch - io->last_pitch), interval);
    io->target_sent = terminal_target;
    if (send_move(pan, pitch, pan_speed, pitch_speed)) {
        io->ordinary_sent_at = now;
        io->sent_ordinary_motion = true;
        io->previous_pan = io->last_pan;
        io->previous_pitch = io->last_pitch;
        io->last_pan = pan;
        io->last_pitch = pitch;
        if (terminal_target)
            MYBOT_LOGI(TAG, "absolute movement sent: pan=%d pitch=%d", pan, pitch);
    }
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
        sync_home(&io, now);
        drain_frames(&io, now);
        if (s_gimbal.disable_home_pending && bk_uart_is_tx_over(GIMBAL_UART)) {
            s_gimbal.disable_home_pending = false;
            if (send_home()) {
                io.disable_home_sent = true;
                io.home_sent_at = now;
                MYBOT_LOGI(TAG, "servo control disabled: center command sent");
            }
        }
        service_charge(&io, now);
        if (s_gimbal.ready && now - io.last_link_rx_at >= LINK_LOSS_MS) {
            MYBOT_LOGW(TAG, "EB521 replies lost, centering again");
            start_home();
        }
        sync_home(&io, now);
        if (s_gimbal.stopping && !s_gimbal.charge_off_pending && !s_gimbal.settings.pending) {
            unlock();
            break;
        }
        if (!s_gimbal.stopping && s_gimbal.settings.motion_enabled) {
            if (s_gimbal.ready) {
                service_motion(&io, now);
            } else {
                io.opening = false;
                service_home(&io, now);
            }
        }
        bool motion_work = s_gimbal.settings.motion_enabled &&
                           (!s_gimbal.ready || s_gimbal.motion.kind != MOTION_NONE);
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
        s_gimbal.accepting = false;
        s_gimbal.stopping = true;
        s_gimbal.pending_probe = false;
        s_gimbal.probe_running = false;
        bk7259_audio_dialogue_set_speaking(false);
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
    bool started = active && !s_gimbal.conversation;
    bool ended = !active && (s_gimbal.conversation || s_gimbal.ready);
    bool changed = active && (phase != s_gimbal.phase || s_gimbal.motion.kind != MOTION_DIALOGUE);
    if (started || ended || changed) {
        s_gimbal.conversation = active;
        s_gimbal.phase = phase;
        s_gimbal.phase_at = rtos_get_time();
        bk7259_audio_dialogue_set_speaking(active && s_gimbal.settings.motion_enabled &&
                                           phase == BK7259_GIMBAL_PHASE_SPEAKING);
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
            if (started && s_gimbal.ready)
                s_gimbal.opening_pending = true;
        }
        notify(&s_wake);
    }
    int result = 0;
    if (!s_gimbal.settings.motion_enabled)
        result = BK7259_GIMBAL_MOTION_DISABLED;
    else if (active && !s_gimbal.ready)
        result = BK7259_GIMBAL_NOT_READY;
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
        s_gimbal.opening_pending = false;
        s_gimbal.motion.kind = MOTION_SCENE;
        s_gimbal.motion.scene = scene;
        s_gimbal.motion.started_at = rtos_get_time();
        ++s_gimbal.motion.revision;
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
    bk7259_audio_dialogue_set_speaking(false);
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
        s_gimbal.settings.motion_enabled = enabled;
        s_gimbal.disable_home_pending = !enabled;
        start_home();
        bk7259_audio_dialogue_set_speaking(enabled && s_gimbal.conversation &&
                                           s_gimbal.phase == BK7259_GIMBAL_PHASE_SPEAKING);
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
    s_gimbal.charge_locked = true;
    s_gimbal.pending_probe = false;
    s_gimbal.opening_pending = false;
    s_gimbal.conversation = false;
    s_gimbal.motion.kind = MOTION_NONE;
    ++s_gimbal.motion.revision;
    bk7259_audio_dialogue_set_speaking(false);
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
