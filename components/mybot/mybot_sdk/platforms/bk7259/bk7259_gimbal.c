/* SPDX-License-Identifier: Apache-2.0 */
#include "bk7259_gimbal.h"
#include "bk7259_platform_log.h"

#include <api/aosl.h>
#include <common/bk_err.h>
#include <driver/gpio.h>
#include <driver/uart.h>
#include "gpio_driver.h"
#include <math.h>
#include <os/os.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define TAG "bk7259_gimbal"
#define GIMBAL_UART UART_ID_2
#define FRAME_BYTES 200U
#define PAYLOAD_BYTES (FRAME_BYTES - 6U)
#define IO_TIMEOUT_MS 250U
#define HOMING_TIMEOUT_MS 30000U
#define STOP_TIMEOUT_MS 3000U
#define STACK_BYTES 4096U
#define CONTROL_PERIOD_MS 40U
#define CENTER_TIME_MS 280U
#define TARGET_TOLERANCE 4 /* Protocol units: 0.1 degree. */
#define PAN_LIMIT 800
#define PITCH_UP_LIMIT (-200)
#define PITCH_DOWN_LIMIT 100
#define COMMAND_ACK_TIMEOUT_MS 1000U
#define STATUS_TIMEOUT_MS 1000U
#define COMMAND_DUMP_BYTES 128U

/* Only the worker reads these fields. Buffers are fixed; no per-frame heap. */
typedef struct {
    uint8_t rx[FRAME_BYTES];
    size_t used;
    int16_t pan;
    int16_t pitch;
    uint8_t flow;
    uint8_t pan_flags;
    uint8_t pitch_flags;
    unsigned bad_frames;
    bool query_completion_logged;
    bool motion_pending;
    bool command_trace;
    unsigned command_rx_bytes;
    unsigned command_dump_bytes;
    unsigned command_bad_crc;
} gimbal_io_t;

typedef struct {
    bool prepared;
    bool accepting;
    bool conversation;
    bool stopping;
    bool fault;
    bool pending_scene;
    bool pending_center;
    bool normal_busy;
    bool manual_session;
    bool pending_command;
    bool command_running;
    bool pending_cli_scene;
    bool cli_scene_running;
    size_t next_command;
    size_t command_index;
    uint32_t command_sequence;
    bk7259_gimbal_scene_t scene;
    uint32_t revision;
    uint32_t center_ticket;
    uint32_t centered_ticket;
    beken_thread_t worker;
    bool uart_owned;
    bool tx_mapped;
    bool rx_mapped;
    bool aosl_owned;
    gimbal_io_t io;
} gimbal_context_t;

static gimbal_context_t s_gimbal;
/* Product startup/shutdown is serialized. Mailbox lock and notifications live
 * for the process lifetime: an already-entered SMP ISR or exiting worker must
 * never give a semaphore that the other core has freed. */
static beken_mutex_t s_lock;
static beken_semaphore_t s_wake;
static beken_semaphore_t s_rx;
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
    /* Binary notification coalesces; ownership always lives in the mailbox. */
    if (rtos_set_semaphore(event) != BK_OK) { }
}

