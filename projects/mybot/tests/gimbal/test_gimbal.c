/* SPDX-License-Identifier: Apache-2.0 */
/* Exercise the production parser and UART worker against an EB521 V1.2 model. */
#include <assert.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../../../../components/mybot/mybot_sdk/platforms/bk7259/bk7259_gimbal.c"

#define HOST_CLOCK_SCALE 10U
#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))
#define EXPECTED_MOTION_PERIOD_MS 100U
#define EXPECTED_MOTION_REPLY_MS 250U
#define EXPECTED_CHARGE_PERIOD_MS 2000U
#define MODEL_CHARGE_EXPIRY_MS 3000U

struct host_semaphore { pthread_mutex_t mutex; int count, maximum; };
struct host_thread { pthread_t id; void (*function)(void *); void *argument; };

typedef enum {
    RESPONSE_NORMAL,
    RESPONSE_DROP_COMPLETION,
    RESPONSE_CONTINUOUS_NOISE,
    RESPONSE_ACCEPT_ONLY,
} motion_response_t;

typedef struct {
    uint8_t bytes[14];
    uint32_t at;
} wire_record_t;

typedef struct {
    uint32_t at;
    uint8_t command;
    uint8_t control;
    bool test_running;
} tx_event_t;

static const uint8_t charge_on_frame[] = {3, 0x66, 0, 1, 1, 0xbe, 0xd8};
static const uint8_t charge_off_frame[] = {3, 0x66, 0, 1, 0, 0x7f, 0x18};
static const uint8_t charge_ack_frame[] = {3, 0xe6, 0, 1, 0, 0x56, 0xd8};
static const uint8_t charge_invalid_frame[] = {3, 0xe6, 0, 1, 2, 0xd7, 0x19};
static const uint8_t probe_frame[] = {3, 0x66, 0, 1, 2, 0xfe, 0xd9};
static const uint8_t accepted_frame[] = {3, 0xe1, 0, 1, 0, 0x57, 0xac};
static const uint8_t already_at_target_frame[] = {3, 0xe1, 0, 1, 4, 0x56, 0x6f};

typedef struct {
    pthread_mutex_t mutex;
    bool uart_open, tx_mapped, rx_mapped;
    bool rx_irq_enabled, sw_fifo_enabled;
    uint8_t fifo[4096];
    size_t fifo_size;
    unsigned rx_reads, rx_bytes;
    uint32_t next_noise_at;
    wire_record_t motion[1536], charge[128];
    tx_event_t tx_events[1700];
    unsigned motion_count, charge_count, tx_event_count, boot_replies;
    unsigned stop_attempts, probe_count;
    motion_response_t motion_response;
    int16_t pan, pitch, target_pan, target_pitch;
    uint32_t completes_at;
    bool online, active, tx_paused;
    bool drop_next_move, drop_next_home;
    bool hold_home_reply;
    bool pause_after_next_move, fail_next_move_write;
    uint32_t next_move_write_delay_ms;
    unsigned delayed_motion_count;
    bool charging, drop_charge_on_ack, drop_charge_stop;
    unsigned charge_expirations;
    uint32_t charge_expires, next_charge_write_delay_ms;
    bool saved_motion_enabled, saved_charge_enabled;
    bool prefs_load_fails, prefs_save_fails, hold_prefs_save;
    unsigned prefs_save_calls;
} controller_model_t;

static controller_model_t controller = {.mutex = PTHREAD_MUTEX_INITIALIZER};

static uint64_t clock_origin;
static uint32_t fixed_test_time;
static bool deny_done_wait, deny_thread_create;
static unsigned aosl_references, aosl_constructors, aosl_destructors;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned log_calls;

void aosl_ctor(void) { ++aosl_references; ++aosl_constructors; }
void aosl_dtor(void)
{
    assert(aosl_references > 0);
    --aosl_references; ++aosl_destructors;
}

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static void host_sleep_us(unsigned microseconds)
{
    struct timespec duration = {microseconds / 1000000U, (microseconds % 1000000U) * 1000U};
    nanosleep(&duration, NULL);
}

int bk7259_gimbal_prefs_load(bool *motion, bool *charge)
{
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    bool fail = controller.prefs_load_fails;
    *motion = !fail && controller.saved_motion_enabled;
    *charge = !fail && controller.saved_charge_enabled;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    return fail ? -1 : 0;
}

int bk7259_gimbal_prefs_save(bool motion, bool charge)
{
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    ++controller.prefs_save_calls;
    while (controller.hold_prefs_save) {
        assert(pthread_mutex_unlock(&controller.mutex) == 0);
        host_sleep_us(100);
        assert(pthread_mutex_lock(&controller.mutex) == 0);
    }
    bool fail = controller.prefs_save_fails;
    if (!fail) {
        controller.saved_motion_enabled = motion;
        controller.saved_charge_enabled = charge;
    }
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    return fail ? -1 : 0;
}

uint32_t rtos_get_time(void)
{
    if (fixed_test_time)
        return fixed_test_time;
    return (uint32_t)((monotonic_ns() - clock_origin) / (1000000U / HOST_CLOCK_SCALE));
}

static uint16_t wire_crc(const uint8_t *data, size_t size)
{
    uint16_t value = 0xffff;
    for (size_t i = 0; i < size; ++i) {
        value ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            unsigned carry = value & 1;
            value >>= 1;
            if (carry) value ^= 0xa001;
        }
    }
    return value;
}

static size_t make_reply(uint8_t *frame, uint8_t command, const uint8_t *payload, size_t size)
{
    frame[0] = 3; frame[1] = command; frame[2] = (uint8_t)(size >> 8); frame[3] = (uint8_t)size;
    if (size) memcpy(frame + 4, payload, size);
    uint16_t checksum = wire_crc(frame, size + 4);
    frame[size + 4] = (uint8_t)checksum;
    frame[size + 5] = (uint8_t)(checksum >> 8);
    return size + 6;
}

static void deliver_reply_locked(const uint8_t *frame, size_t bytes)
{
    assert(controller.fifo_size + bytes <= sizeof(controller.fifo));
    memcpy(controller.fifo + controller.fifo_size, frame, bytes);
    controller.fifo_size += bytes;
}
static void reply_locked(uint8_t command, const uint8_t *payload, size_t size)
{
    uint8_t frame[14];
    assert(size + 6 <= sizeof(frame));
    size_t bytes = make_reply(frame, command, payload, size);
    deliver_reply_locked(frame, bytes);
}

static void complete_locked(int16_t pan, int16_t pitch)
{
    const uint8_t payload[] = {6, (uint8_t)pan, (uint8_t)((uint16_t)pan >> 8),
                              (uint8_t)pitch, (uint8_t)((uint16_t)pitch >> 8)};
    reply_locked(0xe1, payload, sizeof(payload));
}

static void controller_tick_locked(void)
{
    uint32_t now = rtos_get_time();
    if (controller.next_noise_at && (int32_t)(now - controller.next_noise_at) >= 0) {
        static const uint8_t noise[240];
        if (controller.fifo_size < sizeof(noise)) {
            deliver_reply_locked(noise, sizeof(noise));
        }
        controller.next_noise_at = now + 10U;
    }
    if (controller.charging && (int32_t)(now - controller.charge_expires) >= 0) {
        controller.charging = false;
        ++controller.charge_expirations;
    }
    if (!controller.active || (int32_t)(now - controller.completes_at) < 0) {
        return;
    }
    controller.active = false;
    controller.pan = controller.target_pan;
    controller.pitch = controller.target_pitch;
    if (controller.motion_response == RESPONSE_NORMAL) {
        complete_locked(controller.pan, controller.pitch);
    }
}

static void controller_tick(void)
{
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller_tick_locked();
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
}

bk_err_t rtos_init_mutex(beken_mutex_t *mutex)
{
    *mutex = malloc(sizeof(**mutex));
    assert(*mutex);
    return pthread_mutex_init(*mutex, NULL) == 0 ? BK_OK : -3;
}
bk_err_t rtos_lock_mutex(beken_mutex_t *mutex) { return pthread_mutex_lock(*mutex) == 0 ? BK_OK : -3; }
bk_err_t rtos_unlock_mutex(beken_mutex_t *mutex) { return pthread_mutex_unlock(*mutex) == 0 ? BK_OK : -3; }
bk_err_t rtos_init_semaphore(beken_semaphore_t *semaphore, int maximum)
{
    *semaphore = calloc(1, sizeof(**semaphore));
    assert(*semaphore);
    (*semaphore)->maximum = maximum;
    return pthread_mutex_init(&(*semaphore)->mutex, NULL) == 0 ? BK_OK : -3;
}
bk_err_t rtos_set_semaphore(beken_semaphore_t *semaphore)
{
    assert(*semaphore);
    pthread_mutex_lock(&(*semaphore)->mutex);
    int ret = kTimeoutErr;
    if ((*semaphore)->count < (*semaphore)->maximum) { ++(*semaphore)->count; ret = BK_OK; }
    pthread_mutex_unlock(&(*semaphore)->mutex);
    return ret;
}
bk_err_t rtos_get_semaphore(beken_semaphore_t *semaphore, uint32_t timeout)
{
    if (semaphore == &s_done && timeout && deny_done_wait) {
        deny_done_wait = false;
        return kTimeoutErr;
    }
    uint32_t start = rtos_get_time();
    for (;;) {
        controller_tick();
        pthread_mutex_lock(&(*semaphore)->mutex);
        bool ready = (*semaphore)->count > 0;
        if (ready) --(*semaphore)->count;
        pthread_mutex_unlock(&(*semaphore)->mutex);
        if (ready) return BK_OK;
        if (rtos_get_time() - start >= timeout) return kTimeoutErr;
        host_sleep_us(100);
    }
}
bk_err_t rtos_delay_milliseconds(uint32_t ms)
{
    host_sleep_us((ms * 1000U) / HOST_CLOCK_SCALE);
    controller_tick();
    return BK_OK;
}
static void *thread_entry(void *argument)
{
    struct host_thread *thread = argument;
    thread->function(thread->argument);
    return NULL;
}
bk_err_t rtos_create_psram_thread(beken_thread_t *thread, uint8_t priority,
                                const char *name, void (*function)(void *),
                                uint32_t stack_size, void *argument)
{
    assert(stack_size == 4096 && priority == BEKEN_APPLICATION_PRIORITY);
    if (deny_thread_create) { deny_thread_create = false; return -3; }
    *thread = malloc(sizeof(**thread));
    assert(*thread);
    (*thread)->function = function; (*thread)->argument = argument;
    return pthread_create(&(*thread)->id, NULL, thread_entry, *thread) == 0 ? BK_OK : -3;
}
bk_err_t rtos_delete_thread(beken_thread_t *thread)
{
    assert(!thread);
    pthread_exit(NULL);
}

bk_err_t bk_uart_init(uart_id_t id, const uart_config_t *config)
{
    assert(id == UART_ID_2 && config->baud_rate == 115200 &&
           config->data_bits == UART_DATA_8_BITS && config->parity == UART_PARITY_NONE &&
           config->stop_bits == UART_STOP_BITS_1 && config->flow_ctrl == UART_FLOWCTRL_DISABLE &&
           config->rx_dma_en == UART_DMA_DISABLE && config->tx_dma_en == UART_DMA_DISABLE);
    controller.uart_open = true;
    controller.rx_irq_enabled = true;
    controller.sw_fifo_enabled = true;
    return BK_OK;
}
bk_err_t bk_uart_deinit(uart_id_t id)
{
    assert(id == UART_ID_2);
    controller.uart_open = false;
    return BK_OK;
}
bk_err_t bk_uart_disable_rx_interrupt(uart_id_t id)
{
    assert(id == UART_ID_2 && controller.uart_open);
    controller.rx_irq_enabled = false;
    return BK_OK;
}

bk_err_t bk_uart_disable_sw_fifo(uart_id_t id)
{
    assert(id == UART_ID_2 && controller.uart_open && !controller.rx_irq_enabled);
    controller.sw_fifo_enabled = false;
    return BK_OK;
}

bool bk_uart_is_tx_over(uart_id_t id)
{
    assert(id == UART_ID_2);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    bool ready = !controller.tx_paused;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    return ready;
}
bk_err_t gpio_dev_map_by_func(int function)
{
    if (function == GPIO_DEV_UART2_TXD) controller.tx_mapped = true;
    else if (function == GPIO_DEV_UART2_RXD) controller.rx_mapped = true;
    else assert(0);
    return BK_OK;
}
bk_err_t gpio_dev_unmap_by_func(int function)
{
    if (function == GPIO_DEV_UART2_TXD) controller.tx_mapped = false;
    else if (function == GPIO_DEV_UART2_RXD) controller.rx_mapped = false;
    else assert(0);
    return BK_OK;
}
static uint16_t wire_u16(const uint8_t *data)
{
    return (uint16_t)(data[0] | (uint16_t)data[1] << 8);
}

static bool is_home_command(const uint8_t *frame)
{
    return wire_u16(frame + 4) == 0 && wire_u16(frame + 6) == 0 &&
           wire_u16(frame + 8) == 1000 && wire_u16(frame + 10) == 1000;
}

static uint32_t controller_axis_ms(int16_t start, int16_t target, uint16_t speed)
{
    return (uint32_t)abs((int)target - start) * (5000U / speed) / 10U;
}

static void receive_move_locked(const uint8_t *frame)
{
    int16_t pan = (int16_t)wire_u16(frame + 4);
    int16_t pitch = (int16_t)wire_u16(frame + 6);
    uint16_t pan_speed = wire_u16(frame + 8);
    uint16_t pitch_speed = wire_u16(frame + 10);
    uint8_t result;
    assert(pan >= -900 && pan <= 900 && pitch >= -900 && pitch <= 900);
    assert(pan_speed >= 1 && pan_speed <= 1000 && pitch_speed >= 1 && pitch_speed <= 1000);
    if (!controller.online) {
        return;
    }
    if ((pan != 0 || pitch != 0) && controller.drop_next_move) {
        controller.drop_next_move = false;
        return;
    }
    if (is_home_command(frame) && (controller.drop_next_home || controller.hold_home_reply)) {
        controller.drop_next_home = false;
        return;
    }
    if (controller.boot_replies) {
        --controller.boot_replies;
        result = 1;
        reply_locked(0xe1, &result, 1);
        return;
    }
    if (controller.motion_response == RESPONSE_ACCEPT_ONLY) {
        deliver_reply_locked(accepted_frame, sizeof(accepted_frame));
        return;
    }
    if (!controller.active && pan == controller.pan && pitch == controller.pitch) {
        deliver_reply_locked(already_at_target_frame, sizeof(already_at_target_frame));
        return;
    }

    uint32_t pan_ms = controller_axis_ms(controller.pan, pan, pan_speed);
    uint32_t pitch_ms = controller_axis_ms(controller.pitch, pitch, pitch_speed);
    controller.completes_at = rtos_get_time() + (pan_ms > pitch_ms ? pan_ms : pitch_ms);
    controller.target_pan = pan;
    controller.target_pitch = pitch;
    controller.active = true;
    if (controller.motion_response == RESPONSE_CONTINUOUS_NOISE) {
        controller.next_noise_at = rtos_get_time() + 1U;
    } else {
        deliver_reply_locked(accepted_frame, sizeof(accepted_frame));
    }
}

static bk_err_t receive_charge_locked(const uint8_t *frame)
{
    if (frame[4] == 1) {
        assert(!memcmp(frame, charge_on_frame, sizeof(charge_on_frame)));
        if (!controller.online) {
            return BK_OK;
        }
        controller.charging = true;
        controller.charge_expires = rtos_get_time() + MODEL_CHARGE_EXPIRY_MS;
        if (controller.drop_charge_on_ack) {
            return BK_OK;
        }
    } else {
        assert(frame[4] == 0 && !memcmp(frame, charge_off_frame, sizeof(charge_off_frame)));
        ++controller.stop_attempts;
        if (!controller.online) {
            return BK_OK;
        }
        if (controller.drop_charge_stop) {
            return BK_OK;
        }
        controller.charging = false;
    }
    deliver_reply_locked(charge_ack_frame, sizeof(charge_ack_frame));
    return BK_OK;
}

static void receive_probe_locked(const uint8_t *frame)
{
    assert(!memcmp(frame, probe_frame, sizeof(probe_frame)));
    ++controller.probe_count;
    if (controller.online) {
        deliver_reply_locked(charge_invalid_frame, sizeof(charge_invalid_frame));
    }
}

