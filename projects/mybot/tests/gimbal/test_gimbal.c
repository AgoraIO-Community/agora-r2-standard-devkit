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
    wire_record_t motion[256], charge[96];
    unsigned motion_count, charge_count, boot_replies;
    unsigned stop_attempts, probe_count, dialogue_level;
    motion_response_t motion_response;
    int16_t pan, pitch, target_pan, target_pitch;
    uint32_t completes_at;
    bool online, active, tx_paused;
    bool drop_next_move, drop_next_home;
    bool charging, drop_charge_on_ack, drop_charge_stop;
    uint32_t charge_expires;
    bool saved_motion_enabled, saved_charge_enabled;
    bool prefs_load_fails, prefs_save_fails, hold_prefs_save;
    unsigned prefs_save_calls;
} controller_model_t;

static controller_model_t controller = {.mutex = PTHREAD_MUTEX_INITIALIZER};

static uint64_t clock_origin;
static bool deny_done_wait, deny_thread_create;
static unsigned aosl_references, aosl_constructors, aosl_destructors;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned log_calls;

unsigned bk7259_audio_dialogue_level(void)
{
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    unsigned level = controller.dialogue_level;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    return level;
}

void bk7259_audio_dialogue_set_speaking(bool speaking)
{
    (void)speaking;
}

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
    bool home = pan == 0 && pitch == 0 && pan_speed == 1000 && pitch_speed == 1000;
    if ((pan != 0 || pitch != 0) && controller.drop_next_move) {
        controller.drop_next_move = false;
        return;
    }
    if (home && controller.drop_next_home) {
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
    if (pan == controller.pan && pitch == controller.pitch) {
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
        controller.charge_expires = rtos_get_time() + 3000U;
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

bk_err_t bk_uart_write_bytes(uart_id_t id, const void *data, uint32_t size)
{
    assert(id == UART_ID_2 && size >= 6 && size <= sizeof(controller.motion[0].bytes));
    const uint8_t *frame = data;
    assert(frame[0] == 3 && size == 6U + frame[2] * 256U + frame[3]);
    assert(wire_crc(frame, size - 2) == wire_u16(frame + size - 2));
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller_tick_locked();
    if (frame[1] == 0x66 && size == sizeof(probe_frame) && frame[4] == 2) {
        receive_probe_locked(frame);
        assert(pthread_mutex_unlock(&controller.mutex) == 0);
        return BK_OK;
    }
    wire_record_t *record;
    if (frame[1] == 0x61) {
        assert(size == 14 && controller.motion_count < ARRAY_COUNT(controller.motion));
        record = &controller.motion[controller.motion_count++];
    } else {
        assert(frame[1] == 0x66 && size == 7 && controller.charge_count < ARRAY_COUNT(controller.charge));
        record = &controller.charge[controller.charge_count++];
    }
    memcpy(record->bytes, frame, size);
    record->at = rtos_get_time();
    bk_err_t result = BK_OK;
    if (frame[1] == 0x61) {
        receive_move_locked(frame);
    } else {
        result = receive_charge_locked(frame);
    }
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
    const uint8_t *frame = controller.motion[index].bytes;
    return motion_pan(index) == 0 && motion_pitch(index) == 0 &&
           wire_u16(frame + 8) == 1000 && wire_u16(frame + 10) == 1000;
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
    assert(choose_speed(30, 40) == 358);
    for (unsigned interval = 10; interval <= 160; interval += 10) {
        for (unsigned distance = 1; distance <= 110; ++distance) {
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

static void test_listening_nods_use_pitch(void)
{
    reset_controller();
    start_and_wait_for_home();
    uint32_t started = rtos_get_time();
    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_LISTENING) == 0);
    rtos_delay_milliseconds(850);
    unsigned after_opening = motion_count();
    assert(after_opening > 1);
    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_LISTENING) == 0);
    rtos_delay_milliseconds(1800);
    assert(motion_count() == after_opening);
    wait_for_count(&controller.motion_count, after_opening + 1);
    rtos_delay_milliseconds(1250);
    unsigned after_nods = motion_count();
    assert(bk7259_gimbal_update_dialogue(false, BK7259_GIMBAL_PHASE_NEUTRAL) == 0);
    assert(bk7259_gimbal_center_wait(3000) == 0);
    shutdown_and_join();

    uint32_t nod_started = controller.motion[after_opening].at - started;
    assert(nod_started >= 3450 && nod_started <= 3800);
    int prior_sign = 0, previous_pitch = 0, peak = 0;
    unsigned positive_lobes = 0, negative_lobes = 0;
    for (unsigned i = after_opening; i < after_nods; ++i) {
        int pitch = motion_pitch(i);
        assert(motion_pan(i) == 0 && abs(pitch) <= 30);
        assert(abs(pitch - previous_pitch) <= 24);
        previous_pitch = pitch;
        if (abs(pitch) > peak) {
            peak = abs(pitch);
        }
        int sign = pitch >= 15 ? 1 : pitch <= -15 ? -1 : 0;
        if (sign && sign != prior_sign) {
            if (sign > 0) {
                ++positive_lobes;
            } else {
                ++negative_lobes;
            }
            prior_sign = sign;
        }
    }
    assert(peak >= 27 && positive_lobes == 2 && negative_lobes == 2);
    assert(controller.pan == 0 && controller.pitch == 0);
}

static int peak_pan(unsigned begin, unsigned end)
{
    int peak = 0;
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    for (unsigned i = begin; i < end; ++i) {
        int pan = abs(motion_pan(i));
        if (pan > peak) {
            peak = pan;
        }
    }
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    return peak;
}

static void set_dialogue_level(unsigned level)
{
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.dialogue_level = level;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
}

static void test_thinking_and_speaking_are_eased(void)
{
    reset_controller();
    start_and_wait_for_home();
    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_NEUTRAL) == 0);
    rtos_delay_milliseconds(900);
    unsigned cursor = motion_count();
    uint32_t thinking_at = rtos_get_time();
    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_THINKING) == 0);
    uint32_t peak_at = wait_for_target(&cursor, 150, 150);
    assert(peak_at - thinking_at >= 600 && peak_at - thinking_at <= 1050);
    unsigned held = motion_count();
    rtos_delay_milliseconds(1400);
    assert(motion_count() == held);
    uint32_t processing_at = wait_for_target(&cursor, -30, 30);
    assert(processing_at - thinking_at >= 3000 && processing_at - thinking_at <= 3950);
    rtos_delay_milliseconds(650);
    assert(peak_pan(cursor, motion_count()) <= 30);

    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_SPEAKING) == 0);
    rtos_delay_milliseconds(700);
    unsigned quiet = motion_count();
    rtos_delay_milliseconds(400);
    assert(motion_count() == quiet);
    set_dialogue_level(100);
    rtos_delay_milliseconds(200);
    unsigned rising = motion_count();
    rtos_delay_milliseconds(1400);
    unsigned loud = motion_count();
    int rising_peak = peak_pan(quiet, rising);
    int loud_peak = peak_pan(rising, loud);
    assert(rising_peak > 0 && rising_peak < loud_peak && loud_peak >= 45 && loud_peak <= 60);
    set_dialogue_level(0);
    rtos_delay_milliseconds(900);
    unsigned settled = motion_count();
    rtos_delay_milliseconds(500);
    assert(motion_count() == settled);
    assert(bk7259_gimbal_update_dialogue(false, BK7259_GIMBAL_PHASE_NEUTRAL) == 0);
    assert(bk7259_gimbal_center_wait(3000) == 0);
    shutdown_and_join();
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
    assert(controller.rx_bytes == 0 && controller.charge_count >= 33);
    for (unsigned i = 0; i < 34; ++i) {
        assert(home_frame(i));
        if (i > 0) {
            uint32_t interval = controller.motion[i].at - controller.motion[i - 1].at;
            assert(interval >= 980 && interval <= 1200);
        }
    }
    for (unsigned i = 0; i < controller.charge_count; ++i) {
        assert(!memcmp(controller.charge[i].bytes, charge_on_frame, sizeof(charge_on_frame)));
        if (i > 0) {
            uint32_t interval = controller.charge[i].at - controller.charge[i - 1].at;
            assert(interval >= 980 && interval <= 1200);
        }
    }
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
    (void)wait_for_target(&cursor, 150, 150);
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

    assert(pthread_mutex_lock(&controller.mutex) == 0);
    deliver_reply_locked(already_at_target_frame, 3);
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    rtos_delay_milliseconds(30);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.motion_response = RESPONSE_NORMAL;
    deliver_reply_locked(already_at_target_frame + 3, sizeof(already_at_target_frame) - 3);
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
    assert(controller.motion[2].at - controller.motion[1].at < 200);
    assert(controller.motion[3].at - controller.motion[2].at < 200);
    assert(pthread_mutex_unlock(&controller.mutex) == 0);

    unsigned missed_home = wait_for_home(1);
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
            assert(abs(motion_pan(1)) <= 12);
            break;
        case BK7259_GIMBAL_SCENE_SAD:
            assert(pan_min == 0 && pan_max == 0 && pitch_max >= 94 && pitch_max <= 100);
            break;
        case BK7259_GIMBAL_SCENE_ANGRY:
            assert(pan_min >= -80 && pan_min <= -74 && pan_max >= 94 && pan_max <= 100);
            break;
        case BK7259_GIMBAL_SCENE_SURPRISED:
            assert(pan_min == 0 && pan_max >= 290 && pan_max <= 300);
            break;
        }
        assert(abs(motion_pan(returned - 2)) <= 15 && abs(motion_pitch(returned - 2)) <= 15);
        if (scene == BK7259_GIMBAL_SCENE_ANGRY || scene == BK7259_GIMBAL_SCENE_SURPRISED) {
            int plateau = scene == BK7259_GIMBAL_SCENE_ANGRY ? 95 : 240;
            uint32_t return_floor = scene == BK7259_GIMBAL_SCENE_ANGRY ? 260U : 420U;
            unsigned last_plateau = 0;
            for (unsigned i = 1; i + 1 < returned; ++i) {
                if (motion_pan(i) >= plateau) {
                    last_plateau = i;
                }
            }
            assert(last_plateau > 0 &&
                   controller.motion[returned - 2].at - controller.motion[last_plateau].at >= return_floor);
        }
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
    controller.dialogue_level = 100;
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
    rtos_delay_milliseconds(700);
    unsigned before_scene = motion_count();
    assert(bk7259_gimbal_request(BK7259_GIMBAL_SCENE_SAD) == 0);
    wait_for_count(&controller.motion_count, before_scene + 1);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    assert(motion_pitch(before_scene) > 0);
    assert(pthread_mutex_unlock(&controller.mutex) == 0);

    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_THINKING) == 0);
    unsigned cursor = before_scene + 1;
    (void)wait_for_target(&cursor, 150, 150);
    assert(bk7259_gimbal_send_next_command() == 0);
    (void)wait_for_target(&cursor, -300, -300);
    /* A new request for the same dialogue phase supersedes the CLI target. */
    assert(bk7259_gimbal_update_dialogue(true, BK7259_GIMBAL_PHASE_THINKING) == 0);
    unsigned first_bridge = cursor;
    (void)wait_for_target(&cursor, 150, 150);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    assert(motion_pan(first_bridge) > -300 && motion_pan(first_bridge) <= -200);
    for (unsigned i = first_bridge; i < cursor; ++i) {
        assert(motion_pitch(i) == 0 && abs(motion_pan(i) - motion_pan(i - 1)) <= 130);
    }
    assert(controller.motion[cursor - 1].at - controller.motion[first_bridge].at >= 240);
    assert(pthread_mutex_unlock(&controller.mutex) == 0);

    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.dialogue_level = 100;
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
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    assert(motion_pitch(2) > 0);
    controller.drop_next_home = true;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);

    assert(bk7259_gimbal_update_dialogue(false, BK7259_GIMBAL_PHASE_NEUTRAL) == 0);
    unsigned after_end = motion_count();
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
    wait_for_count(&controller.charge_count, current_charge + 3);
    unsigned lost_home = wait_for_home(before);
    assert(lost_home > before);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.drop_charge_on_ack = false;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    assert(bk7259_gimbal_center_wait(3000) == 0);
    shutdown_and_join();
}