static void uart_rx(uart_id_t id, void *arg)
{
    (void)id;
    (void)arg;
    /* The vendor has already copied hardware RX to its software FIFO. */
    notify(&s_rx);
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

static void cli_log_hex(const char *direction, const uint8_t *bytes,
                              size_t count, size_t base)
{
    for (size_t offset = 0; offset < count;) {
        char line[16U * 3U + 1U];
        size_t row = count - offset;
        if (row > 16U) row = 16U;
        for (size_t i = 0; i < row; ++i) {
            if (snprintf(line + i * 3U, sizeof(line) - i * 3U,
                          "%02X ", (unsigned)bytes[offset + i]) != 3) {
                MYBOT_LOGE(TAG, "UART command hex formatting failed");
                return;
            }
        }
        MYBOT_LOGI(TAG, "UART command %s bytes=%u..%u: %s", direction,
                   (unsigned)(base + offset), (unsigned)(base + offset + row), line);
        offset += row;
    }
}

/* Each request is at most 14 bytes. Wait for an empty 128-byte hardware FIFO
 * before using the vendor write API, which itself has no timeout. With one
 * owner, DMA and flow control disabled, the whole request fits without a FIFO
 * wait. A peripheral/clock hardware failure still needs watchdog recovery. */
static int send_frame(gimbal_io_t *io, uint8_t command, const uint8_t *payload, size_t length)
{
    uint8_t frame[14];
    if (length > sizeof(frame) - 6U)
        return -1;
    frame[0] = 0x03;
    frame[1] = command;
    frame[2] = (uint8_t)(length >> 8);
    frame[3] = (uint8_t)length;
    if (length)
        memcpy(frame + 4, payload, length);
    write_u16(frame + 4 + length, crc16(frame, length + 4));
    io->command_rx_bytes = 0;
    io->command_dump_bytes = 0;
    io->command_bad_crc = 0;
    if (rtos_lock_mutex(&s_lock) != BK_OK)
        return -1;
    /* A startup query may already be in flight when manual control is
     * requested; no new automatic transaction may cross that handoff. */
    if (s_gimbal.manual_session &&
        (!s_gimbal.cli_scene_running ||
         ((!s_gimbal.accepting || s_gimbal.stopping) && command != 0x63))) {
        unlock();
        return -1;
    }
    uint32_t start = rtos_get_time();
    while (!bk_uart_is_tx_over(GIMBAL_UART)) {
        if (rtos_get_time() - start >= 20U || rtos_delay_milliseconds(1) != BK_OK) {
            unlock();
            return -1;
        }
    }
    bk_err_t written = bk_uart_write_bytes(GIMBAL_UART, frame, (uint32_t)length + 6U);
    unlock();
    if (io->command_trace)
        cli_log_hex("TX", frame, length + 6U, 0);
    return written == BK_OK ? 0 : -1;
}

/* Scan complete CRC-valid candidates so a noisy partial header cannot hide a
 * subsequent complete frame. Keep partial frames across RX notifications. */
static int extract_frame(gimbal_io_t *io, uint8_t *frame)
{
    for (size_t pos = 0; pos + 4U <= io->used; ++pos) {
        if (io->rx[pos] != 0x03 || io->rx[pos + 1] < 0xe1 || io->rx[pos + 1] > 0xe5)
            continue;
        size_t payload = (size_t)io->rx[pos + 2] * 256U + io->rx[pos + 3];
        if (payload > PAYLOAD_BYTES) {
            ++io->bad_frames;
            continue;
        }
        size_t length = payload + 6U;
        if (length > io->used - pos)
            continue;
        uint16_t received = (uint16_t)io->rx[pos + length - 2] |
                            (uint16_t)io->rx[pos + length - 1] << 8;
        if (crc16(io->rx + pos, length - 2U) != received) {
            ++io->bad_frames;
            if (io->command_trace) ++io->command_bad_crc;
            continue;
        }
        memcpy(frame, io->rx + pos, length);
        io->used -= pos + length;
        memmove(io->rx, io->rx + pos + length, io->used);
        return (int)length;
    }
    if (io->used == sizeof(io->rx)) {
        /* Retain the tail, which may contain the start of the next frame. */
        memmove(io->rx, io->rx + 1, --io->used);
        ++io->bad_frames;
    }
    return 0;
}

/* 1 frame (>0), no frame before deadline (0), or transport error (-1). */
static int receive_frame(gimbal_io_t *io, uint8_t *frame, uint32_t timeout_ms)
{
    uint32_t start = rtos_get_time();
    for (;;) {
        int length = extract_frame(io, frame);
        if (length)
            return length;
        uint32_t elapsed = rtos_get_time() - start;
        if (elapsed >= timeout_ms)
            return 0;
        bk_err_t ret = rtos_get_semaphore(&s_rx, timeout_ms - elapsed);
        if (ret == kTimeoutErr)
            return 0;
        if (ret != BK_OK)
            return -1;
        int count = bk_uart_read_bytes(GIMBAL_UART, io->rx + io->used,
                                       (uint32_t)(sizeof(io->rx) - io->used), 0);
        if (count == BK_ERR_UART_RX_TIMEOUT || count == 0)
            continue; /* A coalesced/stale IRQ notification, not a motion ACK. */
        if (count < 0 || (size_t)count > sizeof(io->rx) - io->used)
            return -1;
        if (io->command_trace) {
            size_t dump = COMMAND_DUMP_BYTES - io->command_dump_bytes;
            if (dump > (size_t)count) dump = (size_t)count;
            cli_log_hex("RX", io->rx + io->used, dump, io->command_dump_bytes);
            io->command_dump_bytes += (unsigned)dump;
        }
        io->command_rx_bytes += (unsigned)count;
        io->used += (size_t)count;
        if (io->used == sizeof(io->rx))
            notify(&s_rx); /* Drain a bounded burst larger than one buffer. */
    }
}

static bool near_target(int16_t actual, int16_t target)
{
    int delta = (int)actual - target;
    return delta >= -TARGET_TOLERANCE && delta <= TARGET_TOLERANCE;
}

static int drain_input(gimbal_io_t *io)
{
    size_t dumped = 0;
    if (io->command_trace && io->used) {
        dumped = io->used < COMMAND_DUMP_BYTES ? io->used : COMMAND_DUMP_BYTES;
        cli_log_hex("discard RX", io->rx, dumped, 0);
    }
    io->used = 0;
    /* Discard already-arrived responses before a new transaction. No sequence
     * exists on the wire, so an IO timeout remains a fault rather than a retry
     * of a possibly accepted motion. Bound draining even under noisy input. */
    for (unsigned i = 0; i < 8; ++i) {
        bk_err_t ret = rtos_get_semaphore(&s_rx, 0);
        if (ret == kTimeoutErr)
            return 0;
        if (ret != BK_OK)
            return -1;
        int count = bk_uart_read_bytes(GIMBAL_UART, io->rx, sizeof(io->rx), 0);
        if (count == BK_ERR_UART_RX_TIMEOUT || count == 0)
            return 0;
        if (count < 0 || count > (int)sizeof(io->rx))
            return -1;
        if (io->command_trace && dumped < COMMAND_DUMP_BYTES) {
            size_t dump = (size_t)count;
            if (dump > COMMAND_DUMP_BYTES - dumped)
                dump = COMMAND_DUMP_BYTES - dumped;
            cli_log_hex("discard RX", io->rx, dump, dumped);
            dumped += dump;
        }
        if (count == sizeof(io->rx))
            notify(&s_rx);
    }
    return -1;
}

static int query_status_wait(gimbal_io_t *io, uint32_t timeout_ms)
{
    if (timeout_ms > STATUS_TIMEOUT_MS)
        timeout_ms = STATUS_TIMEOUT_MS;
    uint32_t start = rtos_get_time();
    if (!timeout_ms || drain_input(io) < 0 || send_frame(io, 0x63, NULL, 0) < 0)
        return -1;
    uint8_t frame[FRAME_BYTES];
    for (;;) {
        uint32_t elapsed = rtos_get_time() - start;
        if (elapsed >= timeout_ms)
            break;
        int size = receive_frame(io, frame, timeout_ms - elapsed);
        if (size <= 0)
            break;
        /* Old motion notifications cannot complete a new status request. */
        if (frame[1] != 0xe3 || size != 18 || frame[4] != 0x00 || frame[5] > 2)
            continue;
        io->flow = frame[5];
        io->pan = read_i16(frame + 6);
        io->pan_flags = frame[10];
        io->pitch = read_i16(frame + 11);
        io->pitch_flags = frame[15];
        if (io->command_trace)
            MYBOT_LOGI(TAG, "status response: flow=%u pan=%d pitch=%d flags=0x%02x/0x%02x elapsed=%ums",
                       (unsigned)io->flow, io->pan, io->pitch,
                       (unsigned)io->pan_flags, (unsigned)io->pitch_flags,
                       (unsigned)(rtos_get_time() - start));
        return 0;
    }
    if (io->command_trace)
        MYBOT_LOGE(TAG, "status query failed: timeout=%ums elapsed=%ums rx_bytes=%u bad_crc=%u buffered=%u",
                   (unsigned)timeout_ms, (unsigned)(rtos_get_time() - start),
                   io->command_rx_bytes, io->command_bad_crc, (unsigned)io->used);
    return -1;
}

static int query_status(gimbal_io_t *io)
{
    return query_status_wait(io, STATUS_TIMEOUT_MS);
}

static bool at_target(const gimbal_io_t *io, int16_t pan, int16_t pitch)
{
    return io->flow == 2 && !(io->pan_flags & 1) && !(io->pitch_flags & 1) &&
           near_target(io->pan, pan) && near_target(io->pitch, pitch);
}

/* Quantized controller speed tiers, in descending milliseconds per phase. */
static uint16_t choose_speed(int delta, uint32_t duration_ms, uint32_t *motion_ms)
{
    static const uint16_t speeds[] = {20, 25, 40, 50, 62, 83, 100, 125,
                                      166, 200, 250, 333, 500, 1000};
    if (delta < 0)
        delta = -delta;
    /* One phase = 0.315 degree = 3.15 protocol units. Round up only for
     * timing; send the original 0.1-degree absolute target to the controller. */
    uint32_t phases = ((uint32_t)delta * 100U + 314U) / 315U;
    uint32_t available = duration_ms > 3U ? duration_ms - 3U : 1U;
    uint16_t speed = 1000;
    for (size_t i = 0; i < sizeof(speeds) / sizeof(speeds[0]); ++i) {
        if (phases * (1000U / speeds[i]) <= available) {
            speed = speeds[i];
            break;
        }
    }
    *motion_ms = phases * (1000U / speed);
    return speed;
}

static int move_to(gimbal_io_t *io, int16_t pan, int16_t pitch, uint32_t duration_ms)
{
    if (pan < -PAN_LIMIT || pan > PAN_LIMIT || pitch < PITCH_UP_LIMIT || pitch > PITCH_DOWN_LIMIT)
        return -1;
    if (pan == io->pan && pitch == io->pitch)
        return 0;
    uint8_t payload[8];
    uint32_t pan_ms, pitch_ms;
    uint16_t pan_speed = choose_speed((int)pan - io->pan, duration_ms, &pan_ms);
    uint16_t pitch_speed = choose_speed((int)pitch - io->pitch, duration_ms, &pitch_ms);
    write_u16(payload, (uint16_t)pan);
    write_u16(payload + 2, (uint16_t)pitch);
    write_u16(payload + 4, pan_speed);
    write_u16(payload + 6, pitch_speed);
    uint32_t expected_ms = pan_ms > pitch_ms ? pan_ms : pitch_ms;
    if (io->command_trace)
        MYBOT_LOGI(TAG, "absolute move: from=%d,%d target=%d,%d speed=%u,%u expected=%ums ack_timeout=%ums",
                   io->pan, io->pitch, pan, pitch, (unsigned)pan_speed,
                   (unsigned)pitch_speed, (unsigned)expected_ms, COMMAND_ACK_TIMEOUT_MS);
    uint32_t start = rtos_get_time();
    if (drain_input(io) < 0)
        return -1;
    io->motion_pending = true;
    if (send_frame(io, 0x61, payload, sizeof(payload)) < 0)
        return -1;
    bool accepted = false;
    /* ACK latency is independent of motor phase timing. Leave a full response
     * window for acceptance and completion/status instead of failing after
     * the first short receive wait. Never retransmit an uncertain movement. */
    uint32_t timeout = expected_ms + COMMAND_ACK_TIMEOUT_MS + STATUS_TIMEOUT_MS;
    uint8_t frame[FRAME_BYTES];
    for (;;) {
        uint32_t elapsed = rtos_get_time() - start;
        if (!accepted && elapsed >= COMMAND_ACK_TIMEOUT_MS) {
            MYBOT_LOGE(TAG, "absolute move acknowledgement timed out: target=%d,%d elapsed=%ums rx_bytes=%u bad_crc=%u",
                       pan, pitch, (unsigned)elapsed, io->command_rx_bytes, io->command_bad_crc);
            return -1;
        }
        if (elapsed >= timeout)
            break;
        uint32_t next_query = expected_ms + 5U;
        uint32_t wait = accepted ? (elapsed < next_query ? next_query - elapsed : 1U)
                                 : COMMAND_ACK_TIMEOUT_MS - elapsed;
        if (wait > IO_TIMEOUT_MS)
            wait = IO_TIMEOUT_MS;
        if (wait > timeout - elapsed)
            wait = timeout - elapsed;
        int size = receive_frame(io, frame, wait);
        if (size < 0)
            return -1;
        if (size > 0 && frame[1] == 0xe1) {
            uint8_t result = frame[4];
            if (io->command_trace)
                MYBOT_LOGI(TAG, "absolute move response: result=0x%02x length=%d elapsed=%ums",
                           (unsigned)result, size, (unsigned)(rtos_get_time() - start));
            if (size == 7 && result == 0x00) {
                accepted = true;
            } else if (accepted && size == 11 && result == 0x06) {
                int16_t actual_pan = read_i16(frame + 5);
                int16_t actual_pitch = read_i16(frame + 7);
                if (near_target(actual_pan, pan) && near_target(actual_pitch, pitch)) {
                    /* A delayed completion can resemble a new nearby target.
                     * Confirm idle with a new query before advancing. */
                    uint32_t spent = rtos_get_time() - start;
                    if (spent >= timeout || query_status_wait(io, timeout - spent) < 0) {
                        MYBOT_LOGE(TAG, "absolute move completion status unavailable: target=%d,%d", pan, pitch);
                        return -1;
                    }
                    if (at_target(io, pan, pitch)) {
                        io->motion_pending = false;
                        return 0;
                    }
                }
            } else if (size == 7 && result == 0x03) {
                /* No physical phase: leave the actual position unchanged.
                 * Future absolute samples accumulate the intended movement. */
                io->motion_pending = false;
                return 0;
            } else if (size == 7 && result == 0x04) {
                accepted = true;
            } else if (size == 7 && result >= 0x01 && result <= 0x05) {
                io->motion_pending = false;
                MYBOT_LOGE(TAG, "absolute move rejected: result=%u target=%d,%d",
                           result, pan, pitch);
                return -1;
            }
        }
        if (accepted && rtos_get_time() - start >= next_query) {
            uint32_t spent = rtos_get_time() - start;
            if (spent >= timeout || query_status_wait(io, timeout - spent) < 0) {
                MYBOT_LOGE(TAG, "absolute move status unavailable: target=%d,%d", pan, pitch);
                return -1;
            }
            if (at_target(io, pan, pitch)) {
                io->motion_pending = false;
                if (!io->query_completion_logged) {
                    MYBOT_LOGI(TAG, "motion completion confirmed by status query");
                    io->query_completion_logged = true;
                }
                return 0;
            }
            if (rtos_delay_milliseconds(10) != BK_OK)
                return -1;
        }
    }
    MYBOT_LOGE(TAG, "motion completion timed out: target=%d,%d actual=%d,%d",
               pan, pitch, io->pan, io->pitch);
    return -1;
}

static bool superseded(uint32_t revision)
{
    if (rtos_lock_mutex(&s_lock) != BK_OK)
        return true;
    bool canceled = s_gimbal.revision != revision;
    unlock();
    return canceled;
}

static bool stopping(void)
{
    if (rtos_lock_mutex(&s_lock) != BK_OK)
        return true;
    bool value = s_gimbal.stopping;
    unlock();
    return value;
}

static bool manual_control(void)
{
    if (rtos_lock_mutex(&s_lock) != BK_OK)
        return true;
    bool manual = s_gimbal.manual_session;
    unlock();
    return manual;
}

/* Waits can be interrupted by a new mailbox request; a move already accepted
 * by EB521 is always completed before cancellation is acted upon. */
static int wait_segment(uint32_t start, uint32_t duration, uint32_t revision, bool cancelable)
{
    for (;;) {
        uint32_t elapsed = rtos_get_time() - start;
        if (elapsed >= duration)
            break;
        if (cancelable && superseded(revision))
            return 1;
        uint32_t wait = duration - elapsed;
        if (wait > 10U)
            wait = 10U;
        bk_err_t ret = rtos_get_semaphore(&s_wake, wait);
        if (ret != BK_OK && ret != kTimeoutErr)
            return -1;
    }
    return cancelable && superseded(revision) ? 1 : 0;
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
        angle = 6.0f * sinf(2.0f * 3.14159265359f * 0.8f * time / 1000.0f);
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

static int center(gimbal_io_t *io, uint32_t revision, bool cancelable)
{
    int16_t start_pan = io->pan, start_pitch = io->pitch;
    for (uint32_t time = CONTROL_PERIOD_MS; time <= CENTER_TIME_MS; time += CONTROL_PERIOD_MS) {
        if (cancelable && superseded(revision))
            return 1;
        float remaining = 1.0f - smooth((float)time / CENTER_TIME_MS);
        int16_t pan = angle_units(start_pan * remaining / 10.0f);
        int16_t pitch = angle_units(start_pitch * remaining / 10.0f);
        /* Feedback can be one quantized phase beyond a nominal limit (e.g.
         * 10.0-degree target reports 10.1). Never send that out-of-range value
         * back while interpolating the return to center. */
        if (pan < -PAN_LIMIT) pan = -PAN_LIMIT;
        if (pan > PAN_LIMIT) pan = PAN_LIMIT;
        if (pitch < PITCH_UP_LIMIT) pitch = PITCH_UP_LIMIT;
        if (pitch > PITCH_DOWN_LIMIT) pitch = PITCH_DOWN_LIMIT;
        uint32_t start = rtos_get_time();
        if (move_to(io, pan, pitch, CONTROL_PERIOD_MS) < 0) {
            MYBOT_LOGE(TAG, "centering failed: sample=%ums target=%d,%d",
                       (unsigned)time, pan, pitch);
            return -1;
        }
        int ret = wait_segment(start, CONTROL_PERIOD_MS, revision, cancelable);
        if (ret)
            return ret;
    }
    return near_target(io->pan, 0) && near_target(io->pitch, 0) ? 0 : -1;
}

static const char *scene_name(bk7259_gimbal_scene_t scene)
{
    static const char *const names[] = {"happy", "sad", "angry", "surprised"};
    return names[scene];
}

static int play_scene(gimbal_io_t *io, bk7259_gimbal_scene_t scene, uint32_t revision)
{
    MYBOT_LOGI(TAG, "scene started: %s request=%lu min_period=%ums",
               scene_name(scene), (unsigned long)revision, CONTROL_PERIOD_MS);
    if (io->command_trace)
        MYBOT_LOGI(TAG, "scene phase: %s initial centering", scene_name(scene));
    int ret = center(io, revision, true);
    if (ret)
        return ret;
    uint32_t time = 0;
    while (time < scene_duration(scene)) {
        if (superseded(revision))
            return 1;
        uint32_t next = next_sample(scene, time);
        int16_t pan, pitch;
        scene_sample(scene, next, &pan, &pitch);
        uint32_t start = rtos_get_time();
        if (move_to(io, pan, pitch, next - time) < 0) {
            MYBOT_LOGE(TAG, "scene sample failed: %s time=%ums target=%d,%d",
                       scene_name(scene), (unsigned)next, pan, pitch);
            return -1;
        }
        ret = wait_segment(start, next - time, revision, true);
        if (ret)
            return ret;
        time = next; /* Late segments extend the timeline; never burst to catch up. */
    }
    if (io->command_trace)
        MYBOT_LOGI(TAG, "scene phase: %s return to center", scene_name(scene));
    ret = center(io, revision, true);
    if (!ret)
        MYBOT_LOGI(TAG, "scene completed and centered: %s", scene_name(scene));
    return ret;
}

static bool cli_scene_canceled(uint32_t revision)
{
    if (rtos_lock_mutex(&s_lock) != BK_OK)
        return true;
    bool canceled = !s_gimbal.accepting || s_gimbal.stopping ||
                    s_gimbal.revision != revision;
    unlock();
    return canceled;
}

static int play_cli_scene(gimbal_io_t *io, bk7259_gimbal_scene_t scene,
                          uint32_t revision, bool *started)
{
    *started = false;
    if (cli_scene_canceled(revision))
        return 1;
    MYBOT_LOGI(TAG, "CLI scene synchronizing pose: %s", scene_name(scene));
    /* A raw command may have moved the controller without updating io.
     * Never run a trajectory or skip its initial centering from stale pose. */
    if (query_status(io) < 0) {
        MYBOT_LOGE(TAG, "CLI scene not started: %s; no valid E3 status response (bad_frames=%u)",
                   scene_name(scene), io->bad_frames);
        return -1;
    }
    if (io->flow != 2 || (io->pan_flags & 1U) || (io->pitch_flags & 1U) ||
        io->pan < -PAN_LIMIT - TARGET_TOLERANCE || io->pan > PAN_LIMIT + TARGET_TOLERANCE ||
        io->pitch < PITCH_UP_LIMIT - TARGET_TOLERANCE ||
        io->pitch > PITCH_DOWN_LIMIT + TARGET_TOLERANCE) {
        MYBOT_LOGE(TAG, "CLI scene not started: %s; flow=%u pan=%d pitch=%d flags=0x%02x/0x%02x",
                   scene_name(scene), (unsigned)io->flow, io->pan, io->pitch,
                   (unsigned)io->pan_flags, (unsigned)io->pitch_flags);
        return -1;
    }
    if (cli_scene_canceled(revision))
        return 1;
    io->motion_pending = false; /* Fresh feedback confirms both axes idle. */
    *started = true;
    return play_scene(io, scene, revision);
}

static int wait_ready(gimbal_io_t *io)
{
    uint32_t start = rtos_get_time();
    MYBOT_LOGI(TAG, "waiting for controller homing");
    while (rtos_get_time() - start < HOMING_TIMEOUT_MS && !stopping()) {
        if (manual_control())
            return 1; /* The CLI does not depend on a status-query response. */
        if (query_status(io) == 0 && io->flow == 2 &&
            !(io->pan_flags & 1) && !(io->pitch_flags & 1)) {
            MYBOT_LOGI(TAG, "controller ready: pan=%d pitch=%d (0.1deg)", io->pan, io->pitch);
            return 0;
        }
        bk_err_t ret = rtos_get_semaphore(&s_wake, 250);
        if (ret != BK_OK && ret != kTimeoutErr)
            return -1;
    }
    return -1;
}

static void stop_pending_motion(gimbal_io_t *io)
{
    if (!io->motion_pending)
        return;
    const uint8_t payload[] = {0x03, 0x01}; /* Both axes, energized hold. */
    bool acknowledged = false;
    uint8_t frame[FRAME_BYTES];
    uint32_t start = rtos_get_time();
    if (drain_input(io) == 0 && send_frame(io, 0x64, payload, sizeof(payload)) == 0) {
        for (;;) {
            uint32_t elapsed = rtos_get_time() - start;
            if (elapsed >= COMMAND_ACK_TIMEOUT_MS)
                break;
            int size = receive_frame(io, frame, COMMAND_ACK_TIMEOUT_MS - elapsed);
            if (size <= 0)
                break;
            if (size == 7 && frame[1] == 0xe4 && frame[4] == 0x00) {
                acknowledged = true;
                break;
            }
        }
    }
    /* 0x64 is sequential and does not clear tasks. This is a single fault
     * recovery attempt, never normal preemption, and its ACK alone proves
     * neither that the old task was removed nor that the axes have stopped. */
    if (acknowledged && query_status(io) == 0 && io->flow == 2 &&
        !(io->pan_flags & 1) && !(io->pitch_flags & 1)) {
        io->motion_pending = false;
        MYBOT_LOGW(TAG, "fault recovery: holding stop confirmed");
    } else {
        MYBOT_LOGE(TAG, "fault recovery: physical stop could not be confirmed");
    }
}

static void mark_fault(void)
{
    stop_pending_motion(&s_gimbal.io);
    if (rtos_lock_mutex(&s_lock) == BK_OK) {
        s_gimbal.fault = true;
        s_gimbal.pending_scene = false;
        s_gimbal.pending_center = false;
        unlock();
    }
    notify(&s_centered);
    MYBOT_LOGE(TAG, "controller unavailable; automatic scenes disabled until platform restart (bad_frames=%u)",
               s_gimbal.io.bad_frames);
}

static const struct {
    const char *name;
    size_t length;
    uint8_t bytes[14];
} k_cli_commands[] = {
    {"pan left 30deg", 14, {0x03, 0x61, 0x00, 0x08, 0xd4, 0xfe, 0x00, 0x00,
                             0xf4, 0x01, 0xf4, 0x01, 0x0a, 0xab}},
    {"pitch down 10deg", 14, {0x03, 0x61, 0x00, 0x08, 0x00, 0x00, 0x64, 0x00,
                               0xf4, 0x01, 0xf4, 0x01, 0x11, 0xef}},
    {"pitch up 20deg", 14, {0x03, 0x61, 0x00, 0x08, 0x00, 0x00, 0x38, 0xff,
                             0xf4, 0x01, 0xf4, 0x01, 0x09, 0x67}},
    {"pan relative right 10deg", 12, {0x03, 0x62, 0x00, 0x06, 0x00, 0x00,
                                      0x64, 0x00, 0xf4, 0x01, 0xcf, 0x5a}},
};

/* 0: matching successful ACK, 1: canceled, -1: transaction failed, no retry.
 * A missing relative-move ACK must never cause the same move to accumulate. */
static int cli_command_exchange(gimbal_io_t *io, size_t index)
{
    if (index >= sizeof(k_cli_commands) / sizeof(k_cli_commands[0]))
        return -1;
    const uint8_t *bytes = k_cli_commands[index].bytes;
    size_t length = k_cli_commands[index].length;
    uint8_t expected = bytes[1] | 0x80U;
    unsigned bad_before = io->bad_frames;
    unsigned unexpected = 0;
    int result = -1;
    const char *reason = "IO_ERROR";
    bool tx_ok = false;
    io->command_trace = false;
    io->command_rx_bytes = 0;
    io->command_dump_bytes = 0;
    io->command_bad_crc = 0;
    MYBOT_LOGI(TAG, "UART command start: command=%u/4 %s expected_ack=0x%02x timeout=%ums",
               (unsigned)index + 1U, k_cli_commands[index].name,
               (unsigned)expected, COMMAND_ACK_TIMEOUT_MS);
    if (drain_input(io) < 0 || rtos_lock_mutex(&s_lock) != BK_OK)
        goto finished;
    /* Serialize the finite write with shutdown's rejection of new work. */
    if (!s_gimbal.accepting || s_gimbal.stopping) {
        unlock();
        result = 1;
        reason = "CANCELED";
        goto finished;
    }
    uint32_t start = rtos_get_time();
    while (!bk_uart_is_tx_over(GIMBAL_UART)) {
        if (rtos_get_time() - start >= 20U || rtos_delay_milliseconds(1) != BK_OK) {
            unlock();
            reason = "TX_ERROR";
            goto finished;
        }
    }
    bk_err_t written = bk_uart_write_bytes(GIMBAL_UART, bytes, (uint32_t)length);
    unlock();
    cli_log_hex("TX", bytes, length, 0);
    if (written != BK_OK) {
        reason = "TX_ERROR";
        goto finished;
    }
    tx_ok = true;
    io->command_trace = true;
    start = rtos_get_time();
    uint8_t frame[FRAME_BYTES];
    for (;;) {
        if (stopping()) {
            result = 1;
            reason = "CANCELED";
            break;
        }
        uint32_t elapsed = rtos_get_time() - start;
        if (elapsed >= COMMAND_ACK_TIMEOUT_MS) {
            reason = io->command_rx_bytes ? "NO_MATCHING_ACK" : "NO_RX";
            break;
        }
        int size = receive_frame(io, frame, COMMAND_ACK_TIMEOUT_MS - elapsed);
        if (size <= 0) {
            reason = size < 0 ? "IO_ERROR" : io->command_rx_bytes ? "NO_MATCHING_ACK" : "NO_RX";
            break;
        }
        /* Completion reports and another command's ACK cannot advance this
         * transaction. extract_frame has already verified the wire CRC. */
        if (size != 7 || frame[1] != expected) {
            ++unexpected;
            continue;
        }
        if (frame[4] != 0x00) {
            MYBOT_LOGE(TAG, "UART command LINK_RESPONSE: CRC=OK ack=0x%02x "
                       "result=0x%02x; controller REJECTED command",
                       (unsigned)frame[1], (unsigned)frame[4]);
            reason = "REJECTED";
            break;
        }
        MYBOT_LOGI(TAG, "UART command PASS: CRC=OK ack=0x%02x result=0x00",
                   (unsigned)frame[1]);
        result = 0;
        reason = "PASS";
        break;
    }
finished:
    io->command_trace = false;
    MYBOT_LOGI(TAG, "UART command %s: command=%u tx_ok=%d rx_bytes=%u dumped=%u/%u "
               "bad_frames=%u bad_crc=%u unexpected=%u", reason, (unsigned)index + 1U,
               (int)tx_ok, io->command_rx_bytes, io->command_dump_bytes, COMMAND_DUMP_BYTES,
               io->bad_frames - bad_before, io->command_bad_crc, unexpected);
    return result;
}

static void worker(void *arg)
{
    (void)arg;
    if (wait_ready(&s_gimbal.io) < 0 && !stopping() && !manual_control())
        mark_fault();
    for (;;) {
        if (rtos_lock_mutex(&s_lock) != BK_OK)
            break;
        bool finish = s_gimbal.stopping;
        bool fault = s_gimbal.fault;
        bool manual = s_gimbal.manual_session;
        bool command_pending = manual && s_gimbal.pending_command;
        bool cli_scene_pending = manual && s_gimbal.pending_cli_scene;
        size_t command_index = s_gimbal.command_index;
        uint32_t command_id = s_gimbal.command_sequence;
        bool recenter = !manual && !fault && s_gimbal.pending_center;
        bool scene_pending = !manual && !fault && !recenter && s_gimbal.pending_scene;
        uint32_t revision = s_gimbal.revision;
        uint32_t ticket = s_gimbal.center_ticket;
        bk7259_gimbal_scene_t scene = s_gimbal.scene;
        if (command_pending) {
            s_gimbal.pending_command = false;
            s_gimbal.command_running = true;
        }
        if (cli_scene_pending) {
            s_gimbal.pending_cli_scene = false;
            s_gimbal.cli_scene_running = true;
        }
        if (recenter) s_gimbal.pending_center = false;
        if (scene_pending) s_gimbal.pending_scene = false;
        if (recenter || scene_pending) s_gimbal.normal_busy = true;
        unlock();
        if (finish)
            break;
        if (command_pending) {
            int ret = cli_command_exchange(&s_gimbal.io, command_index);
            MYBOT_LOGI(TAG, "UART command finished: id=%lu command=%u result=%d",
                       (unsigned long)command_id, (unsigned)command_index + 1U, ret);
            if (rtos_lock_mutex(&s_lock) != BK_OK)
                break;
            s_gimbal.command_running = false;
            unlock();
            continue; /* Every frame needs another explicit CLI invocation. */
        }
        if (cli_scene_pending) {
            bool started = false;
            s_gimbal.io.command_trace = true;
            int ret = play_cli_scene(&s_gimbal.io, scene, revision, &started);
            bool canceled = cli_scene_canceled(revision);
            if (ret < 0 && started && !canceled)
                mark_fault();
            MYBOT_LOGI(TAG, "CLI scene finished: %s result=%s", scene_name(scene),
                       canceled || ret > 0 ? "canceled" : ret == 0 ? "completed and centered" :
                       started ? "failed" : "not started");
            s_gimbal.io.command_trace = false;
            if (rtos_lock_mutex(&s_lock) != BK_OK)
                break;
            s_gimbal.cli_scene_running = false;
            unlock();
            continue; /* One complete scene per explicit CLI invocation. */
        }
        if (recenter) {
            int ret = center(&s_gimbal.io, revision, false);
            if (ret < 0) {
                mark_fault();
            } else if (rtos_lock_mutex(&s_lock) == BK_OK) {
                s_gimbal.centered_ticket = ticket;
                unlock();
                notify(&s_centered);
                MYBOT_LOGI(TAG, "centered after conversation cancellation");
            }
            if (rtos_lock_mutex(&s_lock) != BK_OK)
                break;
            s_gimbal.normal_busy = false;
            unlock();
            continue;
        }
        if (scene_pending) {
            int ret = play_scene(&s_gimbal.io, scene, revision);
            if (ret < 0) mark_fault();
            else if (ret > 0)
                MYBOT_LOGI(TAG, "scene replaced/canceled after current segment: %s", scene_name(scene));
            if (rtos_lock_mutex(&s_lock) != BK_OK)
                break;
            s_gimbal.normal_busy = false;
            unlock();
            continue;
        }
        bk_err_t ret = rtos_get_semaphore(&s_wake, 250);
        if (ret != BK_OK && ret != kTimeoutErr) {
            if (!manual_control()) mark_fault();
            break;
        }
    }
    /* Keep prepared/UART ownership until an exiting worker is collected. */
    if (rtos_lock_mutex(&s_lock) == BK_OK) {
        bool canceled = s_gimbal.pending_command;
        bool canceled_scene = s_gimbal.pending_cli_scene;
        bk7259_gimbal_scene_t scene = s_gimbal.scene;
        uint32_t command_id = s_gimbal.command_sequence;
        s_gimbal.accepting = false;
        s_gimbal.stopping = true;
        s_gimbal.pending_command = false;
        s_gimbal.command_running = false;
        s_gimbal.pending_cli_scene = false;
        s_gimbal.cli_scene_running = false;
        s_gimbal.normal_busy = false;
        unlock();
        if (canceled)
            MYBOT_LOGW(TAG, "UART command canceled: id=%lu worker exited", (unsigned long)command_id);
        if (canceled_scene)
            MYBOT_LOGW(TAG, "CLI scene canceled: %s worker exited", scene_name(scene));
    }
    /* Last access to gimbal-owned data. Notifications survive thread deletion. */
    notify(&s_done);
    rtos_delete_thread(NULL);
}

static int release_uart(void)
{
    if (s_gimbal.uart_owned) {
        if (bk_uart_register_rx_isr(GIMBAL_UART, NULL, NULL) != BK_OK ||
            bk_uart_deinit(GIMBAL_UART) != BK_OK)
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
    if (!s_lock && rtos_init_mutex(&s_lock) != BK_OK)
        return -1;
    if ((!s_wake && rtos_init_semaphore(&s_wake, 1) != BK_OK) ||
        (!s_rx && rtos_init_semaphore(&s_rx, 1) != BK_OK) ||
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
    memset(&s_gimbal, 0, sizeof(s_gimbal));
    while (rtos_get_semaphore(&s_done, 0) == BK_OK) { }
    while (rtos_get_semaphore(&s_rx, 0) == BK_OK) { }
    while (rtos_get_semaphore(&s_wake, 0) == BK_OK) { }
    while (rtos_get_semaphore(&s_centered, 0) == BK_OK) { }
    if (bk_uart_init(GIMBAL_UART, &k_uart_config) != BK_OK)
        return -1;
    s_gimbal.uart_owned = true;
    if (gpio_dev_map_by_func(GPIO_DEV_UART2_TXD) != BK_OK)
        goto failed;
    s_gimbal.tx_mapped = true;
    if (gpio_dev_map_by_func(GPIO_DEV_UART2_RXD) != BK_OK)
        goto failed;
    s_gimbal.rx_mapped = true;
    if (bk_uart_register_rx_isr(GIMBAL_UART, uart_rx, NULL) != BK_OK ||
        bk_uart_enable_rx_interrupt(GIMBAL_UART) != BK_OK)
        goto failed;
    /* Keep the logging runtime alive even when a failed product shutdown
     * drops its reference while this worker is retained for a later retry. */
    aosl_ctor();
    s_gimbal.aosl_owned = true;
    if (rtos_lock_mutex(&s_lock) != BK_OK)
        goto failed;
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

int bk7259_gimbal_set_conversation(bool active)
{
    if (!s_lock)
        return active ? -1 : 0;
    if (rtos_lock_mutex(&s_lock) != BK_OK)
        return -1;
    int result = 0;
    if (!s_gimbal.prepared || !s_gimbal.accepting) {
        result = active ? -1 : 0;
    } else if (s_gimbal.conversation != active) {
        s_gimbal.conversation = active;
        if (s_gimbal.manual_session) {
            unlock();
            return 0; /* Manual UART control never schedules a movement. */
        }
        ++s_gimbal.revision;
        if (!active) {
            s_gimbal.pending_scene = false;
            s_gimbal.pending_center = true;
            s_gimbal.center_ticket = s_gimbal.revision;
        }
        notify(&s_wake);
    }
    unlock();
    return result;
}

int bk7259_gimbal_send_next_command(void)
{
    if (!s_lock || rtos_lock_mutex(&s_lock) != BK_OK)
        return -1;
    if (!s_gimbal.prepared || !s_gimbal.accepting || s_gimbal.stopping) {
        unlock();
        return -1;
    }
    if (s_gimbal.conversation || s_gimbal.normal_busy ||
        s_gimbal.pending_command || s_gimbal.command_running ||
        s_gimbal.pending_cli_scene || s_gimbal.cli_scene_running) {
        unlock();
        return -2;
    }
    s_gimbal.manual_session = true;
    s_gimbal.pending_scene = false;
    s_gimbal.pending_center = false;
    ++s_gimbal.revision;
    size_t index = s_gimbal.next_command;
    s_gimbal.command_index = index;
    s_gimbal.next_command = (index + 1U) % (sizeof(k_cli_commands) / sizeof(k_cli_commands[0]));
    uint32_t id = ++s_gimbal.command_sequence;
    s_gimbal.pending_command = true;
    notify(&s_wake);
    unlock();
    MYBOT_LOGI(TAG, "UART command queued: id=%lu command=%u/4 %s; manual control until restart",
               (unsigned long)id, (unsigned)index + 1U, k_cli_commands[index].name);
    return 0;
}

int bk7259_gimbal_request_cli_scene(bk7259_gimbal_scene_t scene)
{
    if (scene < BK7259_GIMBAL_SCENE_HAPPY || scene >= BK7259_GIMBAL_SCENE_COUNT ||
        !s_lock || rtos_lock_mutex(&s_lock) != BK_OK)
        return -1;
    if (!s_gimbal.prepared || !s_gimbal.accepting || s_gimbal.stopping) {
        unlock();
        return -1;
    }
    if (s_gimbal.conversation || s_gimbal.normal_busy ||
        s_gimbal.pending_command || s_gimbal.command_running ||
        s_gimbal.pending_cli_scene || s_gimbal.cli_scene_running) {
        unlock();
        return -2;
    }
    s_gimbal.manual_session = true;
    s_gimbal.pending_scene = false;
    s_gimbal.pending_center = false;
    s_gimbal.scene = scene;
    uint32_t revision = ++s_gimbal.revision;
    s_gimbal.pending_cli_scene = true;
    notify(&s_wake);
    unlock();
    MYBOT_LOGI(TAG, "CLI scene queued: %s request=%lu", scene_name(scene), (unsigned long)revision);
    return 0;
}

int bk7259_gimbal_request(bk7259_gimbal_scene_t scene)
{
    if (scene < BK7259_GIMBAL_SCENE_HAPPY || scene >= BK7259_GIMBAL_SCENE_COUNT || !s_lock)
        return -1;
    if (rtos_lock_mutex(&s_lock) != BK_OK)
        return -1;
    if (!s_gimbal.prepared || !s_gimbal.accepting || !s_gimbal.conversation ||
        s_gimbal.fault || s_gimbal.manual_session) {
        unlock();
        return -1;
    }
    s_gimbal.scene = scene;
    s_gimbal.pending_scene = true;
    uint32_t revision = ++s_gimbal.revision;
    notify(&s_wake);
    unlock();
    MYBOT_LOGI(TAG, "scene accepted: %s request=%lu", scene_name(scene), (unsigned long)revision);
    return 0;
}

int bk7259_gimbal_center_wait(uint32_t timeout_ms)
{
    if (!s_lock)
        return 0;
    if (rtos_lock_mutex(&s_lock) != BK_OK)
        return -1;
    if (!s_gimbal.prepared) {
        unlock();
        return 0;
    }
    if (s_gimbal.manual_session) {
        unlock();
        return -1;
    }
    s_gimbal.conversation = false;
    s_gimbal.pending_scene = false;
    s_gimbal.pending_center = true;
    uint32_t ticket = ++s_gimbal.revision;
    s_gimbal.center_ticket = ticket;
    notify(&s_wake);
    unlock();
    uint32_t start = rtos_get_time();
    for (;;) {
        if (rtos_lock_mutex(&s_lock) != BK_OK)
            return -1;
        bool done = (int32_t)(s_gimbal.centered_ticket - ticket) >= 0;
        bool fault = s_gimbal.fault;
        unlock();
        if (done) return 0;
        uint32_t elapsed = rtos_get_time() - start;
        if (fault || elapsed >= timeout_ms) return -1;
        uint32_t remaining = timeout_ms - elapsed;
        bk_err_t ret = rtos_get_semaphore(&s_centered, remaining > 50U ? 50U : remaining);
        if (ret != BK_OK && ret != kTimeoutErr)
            return -1;
    }
}

int bk7259_gimbal_shutdown(void)
{
    if (!s_lock)
        return 0;
    if (rtos_lock_mutex(&s_lock) != BK_OK)
        return -1;
    bool prepared = s_gimbal.prepared;
    bool manual = s_gimbal.manual_session;
    s_gimbal.accepting = false;
    bool canceled_command = s_gimbal.pending_command;
    bool canceled_scene = s_gimbal.pending_cli_scene;
    bk7259_gimbal_scene_t scene = s_gimbal.scene;
    uint32_t command_id = s_gimbal.command_sequence;
    s_gimbal.pending_command = false;
    s_gimbal.pending_cli_scene = false;
    bool stop_already_requested = s_gimbal.stopping;
    unlock();
    if (canceled_command)
        MYBOT_LOGW(TAG, "UART command canceled: id=%lu shutdown", (unsigned long)command_id);
    if (canceled_scene)
        MYBOT_LOGW(TAG, "CLI scene canceled: %s shutdown", scene_name(scene));
    if (prepared && !stop_already_requested) {
        if (!manual && bk7259_gimbal_center_wait(STOP_TIMEOUT_MS) < 0)
            MYBOT_LOGW(TAG, "centering unavailable before shutdown");
        if (rtos_lock_mutex(&s_lock) != BK_OK)
            return -1;
        s_gimbal.stopping = true;
        /* A fault or homing timeout must not start a late centering request. */
        s_gimbal.pending_center = false;
        ++s_gimbal.revision;
        notify(&s_wake);
        unlock();
    }
    if (prepared && rtos_get_semaphore(&s_done, STOP_TIMEOUT_MS) != BK_OK) {
        MYBOT_LOGE(TAG, "worker exit timed out; retaining UART resources");
        return -1;
    }
    if (prepared) {
        if (rtos_lock_mutex(&s_lock) != BK_OK)
            return -1;
        s_gimbal.prepared = false;
        s_gimbal.worker = NULL;
        unlock();
    }
    if (release_uart() < 0)
        return -1;
    MYBOT_LOGI(TAG, "UART2 worker shut down");
    if (s_gimbal.aosl_owned) {
        aosl_dtor();
        s_gimbal.aosl_owned = false;
    }
    return 0;
}