static void record_tx_event_locked(const uint8_t *frame)
{
    assert(controller.tx_event_count < ARRAY_COUNT(controller.tx_events));
    controller.tx_events[controller.tx_event_count++] = (tx_event_t){
        .at = rtos_get_time(),
        .command = frame[1],
        .control = frame[4],
        .test_running = s_gimbal.test.state == BK7259_GIMBAL_TEST_RUNNING,
    };
}

bk_err_t bk_uart_write_bytes(uart_id_t id, const void *data, uint32_t size)
{
    assert(id == UART_ID_2 && size >= 6 && size <= sizeof(controller.motion[0].bytes));
    const uint8_t *frame = data;
    assert(frame[0] == 3 && size == 6U + frame[2] * 256U + frame[3]);
    assert(wire_crc(frame, size - 2) == wire_u16(frame + size - 2));
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller_tick_locked();
    if (frame[1] == 0x66 && size == sizeof(probe_frame) && frame[4] == 2) {
        record_tx_event_locked(frame);
        receive_probe_locked(frame);
        assert(pthread_mutex_unlock(&controller.mutex) == 0);
        return BK_OK;
    }
    wire_record_t *record;
    if (frame[1] == 0x61) {
        assert(size == 14 && controller.motion_count < ARRAY_COUNT(controller.motion));
        if (controller.next_move_write_delay_ms && !is_home_command(frame)) {
            /* Delay the successful submission before taking its wire timestamp. */
            assert(!controller.fail_next_move_write);
            host_sleep_us(controller.next_move_write_delay_ms * 1000U / HOST_CLOCK_SCALE);
            controller.next_move_write_delay_ms = 0;
            controller.delayed_motion_count = controller.motion_count + 1U;
            controller_tick_locked();
        }
        record = &controller.motion[controller.motion_count++];
    } else {
        assert(frame[1] == 0x66 && size == 7 && controller.charge_count < ARRAY_COUNT(controller.charge));
        if (controller.next_charge_write_delay_ms) {
            host_sleep_us(controller.next_charge_write_delay_ms * 1000U / HOST_CLOCK_SCALE);
            controller.next_charge_write_delay_ms = 0;
            controller_tick_locked();
        }
        record = &controller.charge[controller.charge_count++];
    }
    memcpy(record->bytes, frame, size);
    record->at = rtos_get_time();
    bk_err_t result = BK_OK;
    if (frame[1] == 0x61) {
        if (controller.fail_next_move_write) {
            controller.fail_next_move_write = false;
            result = -1;
        } else {
            receive_move_locked(frame);
        }
        if (controller.pause_after_next_move) {
            controller.pause_after_next_move = false;
            controller.tx_paused = true;
        }
    } else {
        result = receive_charge_locked(frame);
    }
    if (result == BK_OK)
        record_tx_event_locked(frame);
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    return result;
}

bk_err_t uart_read_ready(uart_id_t id)
{
    assert(id == UART_ID_2 && controller.uart_open &&
           !controller.rx_irq_enabled && !controller.sw_fifo_enabled);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller_tick_locked();
    bool ready = controller.fifo_size > 0;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    return ready ? BK_OK : kTimeoutErr;
}

int bk_uart_read_bytes(uart_id_t id, void *data, uint32_t size, uint32_t timeout)
{
    assert(id == UART_ID_2 && size > 0 && timeout == 0 && controller.uart_open &&
           !controller.rx_irq_enabled && !controller.sw_fifo_enabled);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    assert(controller.fifo_size > 0);
    ++controller.rx_reads;
    size_t count = controller.fifo_size < size ? controller.fifo_size : size;
    controller.rx_bytes += count;
    memcpy(data, controller.fifo, count);
    controller.fifo_size -= count;
    memmove(controller.fifo, controller.fifo + count, controller.fifo_size);
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    return (int)count;
}

void aosl_log(int level, const char *format, ...)
{
    char message[512];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    assert(pthread_mutex_lock(&log_lock) == 0);
    ++log_calls;
    assert(pthread_mutex_unlock(&log_lock) == 0);
    if (getenv("GIMBAL_TEST_LOGS")) {
        fputs(message, stderr);
    }
}

static void reset_controller(void)
{
    assert(aosl_references == 1 && !controller.uart_open);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    memset((uint8_t *)&controller + offsetof(controller_model_t, uart_open), 0,
           sizeof(controller) - offsetof(controller_model_t, uart_open));
    controller.online = true;
    controller.saved_motion_enabled = true;
    controller.saved_charge_enabled = true;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    log_calls = 0;
}

static int motion_pan(unsigned index)
{
    return (int16_t)wire_u16(controller.motion[index].bytes + 4);
}

static int motion_pitch(unsigned index)
{
    return (int16_t)wire_u16(controller.motion[index].bytes + 6);
}

static bool home_frame(unsigned index)
{
    return is_home_command(controller.motion[index].bytes);
}

static void wait_for_count(const unsigned *counter, unsigned wanted)
{
    uint64_t deadline = monotonic_ns() + 5000000000ULL;
    for (;;) {
        assert(pthread_mutex_lock(&controller.mutex) == 0);
        unsigned count = *counter;
        assert(pthread_mutex_unlock(&controller.mutex) == 0);
        if (count >= wanted) {
            return;
        }
        assert(monotonic_ns() < deadline);
        host_sleep_us(1000);
    }
}

static unsigned motion_count(void)
{
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    unsigned count = controller.motion_count;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    return count;
}

static uint32_t wait_for_target(unsigned *cursor, int minimum, int maximum)
{
    uint64_t deadline = monotonic_ns() + 3000000000ULL;
    for (;;) {
        assert(pthread_mutex_lock(&controller.mutex) == 0);
        while (*cursor < controller.motion_count) {
            unsigned index = (*cursor)++;
            int pan = motion_pan(index);
            if (pan >= minimum && pan <= maximum && motion_pitch(index) == 0) {
                uint32_t at = controller.motion[index].at;
                assert(pthread_mutex_unlock(&controller.mutex) == 0);
                return at;
            }
        }
        assert(pthread_mutex_unlock(&controller.mutex) == 0);
        assert(monotonic_ns() < deadline);
        host_sleep_us(1000);
    }
}

static unsigned wait_for_home(unsigned start)
{
    uint64_t deadline = monotonic_ns() + 3000000000ULL;
    for (;;) {
        assert(pthread_mutex_lock(&controller.mutex) == 0);
        while (start < controller.motion_count) {
            if (home_frame(start++)) {
                assert(pthread_mutex_unlock(&controller.mutex) == 0);
                return start;
            }
        }
        assert(pthread_mutex_unlock(&controller.mutex) == 0);
        assert(monotonic_ns() < deadline);
        host_sleep_us(1000);
    }
}

static void inject_result_locked(uint8_t result)
{
    reply_locked(0xe1, &result, 1);
}

static void start_and_wait_for_home(void)
{
    assert(bk7259_gimbal_prepare() == 0);
    assert(wait_for_home(0) == 1);
    rtos_delay_milliseconds(100);
}

static void shutdown_and_join(void)
{
    beken_thread_t thread = s_gimbal.worker;
    assert(bk7259_gimbal_shutdown() == 0);
    if (thread) {
        assert(pthread_join(thread->id, NULL) == 0);
        free(thread);
    }
    assert(!controller.uart_open && !controller.tx_mapped && !controller.rx_mapped);
    assert(aosl_references == 1);
}

static bk7259_gimbal_stats_snapshot_t gimbal_stats(void)
{
    bk7259_gimbal_stats_snapshot_t result;
    assert(bk7259_gimbal_get_stats(&result) == 0);
    return result;
}

static void assert_trace_tx_gaps(const bk7259_gimbal_stats_snapshot_t *stats, bool test_only)
{
    uint32_t counts[2] = {0};
    uint32_t minimum[2] = {UINT32_MAX, UINT32_MAX};
    const tx_event_t *previous = NULL;
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    for (unsigned i = 0; i < controller.tx_event_count; ++i) {
        const tx_event_t *event = &controller.tx_events[i];
        if (test_only && !event->test_running) {
            continue;
        }
        if (previous && previous->command != event->command) {
            /* At least 3 ms for a motion write or 2 ms for a 0x66 write,
             * accounting for the serializer, resolution, and a FIFO byte. */
            unsigned direction = previous->command == EB521_MOVE ? 0 : 1;
            uint32_t elapsed = event->at - previous->at;
            uint32_t wire_time = direction == 0 ? 3 : 2;
            assert(elapsed >= wire_time + BK7259_GIMBAL_TX_GUARD_MS);
            uint32_t idle = elapsed - wire_time;
            ++counts[direction];
            if (idle < minimum[direction])
                minimum[direction] = idle;
        }
        previous = event;
    }
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    assert(stats->move_to_charge_pairs == counts[0] && stats->charge_to_move_pairs == counts[1]);
    assert((counts[0] && stats->min_move_to_charge_idle_ms >= BK7259_GIMBAL_TX_GUARD_MS &&
            stats->min_move_to_charge_idle_ms <= minimum[0] + 2) ||
           (!counts[0] && stats->min_move_to_charge_idle_ms == 0));
    assert((counts[1] && stats->min_charge_to_move_idle_ms >= BK7259_GIMBAL_TX_GUARD_MS &&
            stats->min_charge_to_move_idle_ms <= minimum[1] + 2) ||
           (!counts[1] && stats->min_charge_to_move_idle_ms == 0));
}

static void assert_charge_periods_locked(unsigned first, unsigned end, bool enabled)
{
    assert(first < end && end <= controller.charge_count);
    const uint8_t *expected = enabled ? charge_on_frame : charge_off_frame;
    for (unsigned i = first; i < end; ++i) {
        assert(!memcmp(controller.charge[i].bytes, expected, sizeof(charge_on_frame)));
        if (i > first) {
            uint32_t gap = controller.charge[i].at - controller.charge[i - 1U].at;
            assert(gap >= EXPECTED_CHARGE_PERIOD_MS && gap < MODEL_CHARGE_EXPIRY_MS);
        }
    }
}

static void test_periodic_charge_spacing(void)
{
    reset_controller();
    uint32_t prepared_at = rtos_get_time();
    start_and_wait_for_home();
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    assert(controller.charge_count == 1 && controller.charge[0].at - prepared_at < 1000U);
    controller.next_charge_write_delay_ms = 350U;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);

    wait_for_count(&controller.charge_count, 3);
    uint32_t shutdown_at = rtos_get_time();
    shutdown_and_join();
    assert_charge_periods_locked(0, 3, true);
    uint32_t delayed_gap = controller.charge[1].at - controller.charge[0].at;
    uint32_t following_gap = controller.charge[2].at - controller.charge[1].at;
    assert(delayed_gap >= EXPECTED_CHARGE_PERIOD_MS + 350U);
    assert(controller.charge_count == 4 && controller.charge_expirations == 0 && !controller.charging);
    assert(!memcmp(controller.charge[3].bytes, charge_off_frame, sizeof(charge_off_frame)));
    assert(controller.charge[3].at - shutdown_at < 1000U);
    printf("EB521 charging heartbeat gaps %u and %u ms around a 350 ms delayed UART write\n",
           delayed_gap, following_gap);
}

static void test_due_motion_waits_for_charge_tail(void)
{
    /* Step the UART worker's services directly so the boundary checks do
     * not depend on the host scheduler waking within a 1 ms tick. */
    reset_controller();
    controller.uart_open = true;
    fixed_test_time = 1000U;
    gimbal_io_t io = {.charge_due = fixed_test_time};
    s_gimbal = (gimbal_context_t){
        .ready = true,
        .settings = {.motion_enabled = true, .charge_enabled = true},
    };
    bk7259_gimbal_stats_reset(&s_gimbal.stats, fixed_test_time);
    service_charge(&io, fixed_test_time);
    assert(controller.charge_count == 1);
    drain_frames(&io, fixed_test_time);

    uint32_t renewal_at = fixed_test_time + EXPECTED_CHARGE_PERIOD_MS;
    fixed_test_time = renewal_at - 75U;
    s_gimbal.motion = (motion_request_t){
        .kind = MOTION_SCENE, .scene = BK7259_GIMBAL_SCENE_SAD,
        .revision = 1, .started_at = fixed_test_time - 1400U,
    };
    /* The sad motion is already in its steady 100 ms sampling interval. */
    io.motion_revision = s_gimbal.motion.revision;
    io.motion_due = fixed_test_time;
    service_motion(&io, fixed_test_time);
    assert(controller.motion_count == 1 && !home_frame(0));

    fixed_test_time = renewal_at - 1U;
    service_charge(&io, fixed_test_time);
    assert(controller.charge_count == 1);
    ++fixed_test_time;
    drain_frames(&io, fixed_test_time);
    service_charge(&io, fixed_test_time);
    assert(controller.charge_count == 2);
    assert(controller.charge[1].at - controller.charge[0].at == EXPECTED_CHARGE_PERIOD_MS);

    /* The next sample becomes due inside the charge frame's guard. The
     * model budgets a 2 ms UART tail for a 0x66 write. */
    fixed_test_time = renewal_at + 2U + 40U - 1U;
    drain_frames(&io, fixed_test_time);
    service_motion(&io, fixed_test_time);
    assert(controller.motion_count == 1);
    ++fixed_test_time;
    service_motion(&io, fixed_test_time);
    assert(controller.motion_count == 2 && !home_frame(1));
    assert(controller.motion[1].at - controller.charge[1].at == 42U);

    fixed_test_time += EXPECTED_MOTION_PERIOD_MS - 1U;
    drain_frames(&io, fixed_test_time);
    service_motion(&io, fixed_test_time);
    assert(controller.motion_count == 2);
    ++fixed_test_time;
    service_motion(&io, fixed_test_time);
    assert(controller.motion_count == 3 && !home_frame(2));
    assert(controller.motion[2].at - controller.motion[1].at == EXPECTED_MOTION_PERIOD_MS);

    const bk7259_gimbal_stats_snapshot_t *stats = &s_gimbal.stats.snapshot;
    assert(stats->min_move_to_charge_idle_ms == 72U && stats->min_charge_to_move_idle_ms == 40U);
    assert_trace_tx_gaps(stats, false);
    printf("EB521 UART boundary test saw %u ms motion-to-charge and %u ms charge-to-motion idle\n",
           stats->min_move_to_charge_idle_ms, stats->min_charge_to_move_idle_ms);
    controller.uart_open = false;
    s_gimbal = (gimbal_context_t){0};
    fixed_test_time = 0;
}

static void poll_fixed_uart(gimbal_io_t *io, uint32_t now)
{
    fixed_test_time = now;
    sync_home(io, now);
    drain_frames(io, now);
    service_charge(io, now);
    sync_home(io, now);
    if (s_gimbal.ready)
        service_motion(io, now);
    else
        service_home(io, now);
}

static void test_ordinary_motion_acknowledgement(void)
{
    const struct {
        uint8_t code;
        uint32_t ack_after;
        uint32_t send_after;
        bool accepted_first;
    } cases[] = {{4, 80, 100, false}, {4, 140, 140, false},
                 {6, 180, 180, false}, {6, 200, 200, true}};
    for (unsigned i = 0; i < ARRAY_COUNT(cases); ++i) {
        reset_controller();
        controller.uart_open = true;
        controller.drop_next_move = true;
        gimbal_io_t io = {.charge_due = 10000U};
        s_gimbal = (gimbal_context_t){
            .ready = true,
            .settings = {.motion_enabled = true, .charge_enabled = true},
        };
        const uint32_t started = 1000U;
        bk7259_gimbal_stats_reset(&s_gimbal.stats, started);
        queue_scene(BK7259_GIMBAL_SCENE_ANGRY, started);
        poll_fixed_uart(&io, started);
        assert(controller.motion_count == 1 && motion_pan(0) == 17);

        if (cases[i].accepted_first) {
            inject_result_locked(0);
            poll_fixed_uart(&io, started + 5U);
            poll_fixed_uart(&io, started + EXPECTED_MOTION_PERIOD_MS);
            assert(controller.motion_count == 1);
        }
        poll_fixed_uart(&io, started + cases[i].ack_after - 1U);
        assert(controller.motion_count == 1);
        uint8_t code = cases[i].code;
        if (code == 6)
            complete_locked(motion_pan(0), motion_pitch(0));
        else
            inject_result_locked(code);
        poll_fixed_uart(&io, started + cases[i].ack_after);
        if (cases[i].ack_after < cases[i].send_after) {
            assert(controller.motion_count == 1);
            poll_fixed_uart(&io, started + cases[i].send_after - 1U);
            assert(controller.motion_count == 1);
            poll_fixed_uart(&io, started + cases[i].send_after);
        }
        assert(controller.motion_count == 2 && motion_pan(1) == 63);
        assert(controller.motion[1].at - controller.motion[0].at == cases[i].send_after);
        const bk7259_gimbal_stats_counts_t *normal = &s_gimbal.stats.snapshot.groups[BK7259_GIMBAL_STATS_NORMAL];
        assert(normal->tx == 2 && normal->accepted == 1 && normal->expired == 0);
        assert(normal->arrival.reached == 1 && normal->arrival.pending == 1);
        assert(s_gimbal.stats.snapshot.arrival_latency_samples == 1);
        assert(s_gimbal.stats.snapshot.arrival_latency_max_ms == cases[i].ack_after);
        if (cases[i].accepted_first)
            assert(s_gimbal.stats.snapshot.latency_max_ms == 5U);

        controller.uart_open = false;
        s_gimbal = (gimbal_context_t){0};
        fixed_test_time = 0;
    }
}