static void test_sustained_invalid_motion_replies_close_gate(void)
{
    reset_controller();
    start_and_wait_for_home();
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    controller.motion_response = RESPONSE_CONTINUOUS_NOISE;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
    assert(bk7259_gimbal_request(BK7259_GIMBAL_SCENE_HAPPY) == 0);
    wait_for_count(&controller.motion_count, 3);
    unsigned lost_home = wait_for_home(1);
    assert(pthread_mutex_lock(&controller.mutex) == 0);
    uint32_t elapsed = controller.motion[lost_home - 1].at - controller.motion[1].at;
    assert(elapsed >= 900 && elapsed <= 1600 && controller.charging);
    assert(controller.rx_bytes > 600);
    assert(controller.charge_count > 1);
    assert(controller.charge[1].at > controller.motion[1].at &&
           controller.charge[1].at < controller.motion[lost_home - 1].at);
    controller.next_noise_at = 0;
    controller.motion_response = RESPONSE_NORMAL;
    assert(pthread_mutex_unlock(&controller.mutex) == 0);
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
    (void)wait_for_target(&cursor, 150, 150);
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
    test_speed_fits_motion_deadline();
    test_listening_nods_use_pitch();
    test_thinking_and_speaking_are_eased();
    test_boot_wait_and_latest_dialogue();
    test_boot_demo_and_stream_recovery();
    test_scene_skips_missing_replies_and_confirms_home();
    test_four_preset_trajectories();
    test_peer_loss_discards_cli_and_reloads_dialogue();
    test_cli_and_dialogue_share_motion();
    test_explicit_end_interrupts_scene_while_dialogue_inactive();
    test_heartbeat_loss_reestablishes_center();
    test_sustained_invalid_motion_replies_close_gate();
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