static void test_ordinary_motion_ack_timeout(void)
{
    reset_controller();
    controller.uart_open = true;
    controller.drop_next_move = true;
    const uint32_t started = 1000U;
    gimbal_io_t io = {.charge_due = started + 150U};
    s_gimbal = (gimbal_context_t){
        .ready = true,
        .settings = {.motion_enabled = true, .charge_enabled = true},
    };
    bk7259_gimbal_stats_reset(&s_gimbal.stats, started);
    queue_scene(BK7259_GIMBAL_SCENE_ANGRY, started);
    poll_fixed_uart(&io, started);
    assert(controller.motion_count == 1 && motion_pan(0) == 17);

    complete_locked(motion_pan(0), motion_pitch(0) + 1);
    inject_result_locked(3);
    uint8_t damaged_reply[sizeof(accepted_frame)];
    memcpy(damaged_reply, accepted_frame, sizeof(damaged_reply));
    damaged_reply[sizeof(damaged_reply) - 1U] ^= 1U;
    deliver_reply_locked(damaged_reply, sizeof(damaged_reply));
    poll_fixed_uart(&io, started + EXPECTED_MOTION_PERIOD_MS);
    assert(controller.motion_count == 1);

    poll_fixed_uart(&io, started + 150U);
    assert(controller.charge_count == 1 && controller.motion_count == 1);
    poll_fixed_uart(&io, started + 192U);
    assert(s_gimbal.stats.snapshot.power.e6_00 == 1);
    assert(controller.motion_count == 1);

    inject_result_locked(0);
    poll_fixed_uart(&io, started + 200U);
    assert(s_gimbal.stats.snapshot.groups[BK7259_GIMBAL_STATS_NORMAL].accepted == 1);
    assert(controller.motion_count == 1);
    poll_fixed_uart(&io, started + EXPECTED_MOTION_REPLY_MS - 1U);
    assert(controller.motion_count == 1);
    poll_fixed_uart(&io, started + EXPECTED_MOTION_REPLY_MS);
    assert(controller.motion_count == 2 && motion_pan(1) == 63);
    assert(s_gimbal.stats.snapshot.groups[BK7259_GIMBAL_STATS_NORMAL].arrival.expired == 1);
    assert(controller.motion[1].at - controller.motion[0].at == EXPECTED_MOTION_REPLY_MS);
    poll_fixed_uart(&io, started + EXPECTED_MOTION_REPLY_MS + EXPECTED_MOTION_PERIOD_MS - 1U);
    assert(controller.motion_count == 2);
    complete_locked(motion_pan(1), motion_pitch(1));
    poll_fixed_uart(&io, started + EXPECTED_MOTION_REPLY_MS + EXPECTED_MOTION_PERIOD_MS);
    assert(controller.motion_count == 3 && motion_pan(2) == 80);

    controller.uart_open = false;
    s_gimbal = (gimbal_context_t){0};
    fixed_test_time = 0;
}

static void test_ordinary_ack_attribution_across_reset(void)
{
    reset_controller();
    assert(mailbox_initialized() && s_wake);
    controller.uart_open = true;
    controller.drop_next_move = true;
    const uint32_t started = 1000U;
    const uint32_t acknowledged = started + 150U;
    const uint32_t timed_out = acknowledged + EXPECTED_MOTION_REPLY_MS;
    gimbal_io_t io = {.charge_due = 10000U};
    s_gimbal = (gimbal_context_t){
        .prepared = true, .accepting = true, .ready = true,
        .settings = {.motion_enabled = true, .charge_enabled = true},
    };
    bk7259_gimbal_stats_reset(&s_gimbal.stats, started);
    assert(bk7259_gimbal_request(BK7259_GIMBAL_SCENE_ANGRY) == 0);
    poll_fixed_uart(&io, started);
    assert(controller.motion_count == 1 && motion_pan(0) == 17);

    inject_result_locked(0);
    poll_fixed_uart(&io, started + 5U);
    fixed_test_time = started + 50U;
    assert(bk7259_gimbal_reset_stats() == 0);
    poll_fixed_uart(&io, started + EXPECTED_MOTION_PERIOD_MS);
    assert(controller.motion_count == 1);
    controller.drop_next_move = true;
    complete_locked(motion_pan(0), motion_pitch(0));
    poll_fixed_uart(&io, acknowledged);
    assert(controller.motion_count == 2 && motion_pan(1) == 63);
    const bk7259_gimbal_stats_snapshot_t *stats = &s_gimbal.stats.snapshot;
    const bk7259_gimbal_stats_counts_t *normal = &stats->groups[BK7259_GIMBAL_STATS_NORMAL];
    assert(normal->tx == 1 && normal->accepted == 0 && stats->latency_samples == 0);

    controller.drop_next_move = true;
    poll_fixed_uart(&io, timed_out);
    assert(controller.motion_count == 3 && motion_pan(2) == 80);
    /* A short reply from the expired 6.3-degree frame is ambiguous. Its
     * coordinate-bearing completion also cannot acknowledge the 8-degree frame. */
    inject_result_locked(4);
    poll_fixed_uart(&io, timed_out + EXPECTED_MOTION_PERIOD_MS);
    assert(controller.motion_count == 3 && stats->ambiguous >= 1);
    complete_locked(motion_pan(1), motion_pitch(1));
    poll_fixed_uart(&io, timed_out + EXPECTED_MOTION_PERIOD_MS + 10U);
    assert(controller.motion_count == 3);

    complete_locked(motion_pan(2), motion_pitch(2));
    poll_fixed_uart(&io, timed_out + EXPECTED_MOTION_PERIOD_MS + 20U);
    assert(controller.motion_count == 4 && motion_pan(3) == 71);
    assert(normal->tx == 3 && normal->accepted == 1 && normal->expired == 1);

    controller.uart_open = false;
    s_gimbal = (gimbal_context_t){0};
    fixed_test_time = 0;
}

static void test_long_ordinary_move_waits_for_arrival(void)
{
    reset_controller();
    const uint32_t started = 1000U;
    controller.uart_open = true;
    controller.drop_next_move = true;
    /* Prior skipped samples leave the estimated pose well left of the next
     * planned waypoint. The PPS on the wire makes this step take >250 ms. */
    controller.pan = -400;
    gimbal_io_t io = {.charge_due = 10000U, .pose_pan = {.start = -400, .target = -400}};
    s_gimbal = (gimbal_context_t){.ready = true, .settings = {.motion_enabled = true}};
    queue_scene(BK7259_GIMBAL_SCENE_ANGRY, started);
    io.motion_revision = s_gimbal.motion.revision;
    poll_fixed_uart(&io, started);
    assert(controller.motion_count == 1 && motion_pan(0) == 17);
    uint16_t pps = wire_u16(controller.motion[0].bytes + 8);
    uint32_t travel_ms = controller_axis_ms(-400, motion_pan(0), pps);
    assert(travel_ms > EXPECTED_MOTION_REPLY_MS);
    inject_result_locked(0);
    poll_fixed_uart(&io, started + 5U);
    poll_fixed_uart(&io, started + travel_ms + 50U);
    assert(controller.motion_count == 1 && io.waiting_motion_ack);
    complete_locked(motion_pan(0), motion_pitch(0));
    poll_fixed_uart(&io, started + travel_ms + 51U);
    assert(controller.motion_count == 2 && motion_pan(1) == 63);
    assert(s_gimbal.stats.snapshot.arrival_latency_max_ms == travel_ms + 51U);
    controller.uart_open = false;
    s_gimbal = (gimbal_context_t){0};
    fixed_test_time = 0;
}

static void test_takeover_with_same_coordinates(void)
{
    reset_controller();
    const uint32_t started = 1000U;
    controller.uart_open = true;
    controller.drop_next_move = true;
    gimbal_io_t io = {.charge_due = 10000U};
    s_gimbal = (gimbal_context_t){.ready = true, .settings = {.motion_enabled = true}};
    queue_scene(BK7259_GIMBAL_SCENE_ANGRY, started);
    poll_fixed_uart(&io, started);
    assert(controller.motion_count == 1 && motion_pan(0) == 17);
    inject_result_locked(0);
    poll_fixed_uart(&io, started + 5U);
    controller.drop_next_move = true;
    s_gimbal.motion = (motion_request_t){
        .kind = MOTION_TARGET, .revision = s_gimbal.motion.revision + 1U,
        .target = {.pan = motion_pan(0)}, .started_at = started + 50U,
    };
    poll_fixed_uart(&io, started + EXPECTED_MOTION_PERIOD_MS - 1U);
    assert(controller.motion_count == 1);
    poll_fixed_uart(&io, started + EXPECTED_MOTION_PERIOD_MS);
    assert(controller.motion_count == 2 && motion_pan(1) == motion_pan(0));
    /* Same coordinates, two generations. A late 06 from the preempted
     * command cannot confirm the current command. */
    complete_locked(motion_pan(0), motion_pitch(0));
    poll_fixed_uart(&io, started + EXPECTED_MOTION_PERIOD_MS + 10U);
    assert(io.waiting_motion_ack);
    const bk7259_gimbal_arrival_counts_t *arrival =
        &s_gimbal.stats.snapshot.groups[BK7259_GIMBAL_STATS_NORMAL].arrival;
    assert(arrival->cancelled == 1 && arrival->reached == 0 && arrival->pending == 1);
    controller.uart_open = false;
    s_gimbal = (gimbal_context_t){0};
    fixed_test_time = 0;
}

static void test_pending_motion_rehomes(bool boot_demo)
{
    reset_controller();
    assert(mailbox_initialized() && s_wake);
    controller.uart_open = true;
    controller.drop_next_move = true;
    const uint32_t started = 1000U;
    const uint32_t hangup_at = started + 50U;
    const uint32_t charge_at = hangup_at + HOME_GUARD_MS - 10U;
    gimbal_io_t io = {.charge_due = charge_at};
    s_gimbal = (gimbal_context_t){
        .prepared = true, .accepting = true, .ready = true,
        .settings = {.motion_enabled = true, .charge_enabled = true},
    };
    bk7259_gimbal_stats_reset(&s_gimbal.stats, started);
    queue_scene(BK7259_GIMBAL_SCENE_ANGRY, started);
    poll_fixed_uart(&io, started);
    assert(controller.motion_count == 1 && motion_pan(0) == 17);

    fixed_test_time = hangup_at;
    if (boot_demo)
        inject_result_locked(1);
    else
        assert(bk7259_gimbal_update_dialogue(false, BK7259_GIMBAL_PHASE_NEUTRAL) == 0);
    poll_fixed_uart(&io, hangup_at);
    assert(!s_gimbal.ready && controller.motion_count == 1);
    poll_fixed_uart(&io, started + EXPECTED_MOTION_REPLY_MS);
    assert(controller.motion_count == 1);
    poll_fixed_uart(&io, charge_at);
    assert(controller.charge_count == 1 && controller.motion_count == 1);
    poll_fixed_uart(&io, charge_at + 41U);
    assert(controller.motion_count == 1);
    poll_fixed_uart(&io, charge_at + 42U);
    assert(controller.motion_count == 2 && home_frame(1));
    assert(controller.motion[1].at - controller.charge[0].at == 42U);
    assert(s_gimbal.stats.snapshot.groups[BK7259_GIMBAL_STATS_NORMAL].tx == 1);

    controller.uart_open = false;
    s_gimbal = (gimbal_context_t){0};
    fixed_test_time = 0;
}

static void test_phase_takeover_uses_pose_at_transmit_slot(void)
{
    reset_controller();
    assert(mailbox_initialized() && s_wake);
    controller.uart_open = true;
    controller.drop_next_move = true;
    fixed_test_time = 1000U;
    gimbal_io_t io = {.charge_due = fixed_test_time};
    s_gimbal = (gimbal_context_t){
        .prepared = true, .accepting = true, .ready = true,
        .settings = {.motion_enabled = true, .charge_enabled = true},
    };
    bk7259_gimbal_stats_reset(&s_gimbal.stats, fixed_test_time);
    service_charge(&io, fixed_test_time);
    drain_frames(&io, fixed_test_time);

    uint32_t opening_at = fixed_test_time + EXPECTED_CHARGE_PERIOD_MS - 90U;
    fixed_test_time = opening_at;
    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_NEUTRAL) == 0);
    service_motion(&io, fixed_test_time);
    assert(controller.motion_count == 1 && motion_pan(0) == 11);
    assert(wire_u16(controller.motion[0].bytes + 8) == 55U);

    fixed_test_time = opening_at + 50U;
    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_LISTENING) == 0);
    drain_frames(&io, fixed_test_time);
    service_motion(&io, fixed_test_time);
    assert(controller.motion_count == 1);

    fixed_test_time = opening_at + 90U;
    service_charge(&io, fixed_test_time);
    assert(controller.charge_count == 2 && controller.charge[1].at == fixed_test_time);
    assert(!memcmp(controller.charge[1].bytes, charge_on_frame, sizeof(charge_on_frame)));

    /* The 100 ms motion interval ends before the 40 ms charge guard. At
     * 132 ms the previous 1.1-degree target has been reached. */
    fixed_test_time = opening_at + 131U;
    drain_frames(&io, fixed_test_time);
    service_motion(&io, fixed_test_time);
    assert(controller.motion_count == 1);
    ++fixed_test_time;
    service_motion(&io, fixed_test_time);
    assert(controller.motion_count == 2 && controller.motion[1].at == opening_at + 132U);
    assert(motion_pan(1) == 0 && motion_pitch(1) == 0 && !home_frame(1));
    assert(io.pose_pan.start == 11 && io.pose_pan.target == 0);
    controller.uart_open = false;
    s_gimbal = (gimbal_context_t){0};
    fixed_test_time = 0;
}

static void test_previous_zero_reply_during_hangup(void)
{
    reset_controller();
    controller.uart_open = true;
    controller.motion_response = RESPONSE_ACCEPT_ONLY;
    fixed_test_time = 1000;
    gimbal_io_t io = {.charge_due = 10000};
    s_gimbal = (gimbal_context_t){
        .prepared = true, .accepting = true, .ready = true,
        .settings = {.motion_enabled = true},
        .motion = {.kind = MOTION_TARGET, .revision = 1},
    };
    service_motion(&io, fixed_test_time);
    assert(controller.motion_count == 1 && !home_frame(0) && motion_pan(0) == 0);
    drain_frames(&io, fixed_test_time);
    assert(bk7259_gimbal_update_dialogue(false, BK7259_GIMBAL_PHASE_NEUTRAL) == 0);
    sync_home(&io, fixed_test_time);
    fixed_test_time = io.home_due;
    service_home(&io, fixed_test_time);
    assert(home_frame(1));
    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_THINKING) == BK7259_GIMBAL_NOT_READY);
    complete_locked(0, 0); /* The earlier ordinary zero also has these coordinates. */
    drain_frames(&io, ++fixed_test_time);
    assert(!s_gimbal.ready && !s_gimbal.stats.snapshot.center_confirmed);
    controller.motion_response = RESPONSE_NORMAL;
    fixed_test_time = io.home_due;
    service_home(&io, fixed_test_time);
    drain_frames(&io, ++fixed_test_time);
    assert(s_gimbal.ready && s_gimbal.stats.snapshot.center_confirmed == 1);
    controller.uart_open = false;
    s_gimbal = (gimbal_context_t){0};
    fixed_test_time = 0;
}

static void test_path_write_failure(bool fail_endpoint)
{
    reset_controller();
    controller.uart_open = true;
    fixed_test_time = 1000U;
    gimbal_io_t io = {.charge_due = 10000U};
    s_gimbal = (gimbal_context_t){
        .ready = true,
        .settings = {.motion_enabled = true, .charge_enabled = true},
    };
    bk7259_gimbal_stats_reset(&s_gimbal.stats, fixed_test_time);
    queue_scene(BK7259_GIMBAL_SCENE_ANGRY, fixed_test_time);

    const uint32_t failed_at = fail_endpoint ? 2100U : 1100U;
    const uint32_t last_sample_at = fail_endpoint ? 2210U : 1200U;
    for (; fixed_test_time <= last_sample_at; fixed_test_time += 10U) {
        drain_frames(&io, fixed_test_time);
        if (fixed_test_time == failed_at)
            controller.fail_next_move_write = true;
        service_motion(&io, fixed_test_time);
    }
    const bk7259_gimbal_stats_counts_t *normal = &s_gimbal.stats.snapshot.groups[BK7259_GIMBAL_STATS_NORMAL];
    assert(normal->tx_failed == 1 && normal->tx + 1U == controller.motion_count);
    assert(normal->tx == controller.tx_event_count);

    if (fail_endpoint) {
        /* The return-to-zero write failed. Only a home reply can confirm center. */
        assert(motion_pan(controller.motion_count - 1U) == 0);
        assert(!home_frame(controller.motion_count - 1U));
        assert(!s_gimbal.ready && controller.pan != 0);
        assert(s_gimbal.stats.snapshot.center_confirmed == 0);
        sync_home(&io, fixed_test_time);
        fixed_test_time = io.home_due;
        unsigned before_home = controller.motion_count;
        service_home(&io, fixed_test_time);
        assert(controller.motion_count == before_home + 1U && home_frame(before_home));
        assert(!s_gimbal.ready && s_gimbal.stats.snapshot.center_confirmed == 0);
        if (!s_centered)
            assert(rtos_init_semaphore(&s_centered, 1) == BK_OK);
        fixed_test_time += HOME_GUARD_MS;
        drain_frames(&io, fixed_test_time);
        assert(s_gimbal.ready && s_gimbal.stats.snapshot.center_confirmed == 1);
    } else {
        /* The 6.3-degree target was missed. The next slot sends 8 degrees
         * from the successful 1.7-degree position with the same 100 ms budget. */
        assert(controller.motion_count == 3 && motion_pan(0) == 17 && motion_pan(1) == 63);
        assert(motion_pan(2) == 80 && controller.motion[2].at == 1200U);
        assert(wire_u16(controller.motion[2].bytes + 8) == 313U);
    }
    controller.uart_open = false;
    s_gimbal = (gimbal_context_t){0};
    fixed_test_time = 0;
}

static void test_terminal_target_write_failure(void)
{
    reset_controller();
    fixed_test_time = 1000U;
    gimbal_io_t io = {.motion_revision = 1, .motion_due = fixed_test_time, .charge_due = 10000U};
    s_gimbal = (gimbal_context_t){
        .ready = true,
        .settings = {.motion_enabled = true, .charge_enabled = true},
        .motion = {.kind = MOTION_TARGET, .revision = 1, .target = {.pan = -100}},
    };
    bk7259_gimbal_stats_reset(&s_gimbal.stats, fixed_test_time);
    controller.fail_next_move_write = true;
    service_motion(&io, fixed_test_time);
    assert(controller.motion_count == 1 && motion_pan(0) == -100);
    fixed_test_time += EXPECTED_MOTION_PERIOD_MS;
    service_motion(&io, fixed_test_time);
    fixed_test_time += EXPECTED_MOTION_PERIOD_MS;
    service_motion(&io, fixed_test_time);
    const bk7259_gimbal_stats_counts_t *normal = &s_gimbal.stats.snapshot.groups[BK7259_GIMBAL_STATS_NORMAL];
    assert(controller.motion_count == 1 && controller.tx_event_count == 0 && controller.pan == 0);
    assert(normal->tx_failed == 1 && normal->tx == 0 && normal->accepted == 0);
    s_gimbal = (gimbal_context_t){0};
    fixed_test_time = 0;
}

static void test_uart_fixed_guard_for_charge_and_one_shot_home(void)
{
    for (uint8_t follow = 0; follow <= 2; follow += 2) {
        reset_controller();
        controller.saved_charge_enabled = follow == 0;
        start_and_wait_for_home();
        assert(pthread_mutex_lock(&controller.mutex) == 0);
        assert(controller.tx_event_count == 2);
        assert(controller.tx_events[0].command == EB521_CHARGE);
        assert(controller.tx_events[0].control == (follow == 0 ? 1 : 0));
        assert(controller.tx_events[1].command == EB521_MOVE);
        controller.tx_paused = true;
        assert(pthread_mutex_unlock(&controller.mutex) == 0);
        assert(bk7259_gimbal_request_motion_enabled(false) == 0);
        if (follow == 0)
            assert(bk7259_gimbal_set_charging(false) == 0);
        else
            assert(bk7259_gimbal_request_probe() == 0);
        assert(pthread_mutex_lock(&controller.mutex) == 0);
        controller.tx_paused = false;
        assert(pthread_mutex_unlock(&controller.mutex) == 0);
        wait_for_count(&controller.tx_event_count, 4);
        rtos_delay_milliseconds(100);
        assert(pthread_mutex_lock(&controller.mutex) == 0);
        assert(controller.motion_count == 2 && home_frame(1));
        assert(controller.tx_events[2].command == EB521_CHARGE);
        assert(controller.tx_events[2].control == follow);
        assert(controller.tx_events[3].command == EB521_MOVE);
        assert(pthread_mutex_unlock(&controller.mutex) == 0);
        bk7259_gimbal_stats_snapshot_t stats = gimbal_stats();
        assert(stats.groups[BK7259_GIMBAL_STATS_HOME].tx == 2);
        assert(stats.groups[BK7259_GIMBAL_STATS_HOME].tx_failed == 0 && stats.e1_04 == 2);
        assert(stats.move_to_charge_pairs == 1 && stats.charge_to_move_pairs == 2);
        assert_trace_tx_gaps(&stats, false);
        shutdown_and_join();
    }
}

static void test_uart_power_stats_and_diagnostic_exclusion(void)
{
    reset_controller();
    controller.saved_motion_enabled = false;
    assert(bk7259_gimbal_prepare() == 0);
    wait_for_count(&controller.charge_count, 1);
    rtos_delay_milliseconds(80);
    bk7259_gimbal_stats_snapshot_t stats = gimbal_stats();
    assert(stats.power.groups[BK7259_GIMBAL_POWER_REQUEST].tx == 1);
    assert(stats.power.groups[BK7259_GIMBAL_POWER_REQUEST].accepted == 1);
    assert(stats.power.groups[BK7259_GIMBAL_POWER_REQUEST].pending == 0);
    assert(stats.power.e6_00 == 1 && stats.power.latency_samples == 1);
    assert(stats.power.latency_max_ms <= 250U);
    assert_trace_tx_gaps(&stats, false);
    assert(stats.move_to_charge_pairs == 0 && stats.charge_to_move_pairs == 0);

    bk7259_gimbal_power_stats_t before_probe = stats.power;
    assert(bk7259_gimbal_request_probe() == 0);
    wait_for_count(&controller.probe_count, 1);
    rtos_delay_milliseconds(80);
    stats = gimbal_stats();
    assert(!memcmp(&stats.power, &before_probe, sizeof(before_probe)));

    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.next_charge_write_delay_ms = 350U;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    uint32_t stop_requested_at = rtos_get_time();
    assert(bk7259_gimbal_set_charging(false) == 0);
    wait_for_count(&controller.charge_count, 2);
    rtos_delay_milliseconds(80);
    stats = gimbal_stats();
    assert(stats.power.groups[BK7259_GIMBAL_POWER_STOP].tx == 1);
    assert(stats.power.groups[BK7259_GIMBAL_POWER_STOP].accepted == 1);
    assert(stats.power.e6_00 == 2 && stats.power.latency_samples == 2);
    assert(stats.power.groups[BK7259_GIMBAL_POWER_STOP].tx_failed == 0);

    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.drop_charge_stop = true;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    wait_for_count(&controller.charge_count, 3);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    assert_charge_periods_locked(1, 3, false);
    assert(controller.charge[1].at - stop_requested_at < 1000U);
    printf("EB521 stop heartbeat gap %u ms after a 350 ms delayed UART write\n",
           controller.charge[2].at - controller.charge[1].at);
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    rtos_delay_milliseconds(320);
    stats = gimbal_stats();
    assert(stats.power.groups[BK7259_GIMBAL_POWER_STOP].tx == 2);
    assert(stats.power.groups[BK7259_GIMBAL_POWER_STOP].accepted == 1);
    assert(stats.power.groups[BK7259_GIMBAL_POWER_STOP].expired == 1);
    assert(stats.power.groups[BK7259_GIMBAL_POWER_STOP].pending == 0);
    assert(stats.power.e6_00 == 2 && stats.power.latency_samples == 2);
    assert_trace_tx_gaps(&stats, false);

    assert(bk7259_gimbal_reset_stats() == 0);
    stats = gimbal_stats();
    assert(stats.power.groups[BK7259_GIMBAL_POWER_REQUEST].tx == 0);
    assert(stats.power.groups[BK7259_GIMBAL_POWER_STOP].tx == 0);
    assert(stats.power.e6_00 == 0 && stats.power.latency_samples == 0);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.drop_charge_stop = false;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    shutdown_and_join();
}

static bk7259_gimbal_stats_snapshot_t wait_for_test_state(bk7259_gimbal_test_state_t state,
                                                         bk7259_gimbal_test_stage_t stage)
{
    uint64_t deadline = monotonic_ns() + 8000000000ULL;
    for (;;) {
        bk7259_gimbal_stats_snapshot_t stats = gimbal_stats();
        if (stats.test_state == state &&
            (stage == BK7259_GIMBAL_TEST_NONE || stats.test_stage == stage))
            return stats;
        assert(monotonic_ns() < deadline);
        host_sleep_us(500);
    }
}

static void assert_test_stage_motion_gap(const bk7259_gimbal_test_counts_t *counts,
                                         unsigned first, unsigned end, const char *stage)
{
    uint32_t minimum = UINT32_MAX;
    unsigned pairs = 0;
    for (unsigned frame = first + 1U; frame < end; ++frame) {
        if (home_frame(frame - 1U) || home_frame(frame))
            continue;
        uint32_t gap = controller.motion[frame].at - controller.motion[frame - 1U].at;
        if (gap < minimum)
            minimum = gap;
        ++pairs;
    }
    assert(pairs > 0 && counts->motion_pairs == pairs);
    assert(minimum >= EXPECTED_MOTION_PERIOD_MS && counts->min_motion_gap_ms >= EXPECTED_MOTION_PERIOD_MS);
    /* The worker samples its timestamp just after the mock write returns. */
    assert(counts->min_motion_gap_ms <= minimum + 2U &&
           minimum <= counts->min_motion_gap_ms + 2U);
    printf("EB521 self-test %s minimum motion gap %u ms in stats and %u ms on UART (%u pairs)\n",
           stage, counts->min_motion_gap_ms, minimum, pairs);
}

static void assert_motion_pacing(unsigned first, unsigned end)
{
    for (unsigned i = first + 1U; i < end; ++i) {
        if (!home_frame(i - 1U) && !home_frame(i))
            assert(controller.motion[i].at - controller.motion[i - 1U].at >= EXPECTED_MOTION_PERIOD_MS);
    }
}

/* Four seconds of speaking should reach both sides on three full cycles. */
static void assert_speaking_swing(unsigned first, unsigned end)
{
    unsigned peaks = 0;
    int side = 0;
    for (unsigned i = first; i < end; ++i) {
        int pan = motion_pan(i);
        assert(motion_pitch(i) == 0 && !home_frame(i));
        /* Speaking blends out of the 12-degree thinking pose. */
        if (controller.motion[i].at - controller.motion[first].at < 240U && pan > 60)
            continue;
        assert(abs(pan) <= 60);
        int direction = pan > 0 ? 1 : -1;
        if (abs(pan) >= 55 && direction != side) {
            side = direction;
            ++peaks;
        }
    }
    assert(peaks >= 6);
}

typedef struct {
    int pan;
    uint32_t travel_ms;
    uint32_t hold_ms;
} expected_pan_key_t;

static const expected_pan_key_t opening_keys[] = {
    {35, 250, 0}, {0, 250, 0},
};
static const expected_pan_key_t angry_keys[] = {
    {80, 300, 0}, {-80, 500, 0}, {0, 410, 0},
};
static const expected_pan_key_t surprised_keys[] = {
    {300, 800, 300}, {0, 650, 0},
};

static void assert_pan_key_sequence(unsigned first, unsigned end, const expected_pan_key_t *keys, size_t count)
{
    int previous_pan = 0;
    uint32_t previous_budget = 0;
    unsigned frame = first;
    for (size_t key = 0; key < count; ++key) {
        int target = keys[key].pan;
        uint32_t remaining = keys[key].travel_ms;
        /* The last sample combines any remainder shorter than 100 ms. */
        while (remaining) {
            uint32_t budget = remaining < 2U * EXPECTED_MOTION_PERIOD_MS ? remaining : EXPECTED_MOTION_PERIOD_MS;
            assert(frame < end && !home_frame(frame) && motion_pitch(frame) == 0);
            if (frame > first)
                assert(controller.motion[frame].at - controller.motion[frame - 1U].at >= previous_budget);

            int pan = motion_pan(frame);
            if (previous_pan < target)
                assert(pan > previous_pan && pan <= target);
            else
                assert(pan < previous_pan && pan >= target);
            unsigned delta = (unsigned)abs(pan - previous_pan);
            uint16_t speed = wire_u16(controller.motion[frame].bytes + 8);
            assert(delta * (5000U / speed) <= 10U * budget);
            assert(speed == 1 || delta * (5000U / (speed - 1U)) > 10U * budget);
            previous_pan = pan;
            previous_budget = budget;
            remaining -= budget;
            ++frame;
        }
        assert(previous_pan == target);
        previous_budget += keys[key].hold_ms;
    }
    if (frame < end)
        assert(controller.motion[frame].at - controller.motion[frame - 1U].at >= previous_budget);
    for (; frame < end; ++frame)
        assert(home_frame(frame));
}

static void assert_test_stage_motion(const bk7259_gimbal_stats_snapshot_t *stats, unsigned first)
{
    const bk7259_gimbal_test_stage_t stages[] = {
        BK7259_GIMBAL_TEST_OPENING, BK7259_GIMBAL_TEST_LISTENING,
        BK7259_GIMBAL_TEST_THINKING, BK7259_GIMBAL_TEST_SPEAKING, BK7259_GIMBAL_TEST_NEUTRAL,
        BK7259_GIMBAL_TEST_HANGUP, BK7259_GIMBAL_TEST_HAPPY, BK7259_GIMBAL_TEST_SAD,
        BK7259_GIMBAL_TEST_ANGRY, BK7259_GIMBAL_TEST_SURPRISED,
    };
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    assert_motion_pacing(first, controller.motion_count);
    for (unsigned i = 0; i < ARRAY_COUNT(stages); ++i) {
        bk7259_gimbal_test_stage_t stage = stages[i];
        unsigned count = stats->test_counts[stage].tx;
        unsigned end = first + count;
        /* An already centered head needs no extra UART write in neutral. */
        assert((count > 0 || stage == BK7259_GIMBAL_TEST_NEUTRAL) && end <= controller.motion_count);
        int minimum_pan = 0;
        int maximum_pan = 0;
        int maximum_pitch = 0;
        for (unsigned frame = first; frame < end; ++frame) {
            int pan = motion_pan(frame);
            int pitch = motion_pitch(frame);
            if (pan < minimum_pan)
                minimum_pan = pan;
            if (pan > maximum_pan)
                maximum_pan = pan;
            if (pitch > maximum_pitch)
                maximum_pitch = pitch;
        }
        switch (stage) {
        case BK7259_GIMBAL_TEST_OPENING:
            assert_pan_key_sequence(first, end, opening_keys, ARRAY_COUNT(opening_keys));
            break;
        case BK7259_GIMBAL_TEST_LISTENING:
            assert(count == 2 && maximum_pitch == 30 && motion_pitch(first) == 30 && motion_pitch(end - 1U) == 0);
            break;
        case BK7259_GIMBAL_TEST_THINKING:
            assert(count == 1 && maximum_pan == 120 && motion_pitch(first) == 0);
            break;
        case BK7259_GIMBAL_TEST_SPEAKING:
            assert_speaking_swing(first, end);
            assert_test_stage_motion_gap(&stats->test_counts[stage], first, end, "speaking");
            break;
        case BK7259_GIMBAL_TEST_HANGUP: assert(home_frame(end - 1)); break;
        case BK7259_GIMBAL_TEST_HAPPY:
            assert(maximum_pan >= 45 && minimum_pan <= -45 && home_frame(end - 1));
            break;
        case BK7259_GIMBAL_TEST_SAD:
            assert(maximum_pitch >= 80 && home_frame(end - 1));
            assert_test_stage_motion_gap(&stats->test_counts[stage], first, end, "sad");
            break;
        case BK7259_GIMBAL_TEST_ANGRY:
            assert_pan_key_sequence(first, end, angry_keys, ARRAY_COUNT(angry_keys));
            assert(home_frame(end - 1));
            break;
        case BK7259_GIMBAL_TEST_SURPRISED:
            assert_pan_key_sequence(first, end, surprised_keys, ARRAY_COUNT(surprised_keys));
            assert(home_frame(end - 1));
            break;
        default: break;
        }
        first = end;
    }
    assert(first == controller.motion_count && controller.pan == 0 && controller.pitch == 0);
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
}

static void test_scheduled_uart_motion_sequence(void)
{
    reset_controller();
    start_and_wait_for_home();
    unsigned initial_moves = motion_count();
    uint32_t scheduled_at = rtos_get_time();
    assert(bk7259_gimbal_schedule_test() == 0);
    assert(gimbal_stats().test_state == BK7259_GIMBAL_TEST_SCHEDULED);
    assert(bk7259_gimbal_schedule_test() == BK7259_GIMBAL_BUSY);
    assert(bk7259_gimbal_reset_stats() == BK7259_GIMBAL_BUSY);
    assert(bk7259_gimbal_send_next_command() == BK7259_GIMBAL_BUSY);
    assert(bk7259_gimbal_request(BK7259_GIMBAL_SCENE_HAPPY) == BK7259_GIMBAL_BUSY);

    rtos_delay_milliseconds(9000);
    assert(motion_count() == initial_moves);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    assert(controller.charge_count >= 4 && controller.charging);
    assert_charge_periods_locked(0, controller.charge_count, true);
    assert(pthread_mutex_unlock(&controller.mutex) == 0);

    (void)wait_for_test_state(BK7259_GIMBAL_TEST_RUNNING, BK7259_GIMBAL_TEST_OPENING);
    wait_for_count(&controller.motion_count, initial_moves + 1U);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    assert(controller.motion[initial_moves].at - scheduled_at >= 10000);
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    assert(bk7259_gimbal_reset_stats() == BK7259_GIMBAL_BUSY);

    (void)wait_for_test_state(BK7259_GIMBAL_TEST_RUNNING, BK7259_GIMBAL_TEST_LISTENING);
    /* Arm the fault after the nod return so the sole thinking frame loses its reply. */
    wait_for_count(&controller.motion_count, motion_count() + 2U);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.drop_next_move = true;
    controller.pause_after_next_move = true;
    unsigned before_dropped_reply = controller.motion_count;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    wait_for_count(&controller.motion_count, before_dropped_reply + 1U);
    rtos_delay_milliseconds(320);
    assert(gimbal_stats().test_counts[BK7259_GIMBAL_TEST_THINKING].expired == 1);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.tx_paused = false;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);

    (void)wait_for_test_state(BK7259_GIMBAL_TEST_RUNNING, BK7259_GIMBAL_TEST_NEUTRAL);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.hold_home_reply = true;
    unsigned before_hangup_home = controller.motion_count;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    (void)wait_for_test_state(BK7259_GIMBAL_TEST_RUNNING, BK7259_GIMBAL_TEST_HANGUP);
    unsigned hangup_home = wait_for_home(before_hangup_home);
    rtos_delay_milliseconds(350);
    assert(gimbal_stats().test_stage == BK7259_GIMBAL_TEST_HANGUP && motion_count() == hangup_home);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.hold_home_reply = false;
    complete_locked(0, 0);
    assert(pthread_mutex_unlock(&controller.mutex) == 0);

    bk7259_gimbal_stats_snapshot_t complete = wait_for_test_state(BK7259_GIMBAL_TEST_COMPLETED,
                                                                BK7259_GIMBAL_TEST_NONE);
    assert(complete.test_counts[BK7259_GIMBAL_TEST_THINKING].expired == 1);
    assert_test_stage_motion(&complete, initial_moves);
    assert(complete.move_to_charge_pairs > 0 && complete.charge_to_move_pairs > 0);
    assert_trace_tx_gaps(&complete, true);
    printf("EB521 self-test UART idle minimum %u ms motion-to-charge and %u ms charge-to-motion\n",
           complete.min_move_to_charge_idle_ms, complete.min_charge_to_move_idle_ms);
    assert(complete.power.groups[BK7259_GIMBAL_POWER_REQUEST].accepted >= 8);
    assert(complete.power.latency_samples >= 8);

    unsigned after_test = motion_count();
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    unsigned after_test_charge = controller.charge_count;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    assert(bk7259_gimbal_send_next_command() == 0);
    wait_for_count(&controller.motion_count, after_test + 2U);
    wait_for_count(&controller.charge_count, after_test_charge + 1U);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    assert_charge_periods_locked(0, controller.charge_count, true);
    assert(controller.charge_expirations == 0);
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    rtos_delay_milliseconds(80);
    bk7259_gimbal_stats_snapshot_t later = gimbal_stats();
    assert(later.test_state == BK7259_GIMBAL_TEST_COMPLETED && later.window_ms == complete.window_ms);
    assert(later.e1_00 == complete.e1_00 && later.center_confirmed == complete.center_confirmed);
    assert(!memcmp(later.groups, complete.groups, sizeof(later.groups)));
    assert(!memcmp(later.test_counts, complete.test_counts, sizeof(later.test_counts)));
    assert(!memcmp(&later.power, &complete.power, sizeof(later.power)));
    assert(later.move_to_charge_pairs == complete.move_to_charge_pairs &&
           later.charge_to_move_pairs == complete.charge_to_move_pairs &&
           later.min_move_to_charge_idle_ms == complete.min_move_to_charge_idle_ms &&
           later.min_charge_to_move_idle_ms == complete.min_charge_to_move_idle_ms);
    assert(bk7259_gimbal_reset_stats() == 0);
    later = gimbal_stats();
    assert(later.test_state == BK7259_GIMBAL_TEST_IDLE);
    assert(later.move_to_charge_pairs == 0 && later.charge_to_move_pairs == 0);
    assert(later.min_move_to_charge_idle_ms == 0 && later.min_charge_to_move_idle_ms == 0);
    assert(later.power.e6_00 == 0 && later.power.latency_samples == 0);
    shutdown_and_join();
}

static void test_limit_sequence(void)
{
    static const int16_t targets[][2] = {
        {0, -300}, {0, 0}, {0, 150}, {0, 0},
        {-900, 0}, {0, 0}, {900, 0}, {0, 0},
        {-900, -300}, {900, 150}, {900, -300}, {-900, 150},
        {900, 150}, {-900, -300}, {-900, 150}, {900, -300}, {0, 0},
    };
    reset_controller();
    /* Only the limit diagnostic may submit the four physical extremes. */
    gimbal_io_t io = {0};
    for (unsigned i = 0; i < 8U; i += 2U)
        assert(!send_move(&io, BK7259_GIMBAL_STATS_NORMAL, targets[i][0], targets[i][1],
                          1000, 1000, 1000));
    assert(motion_count() == 0);
    start_and_wait_for_home();
    unsigned first = motion_count();
    uint32_t scheduled = rtos_get_time();
    assert(bk7259_gimbal_schedule_limit_test() == 0);
    assert(bk7259_gimbal_schedule_test() == BK7259_GIMBAL_BUSY);
    assert(bk7259_gimbal_reset_stats() == BK7259_GIMBAL_BUSY);
    assert(gimbal_stats().limit_test);
    bk7259_gimbal_stats_snapshot_t stats =
        wait_for_test_state(BK7259_GIMBAL_TEST_COMPLETED, BK7259_GIMBAL_TEST_NONE);
    const bk7259_gimbal_stats_counts_t *normal = &stats.groups[BK7259_GIMBAL_STATS_NORMAL];
    assert(stats.limit_test && ARRAY_COUNT(targets) == BK7259_GIMBAL_LIMIT_STEP_COUNT);
    assert(normal->tx == ARRAY_COUNT(targets) && normal->arrival.reached == ARRAY_COUNT(targets));
    assert(stats.groups[BK7259_GIMBAL_STATS_HOME].tx == 0);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    assert(controller.motion_count == first + ARRAY_COUNT(targets));
    assert(controller.motion[first].at - scheduled >= 10000U);
    int16_t from_pan = 0, from_pitch = 0;
    for (unsigned i = 0; i < ARRAY_COUNT(targets); ++i) {
        const wire_record_t *frame = &controller.motion[first + i];
        const bk7259_gimbal_limit_step_t *step = &stats.limit_steps[i];
        int16_t pan = targets[i][0], pitch = targets[i][1];
        unsigned step_number = i + 1;
        bool diagonal = step_number == 10 || step_number == 12 || step_number == 14 || step_number == 16;
        uint16_t pan_pps = wire_u16(frame->bytes + 8);
        uint16_t pitch_pps = wire_u16(frame->bytes + 10);
        assert(motion_pan(first + i) == pan && motion_pitch(first + i) == pitch);
        assert(pan_pps == 1000 && pitch_pps == (diagonal ? 250 : 1000));
        assert(step->pan_pps == pan_pps && step->pitch_pps == pitch_pps);
        assert(step->tx == 1 && step->result == BK7259_GIMBAL_LIMIT_REACHED);
        assert(step->pan == pan && step->pitch == pitch && step->has_ack && step->has_arrival);
        uint32_t pan_ms = controller_axis_ms(from_pan, pan, pan_pps);
        uint32_t pitch_ms = controller_axis_ms(from_pitch, pitch, pitch_pps);
        if (diagonal)
            assert(pan_ms == 900 && pitch_ms == 900);
        uint32_t travel = pan_ms > pitch_ms ? pan_ms : pitch_ms;
        assert(step->ack_ms <= step->arrival_ms && step->arrival_ms > 0);
        if (i + 1 < ARRAY_COUNT(targets)) {
            uint32_t gap = controller.motion[first + i + 1].at - frame->at;
            assert(gap >= travel + 500U && gap >= step->arrival_ms + 500U);
        }
        from_pan = pan;
        from_pitch = pitch;
    }
    assert(controller.pan == 0 && controller.pitch == 0 && controller.charge_expirations == 0);
    assert_charge_periods_locked(0, controller.charge_count, true);
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    rtos_delay_milliseconds(100);
    bk7259_gimbal_stats_snapshot_t frozen = gimbal_stats();
    assert(frozen.window_ms == stats.window_ms);
    assert(!memcmp(frozen.limit_steps, stats.limit_steps, sizeof(stats.limit_steps)));
    assert(bk7259_gimbal_reset_stats() == 0 && !gimbal_stats().limit_test);
    shutdown_and_join();
}

static void test_limit_interruption(bool live_dialogue)
{
    reset_controller();
    start_and_wait_for_home();
    unsigned first = motion_count();
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.motion_response = RESPONSE_DROP_COMPLETION;
    controller.hold_home_reply = true;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    assert(bk7259_gimbal_schedule_limit_test() == 0);
    wait_for_count(&controller.motion_count, first + 1);
    if (live_dialogue) {
        int result = bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_THINKING);
        assert(result == 0 || result == BK7259_GIMBAL_NOT_READY);
    }
    bk7259_gimbal_stats_snapshot_t stats =
        wait_for_test_state(BK7259_GIMBAL_TEST_INTERRUPTED, BK7259_GIMBAL_TEST_NONE);
    const bk7259_gimbal_limit_step_t *step = &stats.limit_steps[0];
    assert(stats.limit_test && step->tx == 1 && !step->has_arrival);
    assert(step->result == (live_dialogue ? BK7259_GIMBAL_LIMIT_CANCELLED : BK7259_GIMBAL_LIMIT_TIMEOUT));
    if (!live_dialogue)
        assert(step->has_ack);
    for (unsigned i = 1; i < BK7259_GIMBAL_LIMIT_STEP_COUNT; ++i)
        assert(stats.limit_steps[i].tx == 0 && stats.limit_steps[i].result == BK7259_GIMBAL_LIMIT_NOT_RUN);
    assert(wait_for_home(first + 1) == first + 2);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    /* The abandoned extremum completes after a recovery home has been sent. */
    complete_locked(0, -300);
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    rtos_delay_milliseconds(100);
    assert(bk7259_gimbal_request(BK7259_GIMBAL_SCENE_SAD) == BK7259_GIMBAL_NOT_READY);
    assert(motion_count() == first + 2);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.motion_response = RESPONSE_NORMAL;
    controller.hold_home_reply = false;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    assert(bk7259_gimbal_center_wait(3500) == 0);
    shutdown_and_join();
}

static void test_scheduled_uart_motion_interruption(void)
{
    reset_controller();
    start_and_wait_for_home();
    assert(bk7259_gimbal_schedule_test() == 0);
    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_LISTENING) == 0);
    bk7259_gimbal_stats_snapshot_t stats = wait_for_test_state(BK7259_GIMBAL_TEST_INTERRUPTED,
                                                             BK7259_GIMBAL_TEST_NONE);
    assert(stats.test_counts[BK7259_GIMBAL_TEST_OPENING].tx == 0);
    assert(bk7259_gimbal_update_dialogue(false, BK7259_GIMBAL_PHASE_NEUTRAL) == 0);
    assert(bk7259_gimbal_center_wait(3000) == 0);
    shutdown_and_join();

    reset_controller();
    controller.online = false;
    assert(bk7259_gimbal_prepare() == 0);
    assert(bk7259_gimbal_schedule_test() == BK7259_GIMBAL_NOT_READY);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.online = true;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    assert(bk7259_gimbal_center_wait(3000) == 0);
    assert(bk7259_gimbal_schedule_test() == 0);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.online = false;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    stats = wait_for_test_state(BK7259_GIMBAL_TEST_INTERRUPTED, BK7259_GIMBAL_TEST_NONE);
    assert(stats.test_counts[BK7259_GIMBAL_TEST_OPENING].tx == 0);
    assert(bk7259_gimbal_schedule_test() == BK7259_GIMBAL_NOT_READY);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.online = true;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    assert(bk7259_gimbal_center_wait(3000) == 0);
    shutdown_and_join();
}

static void test_scheduled_uart_motion_requires_home_ack(void)
{
    reset_controller();
    start_and_wait_for_home();
    assert(bk7259_gimbal_schedule_test() == 0);
    (void)wait_for_test_state(BK7259_GIMBAL_TEST_RUNNING, BK7259_GIMBAL_TEST_NEUTRAL);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.hold_home_reply = true;
    /* Keep ordinary short ACKs and charging active, but withhold position
     * replies so a late ordinary zero-position completion cannot end homing. */
    controller.motion_response = RESPONSE_DROP_COMPLETION;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);

    (void)wait_for_test_state(BK7259_GIMBAL_TEST_RUNNING, BK7259_GIMBAL_TEST_HANGUP);
    bk7259_gimbal_stats_snapshot_t stats = wait_for_test_state(BK7259_GIMBAL_TEST_INTERRUPTED,
                                                             BK7259_GIMBAL_TEST_NONE);
    assert(stats.test_counts[BK7259_GIMBAL_TEST_HANGUP].tx > 0);
    assert(stats.test_counts[BK7259_GIMBAL_TEST_HANGUP].expired > 0);
    assert(stats.test_counts[BK7259_GIMBAL_TEST_HAPPY].tx == 0);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    assert(controller.charging);
    controller.hold_home_reply = false;
    controller.motion_response = RESPONSE_NORMAL;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    assert(bk7259_gimbal_center_wait(3000) == 0);
    shutdown_and_join();
}

static void test_uart_stats_motion_and_reset(void)
{
    reset_controller();
    start_and_wait_for_home();
    bk7259_gimbal_stats_snapshot_t stats = gimbal_stats();
    assert(stats.groups[BK7259_GIMBAL_STATS_HOME].tx == 1 && stats.e1_04 == 1);
    assert(stats.center_confirmed == 1);
    assert(stats.groups[BK7259_GIMBAL_STATS_HOME].accepted == 1);
    assert(stats.groups[BK7259_GIMBAL_STATS_NORMAL].tx == 0);

    /* The worker handles both charging keepalives and a manual E6 probe. */
    assert(bk7259_gimbal_request_probe() == 0);
    wait_for_count(&controller.probe_count, 1);
    wait_for_count(&controller.charge_count, 2);
    rtos_delay_milliseconds(80);
    stats = gimbal_stats();
    assert(stats.e1_00 == 0 && stats.e1_01 == 0 && stats.e1_02 == 0 &&
           stats.e1_04 == 1 && stats.e1_06 == 0);
    assert(stats.unmatched == 0 && stats.bad_reply == 0);

    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.pause_after_next_move = true;
    controller.motion_response = RESPONSE_ACCEPT_ONLY;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    assert(bk7259_gimbal_send_next_command() == 0);
    wait_for_count(&controller.motion_count, 2);
    rtos_delay_milliseconds(80);
    stats = gimbal_stats();
    assert(stats.groups[BK7259_GIMBAL_STATS_NORMAL].tx == 1 && stats.e1_00 == 1);
    assert(stats.groups[BK7259_GIMBAL_STATS_NORMAL].accepted == 1);
    assert(stats.groups[BK7259_GIMBAL_STATS_HOME].accepted == 1);
    assert(stats.latency_samples == 2 && stats.latency_max_ms <= BK7259_GIMBAL_STATS_NORMAL_WINDOW_MS);

    uint8_t damaged_reply[sizeof(accepted_frame)];
    memcpy(damaged_reply, accepted_frame, sizeof(damaged_reply));
    damaged_reply[sizeof(damaged_reply) - 1] ^= 1;
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    complete_locked(motion_pan(1), motion_pitch(1));
    deliver_reply_locked(damaged_reply, sizeof(damaged_reply));
    inject_result_locked(3); /* Valid frame, unknown E1 result. */
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    rtos_delay_milliseconds(80);
    stats = gimbal_stats();
    assert(stats.e1_06 == 1 && stats.bad_reply == 2);
    assert(stats.groups[BK7259_GIMBAL_STATS_NORMAL].accepted == 1);
    assert(stats.unmatched == 0 && stats.latency_samples == 2);

    assert(bk7259_gimbal_reset_stats() == 0);
    stats = gimbal_stats();
    assert(stats.e1_06 == 0 && stats.center_confirmed == 0);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    complete_locked(motion_pan(1), motion_pitch(1));
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    rtos_delay_milliseconds(80);
    stats = gimbal_stats();
    assert(stats.groups[BK7259_GIMBAL_STATS_NORMAL].tx == 0 && stats.e1_00 == 0);
    assert(stats.groups[BK7259_GIMBAL_STATS_HOME].tx == 0 && stats.e1_04 == 0);
    assert(stats.e1_06 == 1 && stats.bad_reply == 0 && stats.latency_samples == 0 &&
           stats.suppressed == 1);

    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.tx_paused = false;
    controller.motion_response = RESPONSE_NORMAL;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    shutdown_and_join();
}

static void test_uart_stats_disable_home_and_tx_error(void)
{
    reset_controller();
    start_and_wait_for_home();

    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.fail_next_move_write = true;
    controller.pause_after_next_move = true;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    assert(bk7259_gimbal_send_next_command() == 0);
    wait_for_count(&controller.motion_count, 2);
    bk7259_gimbal_stats_snapshot_t stats = gimbal_stats();
    assert(stats.groups[BK7259_GIMBAL_STATS_NORMAL].tx == 0);
    assert(stats.groups[BK7259_GIMBAL_STATS_NORMAL].tx_failed == 1);
    assert(stats.groups[BK7259_GIMBAL_STATS_NORMAL].pending == 0);

    assert(bk7259_gimbal_send_next_command() == 0);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.drop_next_move = true;
    controller.pause_after_next_move = true;
    controller.tx_paused = false;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    wait_for_count(&controller.motion_count, 3);
    stats = gimbal_stats();
    assert(stats.groups[BK7259_GIMBAL_STATS_NORMAL].tx == 1);
    assert(stats.groups[BK7259_GIMBAL_STATS_NORMAL].pending == 1);
    rtos_delay_milliseconds(320);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    inject_result_locked(2);
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    rtos_delay_milliseconds(80);
    stats = gimbal_stats();
    assert(stats.groups[BK7259_GIMBAL_STATS_NORMAL].expired == 1);
    assert(stats.groups[BK7259_GIMBAL_STATS_NORMAL].pending == 0);
    assert(stats.groups[BK7259_GIMBAL_STATS_NORMAL].rejected == 0);
    assert(stats.e1_02 == 1 && stats.ambiguous == 1);

    assert(bk7259_gimbal_request_motion_enabled(false) == 0);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.drop_next_home = true;
    controller.tx_paused = false;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    assert(wait_for_home(3) == 4);
    rtos_delay_milliseconds(400);
    stats = gimbal_stats();
    assert(stats.groups[BK7259_GIMBAL_STATS_HOME].tx == 2);
    assert(stats.groups[BK7259_GIMBAL_STATS_HOME].pending == 1);
    rtos_delay_milliseconds(570);
    stats = gimbal_stats();
    assert(stats.groups[BK7259_GIMBAL_STATS_HOME].expired == 1);
    assert(stats.groups[BK7259_GIMBAL_STATS_HOME].pending == 0 && motion_count() == 4);
    shutdown_and_join();
}

static void test_crc_vectors(void)
{
    const struct { const uint8_t *frame; size_t size; } samples[] = {
        {charge_on_frame, sizeof(charge_on_frame)}, {charge_off_frame, sizeof(charge_off_frame)},
        {charge_ack_frame, sizeof(charge_ack_frame)}, {charge_invalid_frame, sizeof(charge_invalid_frame)},
        {probe_frame, sizeof(probe_frame)}, {accepted_frame, sizeof(accepted_frame)},
        {already_at_target_frame, sizeof(already_at_target_frame)},
    };
    for (size_t i = 0; i < ARRAY_COUNT(samples); ++i) {
        const uint8_t *frame = samples[i].frame;
        size_t covered = samples[i].size - 2;
        assert(wire_crc(frame, covered) == wire_u16(frame + covered));
    }
}

static void test_speed_fits_motion_deadline(void)
{
    assert(choose_speed(100, 100) == 455);
    assert(choose_speed(100, 200) == 239);
    for (unsigned interval = 10; interval <= 400; interval += 10) {
        for (unsigned distance = 1; distance <= 300; ++distance) {
            unsigned speed = choose_speed(distance, interval);
            assert(speed >= 1 && speed <= 1000);
            unsigned duration = (distance * (5000U / speed) + 9U) / 10U;
            if (distance * 5U <= interval * 10U) {
                assert(duration <= interval);
                if (speed > 1) {
                    unsigned slower = (distance * (5000U / (speed - 1U)) + 9U) / 10U;
                    assert(slower > interval);
                }
            } else {
                assert(speed == 1000);
            }
        }
    }
}

static void test_listening_nods_use_pitch(uint32_t phase_offset_ms)
{
    reset_controller();
    start_and_wait_for_home();
    if (phase_offset_ms)
        rtos_delay_milliseconds(phase_offset_ms);
    unsigned before_nods = motion_count();
    uint32_t started = rtos_get_time();
    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_LISTENING) == 0);
    rtos_delay_milliseconds(850);
    assert(motion_count() == before_nods);
    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_LISTENING) == 0);
    rtos_delay_milliseconds(1800);
    assert(motion_count() == before_nods);
    /* Observe two complete cycles, including the quiet interval in between. */
    wait_for_count(&controller.motion_count, before_nods + 4U);
    rtos_delay_milliseconds(750);
    unsigned after_nods = motion_count();
    assert(after_nods == before_nods + 4U);
    assert(bk7259_gimbal_update_dialogue(false, BK7259_GIMBAL_PHASE_NEUTRAL) == 0);
    assert(bk7259_gimbal_center_wait(3000) == 0);
    shutdown_and_join();

    uint32_t first_nod_at = controller.motion[before_nods].at;
    assert(first_nod_at - started >= 3450 && first_nod_at - started <= 3800);
    uint32_t between_nods = controller.motion[before_nods + 2U].at - first_nod_at;
    assert(between_nods >= 3350 && between_nods <= 3650);
    for (unsigned down = before_nods; down < after_nods; down += 2U) {
        unsigned up = down + 1U;
        assert(motion_pan(down) == 0 && motion_pitch(down) == 30);
        assert(motion_pan(up) == 0 && motion_pitch(up) == 0 && !home_frame(up));
        assert(wire_u16(controller.motion[down].bytes + 10) == 27);
        assert(wire_u16(controller.motion[up].bytes + 10) == 27);
        assert(controller.motion[up].at - controller.motion[down].at >= 555U);
    }
    assert(controller.pan == 0 && controller.pitch == 0);
    printf("EB521 listening offset %u ms: two nods, four endpoint frames at 27 PPS\n", phase_offset_ms);
}

static void assert_delayed_motion_gap(const char *motion, uint32_t delay_ms)
{
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.next_move_write_delay_ms = delay_ms;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    wait_for_count(&controller.delayed_motion_count, 1);

    assert(pthread_mutex_lock(&controller.mutex) == 0);
    unsigned next = controller.delayed_motion_count;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    wait_for_count(&controller.motion_count, next + 1U);

    assert(pthread_mutex_lock(&controller.mutex) == 0);
    assert(!home_frame(next - 1U) && !home_frame(next));
    uint32_t gap = controller.motion[next].at - controller.motion[next - 1U].at;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    assert(gap >= EXPECTED_MOTION_PERIOD_MS);
    printf("EB521 %s motion gap %u ms after a %u ms delayed UART write\n", motion, gap, delay_ms);
}

static unsigned pause_after_motion(void)
{
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    unsigned count = controller.motion_count + 1U;
    controller.pause_after_next_move = true;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    wait_for_count(&controller.motion_count, count);
    return count;
}

static void resume_motion(void)
{
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.tx_paused = false;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
}

static void test_real_dialogue_interrupts_test_opening(void)
{
    reset_controller();
    start_and_wait_for_home();
    unsigned initial_moves = motion_count();
    assert(bk7259_gimbal_schedule_test() == 0);
    wait_for_count(&controller.motion_count, initial_moves + 2U);
    unsigned first_live = pause_after_motion();
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    assert(motion_pan(first_live - 1U) == 24);
    assert(pthread_mutex_unlock(&controller.mutex) == 0);

    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_NEUTRAL) == 0);
    bk7259_gimbal_stats_snapshot_t interrupted = gimbal_stats();
    assert(interrupted.test_state == BK7259_GIMBAL_TEST_INTERRUPTED &&
           interrupted.test_stage == BK7259_GIMBAL_TEST_OPENING);
    assert(interrupted.test_counts[BK7259_GIMBAL_TEST_OPENING].tx == 3);
    resume_motion();

    unsigned end_opening = first_live;
    (void)wait_for_target(&end_opening, 35, 35);
    unsigned first_return = end_opening;
    (void)wait_for_target(&end_opening, 0, 0);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    for (unsigned frame = first_live; frame < end_opening; ++frame) {
        int pan = motion_pan(frame);
        assert(frame < first_return ? pan >= motion_pan(first_live - 1U) : pan < motion_pan(frame - 1U));
        assert(motion_pitch(frame) == 0 && !home_frame(frame));
    }
    assert_motion_pacing(first_live - 1U, end_opening);
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    shutdown_and_join();
}

static void test_live_phase_replaces_opening(void)
{
    reset_controller();
    start_and_wait_for_home();
    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_NEUTRAL) == 0);
    wait_for_count(&controller.motion_count, 2);
    unsigned first_thinking = pause_after_motion();
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    assert(motion_pan(first_thinking - 1U) == 35);
    assert(pthread_mutex_unlock(&controller.mutex) == 0);

    uint32_t started = rtos_get_time();
    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_THINKING) == 0);
    resume_motion();
    unsigned cursor = first_thinking;
    uint32_t reached = wait_for_target(&cursor, 120, 120);
    assert(cursor == first_thinking + 1U && reached - started <= 300U);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    for (unsigned frame = first_thinking; frame < cursor; ++frame) {
        assert(motion_pan(frame) > 15 && motion_pitch(frame) == 0);
    }
    assert_motion_pacing(first_thinking - 1U, cursor);
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    shutdown_and_join();
}

static void test_thinking_preempts_nod(bool fail_thinking_write)
{
    reset_controller();
    fixed_test_time = 1000U;
    const uint32_t nod_at = fixed_test_time + 3500U;
    controller.uart_open = true;
    controller.motion_response = RESPONSE_ACCEPT_ONLY;
    gimbal_io_t io = {.charge_due = 20000U};
    s_gimbal = (gimbal_context_t){
        .prepared = true, .accepting = true, .ready = true,
        .settings = {.motion_enabled = true},
    };
    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_LISTENING) == 0);
    poll_fixed_uart(&io, fixed_test_time);
    poll_fixed_uart(&io, nod_at);
    assert(controller.motion_count == 1 && motion_pitch(0) == 30);

    poll_fixed_uart(&io, nod_at + 5U);
    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_THINKING) == 0);
    controller.fail_next_move_write = fail_thinking_write;
    poll_fixed_uart(&io, nod_at + EXPECTED_MOTION_PERIOD_MS);
    assert(controller.motion_count == 2 && motion_pan(1) == 120 && motion_pitch(1) == 0);
    complete_locked(0, 30); /* The old nod completion cannot advance the new phase. */
    poll_fixed_uart(&io, nod_at + 105U);
    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_THINKING) == 0);
    for (uint32_t elapsed = 110U; elapsed < 5000U; elapsed += 10U)
        poll_fixed_uart(&io, nod_at + elapsed);

    const bk7259_gimbal_stats_counts_t *normal = &s_gimbal.stats.snapshot.groups[BK7259_GIMBAL_STATS_NORMAL];
    assert(controller.motion_count == 2 && normal->arrival.cancelled == 1);
    assert(normal->tx_failed == (fail_thinking_write ? 1U : 0U));
    assert(normal->arrival.expired == (fail_thinking_write ? 0U : 1U));
    controller.uart_open = false;
    s_gimbal = (gimbal_context_t){0};
    fixed_test_time = 0;
}

static void test_motion_write_pacing(void)
{
    reset_controller();
    start_and_wait_for_home();
    assert(bk7259_gimbal_request(BK7259_GIMBAL_SCENE_SAD) == 0);
    wait_for_count(&controller.motion_count, 3);
    assert_delayed_motion_gap("sad", 45U);

    unsigned next = pause_after_motion();
    /* A target at the last sent pose can be emitted on the first sample
     * before the new blend has moved away from that pose. */
    assert(rtos_lock_mutex(&s_lock) == BK_OK);
    s_gimbal.motion.kind = MOTION_TARGET;
    s_gimbal.motion.target.pan = motion_pan(next - 1U);
    s_gimbal.motion.target.pitch = motion_pitch(next - 1U);
    s_gimbal.motion.started_at = rtos_get_time();
    ++s_gimbal.motion.revision;
    unlock();
    resume_motion();
    wait_for_count(&controller.motion_count, next + 1U);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    assert(!home_frame(next - 1U) && !home_frame(next));
    uint32_t revision_gap = controller.motion[next].at - controller.motion[next - 1U].at;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    assert(revision_gap >= EXPECTED_MOTION_PERIOD_MS);
    shutdown_and_join();

    reset_controller();
    start_and_wait_for_home();
    unsigned before_dialogue = motion_count();
    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_SPEAKING) == 0);
    wait_for_count(&controller.motion_count, before_dialogue + 3U);
    assert_delayed_motion_gap("speaking", 55U);

    next = pause_after_motion();
    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_NEUTRAL) == 0);
    resume_motion();
    wait_for_count(&controller.motion_count, next + 2U);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    assert(!home_frame(next - 1U) && !home_frame(next) && !home_frame(next + 1U));
    uint32_t neutral_first_gap = controller.motion[next].at - controller.motion[next - 1U].at;
    uint32_t neutral_next_gap = controller.motion[next + 1U].at - controller.motion[next].at;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    assert(neutral_first_gap >= EXPECTED_MOTION_PERIOD_MS && neutral_next_gap >= EXPECTED_MOTION_PERIOD_MS);
    printf("EB521 motion revision gap %u ms and neutral motion gaps %u and %u ms\n",
           revision_gap, neutral_first_gap, neutral_next_gap);
    shutdown_and_join();
}

static void test_thinking_endpoint_and_speaking_swing(void)
{
    reset_controller();
    start_and_wait_for_home();
    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_NEUTRAL) == 0);
    rtos_delay_milliseconds(900);
    unsigned cursor = motion_count();
    unsigned first_thinking = cursor;
    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_THINKING) == 0);
    (void)wait_for_target(&cursor, 120, 120);
    assert(cursor == first_thinking + 1U);
    assert(wire_u16(controller.motion[first_thinking].bytes + 8) == 85);
    rtos_delay_milliseconds(4500);
    assert(motion_count() == cursor);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    assert(controller.pan == 120 && controller.pitch == 0);
    assert(pthread_mutex_unlock(&controller.mutex) == 0);

    unsigned first = motion_count();
    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_SPEAKING) == 0);
    rtos_delay_milliseconds(4000);
    unsigned end = motion_count();
    assert(bk7259_gimbal_update_dialogue(false, BK7259_GIMBAL_PHASE_NEUTRAL) == 0);
    assert(bk7259_gimbal_center_wait(3000) == 0);
    shutdown_and_join();
    assert_speaking_swing(first, end);
    assert(controller.pan == 0 && controller.pitch == 0);
}

static void test_boot_wait_and_latest_dialogue(void)
{
    reset_controller();
    controller.online = false;
    assert(bk7259_gimbal_prepare() == 0);
    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_LISTENING) == BK7259_GIMBAL_NOT_READY);
    wait_for_count(&controller.motion_count, 34); /* Longer than the former 30 s homing deadline. */
    assert(bk7259_gimbal_send_next_command() == BK7259_GIMBAL_NOT_READY);
    assert(bk7259_gimbal_request(BK7259_GIMBAL_SCENE_HAPPY) == BK7259_GIMBAL_NOT_READY);
    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_THINKING) == BK7259_GIMBAL_NOT_READY);

    assert(pthread_mutex_lock(&controller.mutex) == 0);
    assert(controller.rx_bytes == 0 && controller.charge_count >= 12);
    for (unsigned i = 0; i < 34; ++i) {
        assert(home_frame(i));
        if (i > 0) {
            uint32_t interval = controller.motion[i].at - controller.motion[i - 1].at;
            assert(interval >= 980 && interval <= 1200);
        }
    }
    assert_charge_periods_locked(0, controller.charge_count, true);
    unsigned begin = controller.motion_count;
    controller.online = true;
    controller.motion_response = RESPONSE_ACCEPT_ONLY;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);

    unsigned accepted_home = wait_for_home(begin);
    rtos_delay_milliseconds(100);
    assert(bk7259_gimbal_send_next_command() == BK7259_GIMBAL_NOT_READY);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    complete_locked(-300, 100); /* Late completion from a previous nonzero target. */
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    rtos_delay_milliseconds(100);
    assert(motion_count() == accepted_home);
    assert(bk7259_gimbal_request(BK7259_GIMBAL_SCENE_HAPPY) == BK7259_GIMBAL_NOT_READY);

    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.motion_response = RESPONSE_NORMAL;
    complete_locked(0, 0);
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    unsigned cursor = accepted_home;
    (void)wait_for_target(&cursor, 120, 120);
    assert(bk7259_gimbal_update_dialogue(false, BK7259_GIMBAL_PHASE_NEUTRAL) == 0);
    assert(bk7259_gimbal_center_wait(3000) == 0);
    shutdown_and_join();
    assert(controller.pan == 0 && controller.pitch == 0 && controller.probe_count == 0);
}

static void test_boot_demo_and_stream_recovery(void)
{
    reset_controller();
    controller.boot_replies = 2;
    controller.motion_response = RESPONSE_ACCEPT_ONLY;
    assert(bk7259_gimbal_prepare() == 0);
    unsigned pending_home = wait_for_home(2);
    static const uint8_t zeros[600];
    uint8_t bad_ack[sizeof(already_at_target_frame)];
    memcpy(bad_ack, already_at_target_frame, sizeof(bad_ack));
    bad_ack[sizeof(bad_ack) - 1] ^= 1;
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    deliver_reply_locked(zeros, sizeof(zeros));
    reply_locked(0xe1, bad_ack + 4, 2); /* Valid CRC, invalid result shape. */
    deliver_reply_locked(bad_ack, sizeof(bad_ack));
    inject_result_locked(2); /* A command rejection cannot open the home gate. */
    deliver_reply_locked(charge_ack_frame, sizeof(charge_ack_frame));
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    rtos_delay_milliseconds(100);
    assert(bk7259_gimbal_send_next_command() == BK7259_GIMBAL_NOT_READY);

    uint8_t completion[11];
    const uint8_t centered[] = {6, 0, 0, 0, 0};
    size_t size = make_reply(completion, 0xe1, centered, sizeof(centered));
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    deliver_reply_locked(completion, 3);
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    rtos_delay_milliseconds(30);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.motion_response = RESPONSE_NORMAL;
    deliver_reply_locked(completion + 3, size - 3);
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    rtos_delay_milliseconds(100);
    assert(bk7259_gimbal_send_next_command() == 0);
    unsigned cursor = pending_home;
    (void)wait_for_target(&cursor, -300, -300);
    shutdown_and_join();
    assert(motion_pan(pending_home) < 0 && motion_pan(pending_home) > -300);
    assert(controller.rx_bytes > 600 && controller.rx_reads > 3 && log_calls < 55);
}

static void test_scene_skips_missing_replies_and_confirms_home(void)
{
    reset_controller();
    start_and_wait_for_home();
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.drop_next_move = true;
    controller.drop_next_home = true;
    controller.motion_response = RESPONSE_DROP_COMPLETION;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    assert(bk7259_gimbal_request(BK7259_GIMBAL_SCENE_SURPRISED) == 0);
    wait_for_count(&controller.motion_count, 4);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    assert(motion_pan(1) > 0 && motion_pan(2) > motion_pan(1));
    assert_motion_pacing(1, controller.motion_count);
    /* The first move has no replies and the second has only a start ACK.
     * Both must wait for their arrival deadlines before the path advances. */
    for (unsigned i = 2; i <= 3; ++i)
        assert(controller.motion[i].at - controller.motion[i - 1U].at >= EXPECTED_MOTION_REPLY_MS);
    assert(pthread_mutex_unlock(&controller.mutex) == 0);

    unsigned missed_home = wait_for_home(1);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    assert_pan_key_sequence(1, missed_home, surprised_keys, ARRAY_COUNT(surprised_keys));
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    assert(bk7259_gimbal_request(BK7259_GIMBAL_SCENE_HAPPY) == BK7259_GIMBAL_NOT_READY);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.motion_response = RESPONSE_NORMAL;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    unsigned confirmed_home = wait_for_home(missed_home);
    rtos_delay_milliseconds(260);
    assert(bk7259_gimbal_send_next_command() == 0);
    unsigned cursor = confirmed_home;
    (void)wait_for_target(&cursor, -300, -300);
    shutdown_and_join();
    assert(controller.motion[confirmed_home - 1].at - controller.motion[missed_home - 1].at >= 980);
    assert(motion_pan(confirmed_home) < 0 && motion_pan(confirmed_home) > -300);
}

static void test_four_preset_trajectories(void)
{
    for (int scene = 0; scene < BK7259_GIMBAL_SCENE_COUNT; ++scene) {
        reset_controller();
        start_and_wait_for_home();
        assert(bk7259_gimbal_request((bk7259_gimbal_scene_t)scene) == 0);
        unsigned returned = wait_for_home(1);
        rtos_delay_milliseconds(1300);
        shutdown_and_join();
        assert_motion_pacing(1, returned);
        int pan_min = 0, pan_max = 0, pitch_max = 0;
        for (unsigned i = 1; i < returned; ++i) {
            int pan = motion_pan(i);
            int pitch = motion_pitch(i);
            if (pan < pan_min) {
                pan_min = pan;
            }
            if (pan > pan_max) {
                pan_max = pan;
            }
            if (pitch > pitch_max) {
                pitch_max = pitch;
            }
            assert(pitch >= 0 && (scene == BK7259_GIMBAL_SCENE_SAD || pitch == 0));
            if (i + 1 < returned) {
                uint32_t budget = controller.motion[i + 1].at - controller.motion[i].at;
                const uint8_t *frame = controller.motion[i].bytes;
                uint32_t pan_ms = controller_axis_ms(motion_pan(i - 1), pan, wire_u16(frame + 8));
                uint32_t pitch_ms = controller_axis_ms(motion_pitch(i - 1), pitch, wire_u16(frame + 10));
                assert(pan_ms <= budget + 10U && pitch_ms <= budget + 10U);
            }
        }
        switch (scene) {
        case BK7259_GIMBAL_SCENE_HAPPY:
            assert(pan_min >= -60 && pan_min <= -54 && pan_max >= 54 && pan_max <= 60);
            assert(abs(motion_pan(1)) < pan_max / 2);
            break;
        case BK7259_GIMBAL_SCENE_SAD:
            assert(pan_min == 0 && pan_max == 0 && pitch_max >= 94 && pitch_max <= 100);
            break;
        case BK7259_GIMBAL_SCENE_ANGRY:
            assert_pan_key_sequence(1, returned, angry_keys, ARRAY_COUNT(angry_keys));
            assert(pan_min == -80 && pan_max == 80);
            break;
        case BK7259_GIMBAL_SCENE_SURPRISED:
            assert_pan_key_sequence(1, returned, surprised_keys, ARRAY_COUNT(surprised_keys));
            assert(pan_min == 0 && pan_max == 300);
            break;
        }
        assert(abs(motion_pan(returned - 2)) <= 15 && abs(motion_pitch(returned - 2)) <= 15);
        assert(controller.pan == 0 && controller.pitch == 0);
    }
}

static void test_peer_loss_discards_cli_and_reloads_dialogue(void)
{
    reset_controller();
    start_and_wait_for_home();
    assert(bk7259_gimbal_request(BK7259_GIMBAL_SCENE_SAD) == 0);
    wait_for_count(&controller.motion_count, 3);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.online = false;
    controller.active = false;
    unsigned disconnected = controller.motion_count;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    unsigned lost_home = wait_for_home(disconnected);
    assert(bk7259_gimbal_send_next_command() == BK7259_GIMBAL_NOT_READY);
    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_SPEAKING) == BK7259_GIMBAL_NOT_READY);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.online = true;
    controller.pan = 100;
    controller.boot_replies = 1;
    complete_locked(0, 100); /* Queued stale CLI completion after reconnect. */
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    unsigned ready_home = wait_for_home(lost_home + 1);
    unsigned cursor = ready_home;
    (void)wait_for_target(&cursor, -60, -45);
    assert(bk7259_gimbal_update_dialogue(false, BK7259_GIMBAL_PHASE_NEUTRAL) == 0);
    assert(bk7259_gimbal_center_wait(3000) == 0);
    shutdown_and_join();
    for (unsigned i = lost_home - 1; i < controller.motion_count; ++i) {
        assert(motion_pitch(i) == 0);
    }
    assert(controller.pan == 0 && controller.pitch == 0);
}

static void test_cli_and_dialogue_share_motion(void)
{
    reset_controller();
    start_and_wait_for_home();
    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_NEUTRAL) == 0);
    /* Let the 500 ms opening settle before checking the scene transition. */
    rtos_delay_milliseconds(900);
    unsigned before_scene = motion_count();
    assert(bk7259_gimbal_request(BK7259_GIMBAL_SCENE_SAD) == 0);
    wait_for_count(&controller.motion_count, before_scene + 1);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    assert(motion_pitch(before_scene) > 0);
    assert(pthread_mutex_unlock(&controller.mutex) == 0);

    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_THINKING) == 0);
    unsigned cursor = before_scene + 1;
    (void)wait_for_target(&cursor, 120, 120);
    assert(bk7259_gimbal_send_next_command() == 0);
    (void)wait_for_target(&cursor, -300, -300);
    /* A new request for the same dialogue phase supersedes the CLI target. */
    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_THINKING) == 0);
    unsigned first_thinking = cursor;
    (void)wait_for_target(&cursor, 120, 120);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    assert(cursor == first_thinking + 1U);
    /* Even when taking over from the far left, the direct endpoint obeys
     * the dialogue speed bound of 40 degrees/s (25 ms per degree). */
    uint16_t speed = wire_u16(controller.motion[first_thinking].bytes + 8);
    assert(speed && 5000U / speed >= 25U);
    assert(pthread_mutex_unlock(&controller.mutex) == 0);

    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_SPEAKING) == 0);
    (void)wait_for_target(&cursor, 45, 60);
    assert(bk7259_gimbal_update_dialogue(false, BK7259_GIMBAL_PHASE_NEUTRAL) == 0);
    (void)wait_for_home(cursor);
    assert(bk7259_gimbal_center_wait(3000) == 0);
    shutdown_and_join();
    assert(controller.pan == 0 && controller.pitch == 0);
}

static void test_explicit_end_interrupts_scene_while_dialogue_inactive(void)
{
    reset_controller();
    start_and_wait_for_home();
    assert(bk7259_gimbal_request(BK7259_GIMBAL_SCENE_SAD) == 0);
    wait_for_count(&controller.motion_count, 3);
    unsigned after_end = pause_after_motion();
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    assert(motion_pitch(after_end - 1U) > 0);
    controller.drop_next_home = true;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);

    assert(bk7259_gimbal_update_dialogue(false, BK7259_GIMBAL_PHASE_NEUTRAL) == 0);
    assert(motion_count() == after_end);
    resume_motion();
    unsigned missed_home = wait_for_home(after_end);
    assert(bk7259_gimbal_request(BK7259_GIMBAL_SCENE_HAPPY) == BK7259_GIMBAL_NOT_READY);
    unsigned retry = wait_for_home(missed_home);
    assert(bk7259_gimbal_center_wait(3000) == 0);
    shutdown_and_join();
    assert(controller.motion[retry - 1].at - controller.motion[missed_home - 1].at >= 980);
    for (unsigned i = after_end; i < controller.motion_count; ++i) {
        assert(home_frame(i));
    }
    assert(controller.pan == 0 && controller.pitch == 0);
}

static void test_heartbeat_loss_reestablishes_center(void)
{
    reset_controller();
    start_and_wait_for_home();
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.drop_charge_on_ack = true;
    unsigned current_charge = controller.charge_count;
    unsigned before = controller.motion_count;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    unsigned lost_home = wait_for_home(before);
    assert(lost_home > before);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    uint32_t link_loss_gap = controller.motion[lost_home - 1U].at - controller.motion[before - 1U].at;
    assert(link_loss_gap >= 3250U && link_loss_gap < 3650U);
    assert(controller.charge_count > current_charge && controller.charging);
    controller.drop_charge_on_ack = false;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    assert(bk7259_gimbal_center_wait(3000) == 0);
    shutdown_and_join();
}

static void test_sustained_invalid_motion_replies_continue_with_charge_ack(void)
{
    reset_controller();
    start_and_wait_for_home();
    /* Start motion shortly before the next heartbeat so charging replies
     * are interleaved with the corrupt movement stream. */
    rtos_delay_milliseconds(1600);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.motion_response = RESPONSE_CONTINUOUS_NOISE;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    uint32_t started = rtos_get_time();
    assert(bk7259_gimbal_request(BK7259_GIMBAL_SCENE_HAPPY) == 0);
    wait_for_count(&controller.motion_count, 3);
    unsigned scene_home = wait_for_home(1);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    uint32_t elapsed = controller.motion[scene_home - 1U].at - started;
    assert(elapsed >= 2000U && controller.charging && scene_home >= 7U);
    for (unsigned frame = 2; frame < scene_home - 1U; ++frame) {
        assert(controller.motion[frame].at - controller.motion[frame - 1U].at >= EXPECTED_MOTION_REPLY_MS);
        assert(motion_pan(frame) != motion_pan(frame - 1U));
    }
    assert(controller.rx_bytes > 600);
    assert(controller.charge_count > 1);
    assert(controller.charge[1].at > controller.motion[1].at &&
           controller.charge[1].at < controller.motion[scene_home - 1U].at);
    controller.next_noise_at = 0;
    controller.motion_response = RESPONSE_NORMAL;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    assert(gimbal_stats().power.e6_00 >= 2);
    assert(bk7259_gimbal_center_wait(3000) == 0);
    shutdown_and_join();
    assert(controller.pan == 0 && controller.pitch == 0 && log_calls < 60);
}

static void test_probe_and_charge_policy(void)
{
    reset_controller();
    start_and_wait_for_home();
    assert(bk7259_gimbal_request_probe() == 0);
    wait_for_count(&controller.probe_count, 1);
    rtos_delay_milliseconds(1100);
    assert(motion_count() == 1);
    assert(bk7259_gimbal_set_charging(false) == 0);
    rtos_delay_milliseconds(2450);
    assert(motion_count() == 1);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    assert(controller.stop_attempts >= 2 && !controller.charging);
    unsigned first_off = 0;
    while (controller.charge[first_off].bytes[4] == 1) {
        ++first_off;
        assert(first_off < controller.charge_count);
    }
    for (unsigned i = first_off; i < controller.charge_count; ++i) {
        assert(!memcmp(controller.charge[i].bytes, charge_off_frame, sizeof(charge_off_frame)));
    }
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    assert(bk7259_gimbal_set_charging(true) == 0);
    rtos_delay_milliseconds(1100);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    unsigned last_on = controller.charge_count - 1U;
    assert(controller.charging && controller.charge[last_on].bytes[4] == 1);
    assert(controller.charge[last_on].at - controller.charge[last_on - 1U].at < EXPECTED_CHARGE_PERIOD_MS);
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    assert(bk7259_gimbal_stop_charging_wait(1800) == 0);
    assert(bk7259_gimbal_set_charging(true) < 0);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    unsigned final_off = controller.charge_count - 1;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    rtos_delay_milliseconds(1300);
    shutdown_and_join();
    assert(!controller.charging && controller.probe_count == 1);
    for (unsigned i = final_off; i < controller.charge_count; ++i) {
        assert(controller.charge[i].bytes[4] == 0);
    }
}

static void test_shutdown_unconfirmed_off_stops_renewals(void)
{
    reset_controller();
    start_and_wait_for_home();
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    assert(controller.charging);
    controller.drop_charge_stop = true;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    assert(bk7259_gimbal_stop_charging_wait(1000) < 0);
    assert(bk7259_gimbal_set_charging(true) < 0);
    shutdown_and_join();
    assert(controller.stop_attempts >= 1);
    assert(controller.charge[controller.charge_count - 1].bytes[4] == 0);
    rtos_delay_milliseconds(3000);
    assert(!controller.charging);
}

static void test_worker_lifecycle(void)
{
    reset_controller();
    assert(bk7259_gimbal_set_charging(true) < 0);
    start_and_wait_for_home();
    assert(bk7259_gimbal_prepare() == 0 && aosl_references == 2);
    deny_done_wait = true;
    assert(bk7259_gimbal_shutdown() < 0);
    assert(controller.uart_open && controller.tx_mapped && controller.rx_mapped);
    shutdown_and_join();

    reset_controller();
    deny_thread_create = true;
    assert(bk7259_gimbal_prepare() < 0);
    assert(aosl_references == 1 && !controller.uart_open);
}

static bk7259_gimbal_settings_t wait_for_settings_save(void)
{
    uint64_t deadline = monotonic_ns() + 1000000000ULL;
    for (;;) {
        bk7259_gimbal_settings_t settings;
        assert(bk7259_gimbal_get_settings(&settings) == 0);
        if (!settings.pending)
            return settings;
        assert(monotonic_ns() < deadline);
        host_sleep_us(100);
    }
}

static void test_independent_settings_survive_reprepare(void)
{
    reset_controller();
    controller.saved_motion_enabled = false;
    assert(bk7259_gimbal_prepare() == 0);
    wait_for_count(&controller.charge_count, 2);
    assert(motion_count() == 0 && bk7259_gimbal_send_next_command() == BK7259_GIMBAL_MOTION_DISABLED);
    assert(!memcmp(controller.charge[0].bytes, charge_on_frame, sizeof(charge_on_frame)));
    assert(bk7259_gimbal_set_charging(false) == 0);
    bk7259_gimbal_settings_t settings = wait_for_settings_save();
    assert(!settings.motion_enabled && !settings.charge_enabled && !settings.save_failed);
    shutdown_and_join();
    assert(controller.motion_count == 0 && controller.prefs_save_calls == 1);

    unsigned prior_charge = controller.charge_count;
    assert(bk7259_gimbal_prepare() == 0);
    wait_for_count(&controller.charge_count, prior_charge + 2);
    assert(motion_count() == 0);
    assert(!memcmp(controller.charge[prior_charge].bytes, charge_off_frame, sizeof(charge_off_frame)));
    assert(bk7259_gimbal_request_motion_enabled(true) == 0);
    wait_for_count(&controller.motion_count, 1);
    settings = wait_for_settings_save();
    assert(settings.motion_enabled && !settings.charge_enabled && !settings.save_failed);
    assert(home_frame(0));
    assert(bk7259_gimbal_set_charging(true) == 0);
    settings = wait_for_settings_save();
    assert(settings.motion_enabled && settings.charge_enabled && !settings.save_failed);
    shutdown_and_join();
    assert(controller.saved_motion_enabled && controller.saved_charge_enabled);
}

static void test_failed_save_keeps_off_and_can_retry(void)
{
    reset_controller();
    controller.prefs_load_fails = true;
    assert(bk7259_gimbal_prepare() == 0);
    wait_for_count(&controller.charge_count, 1);
    bk7259_gimbal_settings_t settings = wait_for_settings_save();
    assert(!settings.motion_enabled && !settings.charge_enabled && settings.save_failed);
    assert(!memcmp(controller.charge[0].bytes, charge_off_frame, sizeof(charge_off_frame)));
    assert(motion_count() == 0);

    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.prefs_save_fails = true;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    assert(bk7259_gimbal_request_motion_enabled(false) == 0);
    settings = wait_for_settings_save();
    assert(!settings.motion_enabled && !settings.charge_enabled && settings.save_failed);
    assert(bk7259_gimbal_center_wait(500) == 0 && motion_count() == 0);

    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.prefs_save_fails = false;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    assert(bk7259_gimbal_request_motion_enabled(false) == 0);
    settings = wait_for_settings_save();
    assert(!settings.motion_enabled && !settings.charge_enabled && !settings.save_failed);
    shutdown_and_join();
    assert(controller.prefs_save_calls == 2 && controller.motion_count == 0);
}

static void test_policy_change_during_save_retains_latest_selection(void)
{
    reset_controller();
    start_and_wait_for_home();
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.hold_prefs_save = true;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);

    assert(bk7259_gimbal_set_charging(false) == 0);
    wait_for_count(&controller.prefs_save_calls, 1);
    assert(bk7259_gimbal_request_motion_enabled(false) == 0);
    bk7259_gimbal_settings_t settings;
    assert(bk7259_gimbal_get_settings(&settings) == 0);
    assert(!settings.motion_enabled && !settings.charge_enabled && settings.pending);

    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.hold_prefs_save = false;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    settings = wait_for_settings_save();
    assert(!settings.motion_enabled && !settings.charge_enabled && !settings.save_failed);
    shutdown_and_join();
    assert(!controller.saved_motion_enabled && !controller.saved_charge_enabled);
    assert(controller.prefs_save_calls == 2);
}

static void test_motion_enable_keeps_only_live_dialogue_phase(void)
{
    reset_controller();
    start_and_wait_for_home();
    assert(bk7259_gimbal_request(BK7259_GIMBAL_SCENE_HAPPY) == 0);
    wait_for_count(&controller.motion_count, 4);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    unsigned before_disable = controller.motion_count;
    controller.drop_next_home = true;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    assert(bk7259_gimbal_request_motion_enabled(false) == 0);
    bk7259_gimbal_settings_t settings = wait_for_settings_save();
    assert(!settings.motion_enabled && settings.charge_enabled);
    unsigned stopped = wait_for_home(before_disable);
    assert(bk7259_gimbal_request_motion_enabled(false) == 0);
    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_LISTENING) ==
           BK7259_GIMBAL_MOTION_DISABLED);
    assert(bk7259_gimbal_update_dialogue(false, BK7259_GIMBAL_PHASE_NEUTRAL) ==
           BK7259_GIMBAL_MOTION_DISABLED);
    assert(bk7259_gimbal_center_wait(500) == 0);
    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_THINKING) ==
           BK7259_GIMBAL_MOTION_DISABLED);
    assert(bk7259_gimbal_request(BK7259_GIMBAL_SCENE_SAD) == BK7259_GIMBAL_MOTION_DISABLED);
    rtos_delay_milliseconds(1200);
    assert(motion_count() == stopped);

    assert(bk7259_gimbal_request_motion_enabled(true) == 0);
    unsigned first_after_home = wait_for_home(stopped);
    assert(first_after_home == stopped + 1);
    unsigned cursor = first_after_home;
    (void)wait_for_target(&cursor, 120, 120);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    for (unsigned i = first_after_home; i < cursor; ++i) {
        assert(motion_pan(i) >= 0 && motion_pitch(i) == 0);
    }
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    shutdown_and_join();
}

static void test_motion_disable_before_ready_and_charge_independence(void)
{
    reset_controller();
    controller.online = false;
    assert(bk7259_gimbal_prepare() == 0);
    assert(wait_for_home(0) == 1);
    assert(bk7259_gimbal_send_next_command() == BK7259_GIMBAL_NOT_READY);
    assert(bk7259_gimbal_request_motion_enabled(false) == 0);
    assert(wait_for_home(1) == 2);

    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.online = true;
    unsigned prior_charge = controller.charge_count;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    assert(bk7259_gimbal_set_charging(false) == 0);
    wait_for_count(&controller.charge_count, prior_charge + 2);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    assert(!memcmp(controller.charge[prior_charge].bytes, charge_off_frame, sizeof(charge_off_frame)));
    assert(!controller.charging && controller.motion_count == 2);
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    shutdown_and_join();
    assert(controller.motion_count == 2);

    prior_charge = controller.charge_count;
    assert(bk7259_gimbal_prepare() == 0);
    wait_for_count(&controller.charge_count, prior_charge + 2);
    assert(bk7259_gimbal_request_motion_enabled(false) == 0);
    assert(bk7259_gimbal_update_dialogue(false, BK7259_GIMBAL_PHASE_NEUTRAL) ==
           BK7259_GIMBAL_MOTION_DISABLED);
    assert(bk7259_gimbal_center_wait(500) == 0 && motion_count() == 2);
    shutdown_and_join();
    assert(controller.motion_count == 2);
}

static void test_fast_motion_reenable_requires_confirmed_home(void)
{
    reset_controller();
    start_and_wait_for_home();
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.tx_paused = true;
    controller.motion_response = RESPONSE_ACCEPT_ONLY;
    unsigned stopped = controller.motion_count;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);

    assert(bk7259_gimbal_request_motion_enabled(false) == 0);
    assert(bk7259_gimbal_request_motion_enabled(true) == 0);
    assert(bk7259_gimbal_send_next_command() == BK7259_GIMBAL_NOT_READY);
    assert(motion_count() == stopped);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.tx_paused = false;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);

    /* Only the ordinary homing path retries after E1/00. Its second frame is
     * still insufficient to open the gate without a matching completion. */
    unsigned first_home = wait_for_home(stopped);
    (void)wait_for_home(first_home);
    assert(bk7259_gimbal_request(BK7259_GIMBAL_SCENE_HAPPY) == BK7259_GIMBAL_NOT_READY);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    complete_locked(0, 0);
    controller.motion_response = RESPONSE_NORMAL;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    rtos_delay_milliseconds(80);
    assert(bk7259_gimbal_send_next_command() == 0);
    shutdown_and_join();
}

static void test_reenable_discards_late_disable_home_completion(void)
{
    reset_controller();
    start_and_wait_for_home();
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.drop_next_home = true;
    controller.motion_response = RESPONSE_ACCEPT_ONLY;
    unsigned before_disable = controller.motion_count;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);

    assert(bk7259_gimbal_request_motion_enabled(false) == 0);
    unsigned after_disable_home = wait_for_home(before_disable);
    assert(after_disable_home == before_disable + 1);
    assert(bk7259_gimbal_request_motion_enabled(true) == 0);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    complete_locked(0, 0);
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    rtos_delay_milliseconds(120);
    assert(motion_count() == after_disable_home);
    assert(bk7259_gimbal_send_next_command() == BK7259_GIMBAL_NOT_READY);

    unsigned after_new_home = wait_for_home(after_disable_home);
    assert(after_new_home == after_disable_home + 1);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    uint32_t interval = controller.motion[after_disable_home].at -
                        controller.motion[before_disable].at;
    assert(interval > HOME_RESPONSE_MS);
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    assert(bk7259_gimbal_send_next_command() == BK7259_GIMBAL_NOT_READY);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    complete_locked(0, 0);
    controller.motion_response = RESPONSE_NORMAL;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    rtos_delay_milliseconds(80);
    assert(bk7259_gimbal_send_next_command() == 0);
    shutdown_and_join();
}

int main(void)
{
    clock_origin = monotonic_ns();
    aosl_ctor();
    test_crc_vectors();
    test_due_motion_waits_for_charge_tail();
    test_ordinary_motion_acknowledgement();
    test_ordinary_motion_ack_timeout();
    test_long_ordinary_move_waits_for_arrival();
    test_takeover_with_same_coordinates();
    test_path_write_failure(false);
    test_path_write_failure(true);
    test_terminal_target_write_failure();
    test_uart_stats_motion_and_reset();
    test_ordinary_ack_attribution_across_reset();
    test_pending_motion_rehomes(false);
    test_pending_motion_rehomes(true);
    test_phase_takeover_uses_pose_at_transmit_slot();
    test_previous_zero_reply_during_hangup();
    test_uart_stats_disable_home_and_tx_error();
    test_uart_fixed_guard_for_charge_and_one_shot_home();
    test_periodic_charge_spacing();
    test_uart_power_stats_and_diagnostic_exclusion();
    test_scheduled_uart_motion_sequence();
    test_limit_sequence();
    test_limit_interruption(false);
    test_limit_interruption(true);
    test_scheduled_uart_motion_interruption();
    test_scheduled_uart_motion_requires_home_ack();
    test_speed_fits_motion_deadline();
    test_listening_nods_use_pitch(0U);
    test_listening_nods_use_pitch(650U);
    test_real_dialogue_interrupts_test_opening();
    test_live_phase_replaces_opening();
    test_thinking_preempts_nod(false);
    test_thinking_preempts_nod(true);
    test_motion_write_pacing();
    test_thinking_endpoint_and_speaking_swing();
    test_boot_wait_and_latest_dialogue();
    test_boot_demo_and_stream_recovery();
    test_scene_skips_missing_replies_and_confirms_home();
    test_four_preset_trajectories();
    test_peer_loss_discards_cli_and_reloads_dialogue();
    test_cli_and_dialogue_share_motion();
    test_explicit_end_interrupts_scene_while_dialogue_inactive();
    test_heartbeat_loss_reestablishes_center();
    test_sustained_invalid_motion_replies_continue_with_charge_ack();
    test_probe_and_charge_policy();
    test_shutdown_unconfirmed_off_stops_renewals();
    test_worker_lifecycle();
    test_independent_settings_survive_reprepare();
    test_failed_save_keeps_off_and_can_retry();
    test_policy_change_during_save_retains_latest_selection();
    test_motion_enable_keeps_only_live_dialogue_phase();
    test_motion_disable_before_ready_and_charge_independence();
    test_fast_motion_reenable_requires_confirmed_home();
    test_reenable_discards_late_disable_home_completion();
    beken_semaphore_t *events[] = {&s_wake, &s_done, &s_centered};
    for (size_t i = 0; i < ARRAY_COUNT(events); ++i) {
        pthread_mutex_destroy(&(*events[i])->mutex);
        free(*events[i]);
        *events[i] = NULL;
    }
    pthread_mutex_destroy(s_lock);
    free(s_lock);
    aosl_dtor();
    assert(aosl_references == 0 && aosl_constructors == aosl_destructors);
    puts("PASS: EB521 boot and recovery, nonblocking motion, UART noise, and charging policy");
    return 0;
}
