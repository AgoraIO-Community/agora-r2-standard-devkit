/* SPDX-License-Identifier: Apache-2.0 */
/* Exercise the production parser, motion owner and mailbox without vendor HAL.
 * The controller model quantizes angles and reports acceptance separately from
 * movement completion. It does not copy the production trajectories. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../../../../components/mybot/mybot_sdk/platforms/bk7259/bk7259_gimbal.c"

#define HOST_CLOCK_SCALE 10U
#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

struct host_semaphore { pthread_mutex_t mutex; int count, maximum; };
struct host_thread { pthread_t id; void (*function)(void *); void *argument; };
typedef struct { int16_t pan, pitch; uint16_t pan_speed, pitch_speed; uint32_t at; } move_record_t;
typedef enum { TEST_GOOD, TEST_DROP, TEST_REJECT, TEST_BAD_CRC,
               TEST_WRONG_COMMAND, TEST_WRONG_LENGTH, TEST_FRAGMENTED, TEST_TX_FAIL } manual_reply_t;
typedef struct {
    uint8_t bytes[200];
    size_t size;
    uint32_t at;
} delayed_reply_t;
static const uint8_t expected_manual_frames[][14] = {
    {3,0x61,0,8,0xd4,0xfe,0,0,0xf4,1,0xf4,1,0x0a,0xab},
    {3,0x61,0,8,0,0,0x64,0,0xf4,1,0xf4,1,0x11,0xef},
    {3,0x61,0,8,0,0,0x38,0xff,0xf4,1,0xf4,1,0x09,0x67},
    {3,0x62,0,6,0,0,0x64,0,0xf4,1,0xcf,0x5a},
};
typedef struct {
    pthread_mutex_t mutex;
    bool manual_wire, drop_startup_query, hold_receive, rx_waiting;
    uint8_t fifo[4096];
    size_t fifo_size;
    delayed_reply_t delayed[8];
    uint32_t acceptance_delay_ms, status_delay_ms;
    bool drop_acceptance;
    bool active, complete_reply, drop_reply, hold_motion, early_complete;
    int reject;
    int16_t pan, pitch, target_pan, target_pitch;
    uint32_t completes_at, extra_motion_ms;
    unsigned motion_count, query_count, busy_queries, stop_count;
    move_record_t moves[2048];
    void (*move_hook)(void);
    uint8_t flow;
    bool uart_open, tx_mapped, rx_mapped, hook_running;
    bool corrupt_status, status_busy;
    unsigned command_count, command_indices[16];
    uint32_t command_at[16];
    manual_reply_t command_behavior;
    unsigned command_fail_at;
    bool command_pending_ack;
    uint8_t command_staged[200];
    size_t command_staged_size;
    uint32_t command_staged_at;
} controller_model_t;
static controller_model_t controller = {.mutex = PTHREAD_MUTEX_INITIALIZER};
static pthread_mutex_t observations_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned started[BK7259_GIMBAL_SCENE_COUNT], completed[BK7259_GIMBAL_SCENE_COUNT];
static uint64_t clock_origin;
static unsigned groups;
static bool deny_done_wait, deny_thread_create, deny_uart_deinit;
static unsigned aosl_references, aosl_constructors, aosl_destructors;
typedef struct { pthread_t id; void (*function)(void); } hook_task_t;
static hook_task_t *hook_task;
static void *hook_entry(void *argument)
{
    hook_task_t *task = argument;
    task->function();
    pthread_mutex_lock(&controller.mutex);
    controller.hook_running = false;
    pthread_mutex_unlock(&controller.mutex);
    return NULL;
}
static void join_hook(void)
{
    if (hook_task) {
        assert(pthread_join(hook_task->id, NULL) == 0);
        free(hook_task); hook_task = NULL;
    }
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
    bk_err_t ret = rtos_set_semaphore(&s_rx);
    assert(ret == BK_OK || ret == kTimeoutErr);
}
static void reply_locked(uint8_t command, const uint8_t *payload, size_t size)
{
    const bool acceptance = command == 0xe1 && size == 1 && payload[0] == 0;
    if (acceptance && controller.drop_acceptance) return;
    uint8_t frame[200];
    size_t bytes = make_reply(frame, command, payload, size);
    if (command == 0xe3 && controller.corrupt_status) frame[bytes - 1] ^= 1;
    uint32_t delay = acceptance ? controller.acceptance_delay_ms :
                     command == 0xe3 ? controller.status_delay_ms : 0;
    if (delay) {
        for (size_t index = 0; index < ARRAY_COUNT(controller.delayed); ++index) {
            delayed_reply_t *pending = &controller.delayed[index];
            if (pending->size) continue;
            memcpy(pending->bytes, frame, bytes);
            pending->size = bytes;
            pending->at = rtos_get_time() + delay;
            return;
        }
        assert(!"delayed response queue full");
    }
    deliver_reply_locked(frame, bytes);
}
static void controller_tick_locked(void)
{
    for (size_t index = 0; index < ARRAY_COUNT(controller.delayed); ++index) {
        delayed_reply_t *pending = &controller.delayed[index];
        if (pending->size && (int32_t)(rtos_get_time() - pending->at) >= 0) {
            deliver_reply_locked(pending->bytes, pending->size);
            pending->size = 0;
        }
    }
    if (controller.command_staged_size &&
        (int32_t)(rtos_get_time() - controller.command_staged_at) >= 0) {
        assert(controller.fifo_size + controller.command_staged_size <= sizeof(controller.fifo));
        memcpy(controller.fifo + controller.fifo_size, controller.command_staged, controller.command_staged_size);
        controller.fifo_size += controller.command_staged_size;
        controller.command_staged_size = 0;
        bk_err_t ret = rtos_set_semaphore(&s_rx);
        assert(ret == BK_OK || ret == kTimeoutErr);
    }
    if (!controller.active || controller.hold_motion || controller.hook_running ||
        (int32_t)(rtos_get_time() - controller.completes_at) < 0)
        return;
    controller.active = false;
    controller.pan = controller.target_pan;
    controller.pitch = controller.target_pitch;
    if (controller.complete_reply) {
        uint8_t payload[] = {6, (uint8_t)controller.pan, (uint8_t)(controller.pan >> 8),
                            (uint8_t)controller.pitch, (uint8_t)(controller.pitch >> 8)};
        reply_locked(0xe1, payload, sizeof(payload));
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
    if (semaphore == &s_rx && timeout > 0) {
        bool hold;
        do {
            pthread_mutex_lock(&controller.mutex);
            hold = controller.hold_receive;
            if (hold) controller.rx_waiting = true;
            pthread_mutex_unlock(&controller.mutex);
            if (hold) {
                rtos_lock_mutex(&s_lock); bool ending = s_gimbal.stopping; unlock();
                if (ending) break;
                host_sleep_us(100);
            }
        } while (hold);
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
    return BK_OK;
}
bk_err_t bk_uart_deinit(uart_id_t id)
{
    assert(id == UART_ID_2);
    if (deny_uart_deinit) { deny_uart_deinit = false; return -3; }
    controller.uart_open = false;
    return BK_OK;
}
bk_err_t bk_uart_register_rx_isr(uart_id_t id, void (*isr)(uart_id_t, void *), void *arg)
{ assert(id == UART_ID_2); return BK_OK; }
bk_err_t bk_uart_enable_rx_interrupt(uart_id_t id)
{ assert(id == UART_ID_2); return BK_OK; }
bool bk_uart_is_tx_over(uart_id_t id)
{ assert(id == UART_ID_2); return true; }
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
static int16_t wire_i16(const uint8_t *data)
{ return (int16_t)((unsigned)data[0] + 256U * data[1]); }
static uint32_t controller_axis_ms(int16_t start, int16_t target, uint16_t speed)
{
    double phases = fabs((double)target - start) / 3.15;
    return (uint32_t)ceil(phases) * (1000U / speed);
}
bk_err_t bk_uart_write_bytes(uart_id_t id, const void *data, uint32_t size)
{
    assert(id == UART_ID_2);
    const uint8_t *frame = data;
    assert(size >= 6 && frame[0] == 3 && size == 6U + frame[2] * 256U + frame[3]);
    assert(wire_crc(frame, size - 2) == (uint16_t)(frame[size - 2] | frame[size - 1] << 8));
    pthread_mutex_lock(&controller.mutex);
    if (controller.manual_wire && frame[1] != 0x63) {
    assert(controller.command_count < 16);
    if (controller.command_pending_ack)
        assert(rtos_get_time() - controller.command_at[controller.command_count - 1] >= 1000);
    controller.command_pending_ack = false;
    unsigned index = 0;
    for (; index < 4; ++index) {
        size_t length = index == 3 ? 12 : 14;
        if (size == length && !memcmp(frame, expected_manual_frames[index], length)) break;
    }
    assert(index < 4); /* Forbids query, stop, center and retransmit variants. */
    unsigned number = ++controller.command_count;
    controller.command_indices[number - 1] = index;
    controller.command_at[number - 1] = rtos_get_time();
    manual_reply_t behavior = controller.command_fail_at && controller.command_fail_at != number ?
                            TEST_GOOD : controller.command_behavior;
    if (behavior == TEST_TX_FAIL) { pthread_mutex_unlock(&controller.mutex); return -3; }
    controller.command_pending_ack = true;
    if (behavior != TEST_DROP) {
        uint8_t response[200], code = behavior == TEST_REJECT ? 2 : 0;
        uint8_t command = behavior == TEST_WRONG_COMMAND ? (uint8_t)((frame[1] | 0x80U) ^ 3U) :
                                                         (uint8_t)(frame[1] | 0x80U);
        uint8_t payload[] = {code, 0};
        size_t length = make_reply(response, command, payload, behavior == TEST_WRONG_LENGTH ? 2 : 1);
        if (behavior == TEST_BAD_CRC) response[length - 1] ^= 1;
        if (behavior == TEST_FRAGMENTED) {
            memcpy(controller.fifo, response, 3); controller.fifo_size = 3;
            memcpy(controller.command_staged, response + 3, length - 3);
            controller.command_staged_size = length - 3;
            controller.command_staged_at = rtos_get_time() + 30;
            bk_err_t ret = rtos_set_semaphore(&s_rx);
            assert(ret == BK_OK || ret == kTimeoutErr);
        } else {
            memcpy(controller.fifo, response, length); controller.fifo_size = length;
            bk_err_t ret = rtos_set_semaphore(&s_rx);
            assert(ret == BK_OK || ret == kTimeoutErr);
        }
    }
    pthread_mutex_unlock(&controller.mutex);
    return BK_OK;
    }
    void (*hook)(void) = NULL;
    controller_tick_locked();
    if (frame[1] == 0x61) {
        assert(size == 14 && !controller.active);
        int16_t pan = wire_i16(frame + 4), pitch = wire_i16(frame + 6);
        uint16_t pan_speed = (uint16_t)wire_i16(frame + 8), pitch_speed = (uint16_t)wire_i16(frame + 10);
        assert(pan >= -800 && pan <= 800 && pitch >= -200 && pitch <= 100);
        assert(pan_speed >= 1 && pan_speed <= 1000 && pitch_speed >= 1 && pitch_speed <= 1000);
        assert(controller.motion_count < ARRAY_COUNT(controller.moves));
        controller.moves[controller.motion_count++] = (move_record_t){pan, pitch, pan_speed, pitch_speed, rtos_get_time()};
        hook = controller.move_hook; controller.move_hook = NULL;
        if (controller.early_complete) {
            uint8_t payload[] = {6, (uint8_t)pan, (uint8_t)(pan >> 8),
                                (uint8_t)pitch, (uint8_t)(pitch >> 8)};
            reply_locked(0xe1, payload, sizeof(payload));
        }
        if (!controller.drop_reply) {
            uint8_t result = (uint8_t)controller.reject;
            reply_locked(0xe1, &result, 1);
            if (!result) {
                controller.target_pan = (int16_t)lround(lround(pan / 3.15) * 3.15);
                controller.target_pitch = (int16_t)lround(lround(pitch / 3.15) * 3.15);
                uint32_t pan_ms = controller_axis_ms(controller.pan, pan, pan_speed);
                uint32_t pitch_ms = controller_axis_ms(controller.pitch, pitch, pitch_speed);
                controller.completes_at = rtos_get_time() + (pan_ms > pitch_ms ? pan_ms : pitch_ms) + controller.extra_motion_ms;
                controller.active = true;
            }
        }
    } else if (frame[1] == 0x63) {
        static const uint8_t query[] = {3, 0x63, 0, 0, 0xf0, 0x7e};
        assert(size == sizeof(query) && !memcmp(frame, query, sizeof(query)));
        ++controller.query_count;
        if (controller.drop_startup_query) {
            pthread_mutex_unlock(&controller.mutex);
            return BK_OK;
        }
        if (controller.active) ++controller.busy_queries;
        uint8_t payload[12] = {0, controller.flow, (uint8_t)controller.pan, (uint8_t)(controller.pan >> 8),
                              0, 0, controller.active || controller.status_busy ? 1 : 0,
                              (uint8_t)controller.pitch, (uint8_t)(controller.pitch >> 8),
                              0, 0, controller.active || controller.status_busy ? 1 : 0};
        reply_locked(0xe3, payload, sizeof(payload));
    } else if (frame[1] == 0x64) {
        assert(size == 8 && frame[4] == 3 && frame[5] == 1);
        ++controller.stop_count;
        uint8_t result = 0;
        reply_locked(0xe4, &result, 1);
    } else assert(!"unexpected movement command");
    if (hook) controller.hook_running = true;
    pthread_mutex_unlock(&controller.mutex);
    if (hook) {
        assert(!hook_task);
        hook_task = malloc(sizeof(*hook_task)); assert(hook_task);
        hook_task->function = hook;
        assert(pthread_create(&hook_task->id, NULL, hook_entry, hook_task) == 0);
    }
    return BK_OK;
}
int bk_uart_read_bytes(uart_id_t id, void *data, uint32_t size, uint32_t timeout)
{
    assert(id == UART_ID_2 && timeout == 0);
    pthread_mutex_lock(&controller.mutex);
    size_t count = controller.fifo_size < size ? controller.fifo_size : size;
    memcpy(data, controller.fifo, count); controller.fifo_size -= count;
    memmove(controller.fifo, controller.fifo + count, controller.fifo_size);
    if (count && !controller.fifo_size && !controller.command_staged_size)
        controller.command_pending_ack = false;
    pthread_mutex_unlock(&controller.mutex);
    return count ? (int)count : BK_ERR_UART_RX_TIMEOUT;
}
void aosl_log(int level, const char *format, ...)
{
    char message[512];
    va_list args; va_start(args, format);
    vsnprintf(message, sizeof(message), format, args); va_end(args);
    if (getenv("GIMBAL_TEST_LOGS")) fputs(message, stderr);
    pthread_mutex_lock(&observations_lock);
    for (int scene = 0; scene < BK7259_GIMBAL_SCENE_COUNT; ++scene) {
        char prefix[100];
        snprintf(prefix, sizeof(prefix), "scene started: %s", scene_name((bk7259_gimbal_scene_t)scene));
        if (strstr(message, prefix)) ++started[scene];
        snprintf(prefix, sizeof(prefix), "scene completed and centered: %s", scene_name((bk7259_gimbal_scene_t)scene));
        if (strstr(message, prefix)) ++completed[scene];
    }
    pthread_mutex_unlock(&observations_lock);
}
static void reset_controller(void)
{
    join_hook();
    pthread_mutex_lock(&controller.mutex);
    memset(controller.fifo, 0, sizeof(controller) - offsetof(controller_model_t, fifo));
    controller.complete_reply = true; controller.flow = 2;
    controller.manual_wire = controller.drop_startup_query = controller.hold_receive = controller.rx_waiting = false;
    pthread_mutex_unlock(&controller.mutex);
    memset(&s_gimbal, 0, sizeof(s_gimbal));
    while (rtos_get_semaphore(&s_rx, 0) == BK_OK) { }
    while (rtos_get_semaphore(&s_wake, 0) == BK_OK) { }
    while (rtos_get_semaphore(&s_done, 0) == BK_OK) { }
    while (rtos_get_semaphore(&s_centered, 0) == BK_OK) { }
    pthread_mutex_lock(&observations_lock);
    memset(started, 0, sizeof(started)); memset(completed, 0, sizeof(completed));
    pthread_mutex_unlock(&observations_lock);
}

static void test_crc_and_stream(void)
{
    static const uint8_t query[] = {3,0x63,0,0,0xf0,0x7e};
    static const uint8_t center_frame[] = {3,0x61,0,8,0,0,0,0,0xf4,1,0xf4,1,0x19,0xcb};
    static const uint8_t right[] = {3,0x61,0,8,0x2c,1,0,0,0xf4,1,0xf4,1,0x0b,0x46};
    static const uint8_t accepted[] = {3,0xe1,0,1,0,0x57,0xac};
    assert(crc16(query, 4) == 0x7ef0 && crc16(center_frame, 12) == 0xcb19 &&
           crc16(right, 12) == 0x460b && crc16(accepted, 5) == 0xac57);
    gimbal_io_t io = {0}; uint8_t frame[200];
    memcpy(io.rx, accepted, 3); io.used = 3;
    assert(extract_frame(&io, frame) == 0 && io.used == 3);
    memcpy(io.rx + io.used, accepted + 3, 4); io.used += 4;
    assert(extract_frame(&io, frame) == 7 && !memcmp(frame, accepted, 7) && io.used == 0);
    /* Partial false header, impossible length, corrupt frame, then two glued valid frames. */
    const uint8_t noise[] = {0x99,3,0xe1,0,100,0x77,3,0xe3,0xff,0xff};
    memcpy(io.rx, noise, sizeof(noise)); io.used = sizeof(noise);
    memcpy(io.rx + io.used, accepted, 7); io.rx[io.used + 5] ^= 1; io.used += 7;
    memcpy(io.rx + io.used, accepted, 7); io.used += 7;
    memcpy(io.rx + io.used, accepted, 7); io.used += 7;
    assert(extract_frame(&io, frame) == 7 && io.bad_frames >= 2 && io.used == 7);
    assert(extract_frame(&io, frame) == 7 && io.used == 0);
    memset(io.rx, 0xff, sizeof(io.rx)); io.used = sizeof(io.rx);
    assert(extract_frame(&io, frame) == 0 && io.used == sizeof(io.rx) - 1);
    ++groups;
}

static void test_completion_and_failures(void)
{
    reset_controller(); gimbal_io_t io = {0};
    uint32_t began = rtos_get_time();
    controller.extra_motion_ms = 40;
    assert(move_to(&io, 300, 0, 380) == 0);
    assert(rtos_get_time() - began >= 40 && near_target(io.pan, 300));
    assert(!controller.active && controller.motion_count == 1 && controller.busy_queries > 0);
    /* A device that only sends 00 must finish through E3 stopped-and-at-target. */
    reset_controller(); memset(&io, 0, sizeof(io)); controller.complete_reply = false;
    assert(move_to(&io, 250, 0, 200) == 0 && io.query_completion_logged && controller.query_count > 0);
    /* A cached E3 and an already-arrived E3 must not satisfy a fresh query. */
    const uint8_t stale_payload[] = {0, 2, 0x2c, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    io.used = make_reply(io.rx, 0xe3, stale_payload, sizeof(stale_payload));
    pthread_mutex_lock(&controller.mutex);
    controller.pan = 0;
    reply_locked(0xe3, stale_payload, sizeof(stale_payload));
    pthread_mutex_unlock(&controller.mutex);
    assert(query_status(&io) == 0 && io.pan == 0);
    /* A delayed 06 preceding this transaction's 00 cannot advance motion. */
    reset_controller(); memset(&io, 0, sizeof(io)); controller.early_complete = true;
    controller.extra_motion_ms = 40; began = rtos_get_time();
    assert(move_to(&io, 300, 0, 380) == 0 && !controller.active && controller.busy_queries > 0);
    assert(rtos_get_time() - began >= 40);
    reset_controller(); memset(&io, 0, sizeof(io));
    controller.early_complete = true; controller.drop_reply = true;
    assert(move_to(&io, 300, 0, 380) < 0 && controller.motion_count == 1);
    io.flow = 2; io.pan = 252; io.pitch = -2; io.pan_flags = io.pitch_flags = 0;
    assert(at_target(&io, 250, 0));
    io.pan_flags = 1; assert(!at_target(&io, 250, 0));
    io.pan_flags = 0; io.pitch_flags = 1; assert(!at_target(&io, 250, 0));
    io.pitch_flags = 0; io.flow = 1; assert(!at_target(&io, 250, 0));
    io.flow = 2; io.pan = 255; assert(!at_target(&io, 250, 0));
    reset_controller(); memset(&io, 0, sizeof(io));
    assert(move_to(&io, 801, 0, 20) < 0 && move_to(&io, 0, 101, 20) < 0 &&
           move_to(&io, 0, -201, 20) < 0 && controller.motion_count == 0);
    controller.drop_reply = true;
    assert(move_to(&io, 300, 0, 380) < 0 && controller.motion_count == 1);
    reset_controller(); memset(&io, 0, sizeof(io)); controller.reject = 2;
    assert(move_to(&io, 300, 0, 380) < 0 && controller.motion_count == 1 && controller.query_count == 0);
    reset_controller(); memset(&io, 0, sizeof(io)); controller.reject = 3;
    assert(move_to(&io, 1, 0, 20) == 0 && io.pan == 0 && controller.motion_count == 1);
    reset_controller(); memset(&io, 0, sizeof(io)); controller.hold_motion = true;
    assert(move_to(&io, 300, 0, 380) < 0 && controller.motion_count == 1 && controller.busy_queries > 0);

    /* Real controllers may answer after several receive slices. E3 alone
     * must have its full response deadline rather than fail at 250 ms. */
    reset_controller(); memset(&io, 0, sizeof(io)); controller.status_delay_ms = 300;
    began = rtos_get_time();
    assert(query_status(&io) == 0 && io.flow == 2 && controller.query_count == 1);
    assert(rtos_get_time() - began >= 300 && rtos_get_time() - began < 1500);

    /* Keep acceptance and completion independent, delaying both the E1 ACK
     * and each E3 by 300 ms. An intermediate receive timeout is not a fault. */
    reset_controller(); memset(&io, 0, sizeof(io));
    controller.acceptance_delay_ms = controller.status_delay_ms = 300;
    controller.complete_reply = false;
    began = rtos_get_time();
    assert(move_to(&io, 300, 0, 380) == 0 && !io.motion_pending);
    assert(rtos_get_time() - began >= 600 && rtos_get_time() - began < 2000);
    assert(controller.motion_count == 1 && controller.query_count > 0 && !controller.active);
    assert(near_target(io.pan, 300));

    /* Lost acceptance does not mean the command failed to execute. Even a
     * subsequent physical completion must not acknowledge that transaction,
     * and the driver must time out once without retransmitting the motion. */
    reset_controller(); memset(&io, 0, sizeof(io)); controller.drop_acceptance = true;
    began = rtos_get_time();
    assert(move_to(&io, 300, 0, 380) < 0);
    assert(rtos_get_time() - began >= 1000 && rtos_get_time() - began < 1500);
    assert(controller.motion_count == 1 && controller.query_count == 0);
    assert(!controller.active && near_target(controller.pan, 300));
    ++groups;
}

static void test_presets_and_serial_execution(void)
{
    for (int scene = 0; scene < BK7259_GIMBAL_SCENE_COUNT; ++scene) {
        uint32_t time = 0;
        unsigned samples = 0, turns = 0;
        while (time < scene_duration((bk7259_gimbal_scene_t)scene)) {
            uint32_t next = next_sample((bk7259_gimbal_scene_t)scene, time);
            assert(next >= time + 40U && next <= scene_duration((bk7259_gimbal_scene_t)scene));
            if (scene == BK7259_GIMBAL_SCENE_ANGRY) {
                if (next == 180) turns |= 1U;
                if (next == 360) turns |= 2U;
                if (next == 550) turns |= 4U;
            }
            if (scene == BK7259_GIMBAL_SCENE_SURPRISED) {
                if (next == 380) turns |= 1U;
                if (next == 500) turns |= 2U;
            }
            time = next;
            ++samples;
        }
        assert(time == scene_duration((bk7259_gimbal_scene_t)scene));
        if (scene == BK7259_GIMBAL_SCENE_HAPPY) assert(samples == 50);
        if (scene == BK7259_GIMBAL_SCENE_SAD) assert(samples == 60);
        if (scene == BK7259_GIMBAL_SCENE_ANGRY) assert(turns == 7U);
        if (scene == BK7259_GIMBAL_SCENE_SURPRISED) assert(turns == 3U);
        for (uint32_t ms = 0; ms <= scene_duration((bk7259_gimbal_scene_t)scene); ++ms) {
            int16_t pan, pitch;
            scene_sample((bk7259_gimbal_scene_t)scene, ms, &pan, &pitch);
            assert(pan >= -800 && pan <= 800 && pitch >= -200 && pitch <= 100);
            if (scene == BK7259_GIMBAL_SCENE_HAPPY) assert(abs(pan) <= 60 && pitch == 0);
            if (scene == BK7259_GIMBAL_SCENE_SAD) assert(pan == 0 && pitch >= 0);
        }
    }
    int16_t pan, pitch;
    scene_sample(BK7259_GIMBAL_SCENE_SAD, 3000, &pan, &pitch); assert(pitch == 100);
    scene_sample(BK7259_GIMBAL_SCENE_ANGRY, 180, &pan, &pitch); assert(pan == 80);
    scene_sample(BK7259_GIMBAL_SCENE_ANGRY, 360, &pan, &pitch); assert(pan == -80);
    scene_sample(BK7259_GIMBAL_SCENE_ANGRY, 550, &pan, &pitch); assert(pan == 100);
    scene_sample(BK7259_GIMBAL_SCENE_SURPRISED, 380, &pan, &pitch); assert(pan == 300);
    scene_sample(BK7259_GIMBAL_SCENE_SURPRISED, 500, &pan, &pitch); assert(pan == 250);
    for (int scene = 0; scene < BK7259_GIMBAL_SCENE_COUNT; ++scene) {
        reset_controller(); s_gimbal.revision = 7;
        gimbal_io_t io = {.flow = 2};
        int result = play_scene(&io, (bk7259_gimbal_scene_t)scene, 7);
        if (result) fprintf(stderr, "scene=%s result=%d actual=%d,%d moves=%u\n",
                            scene_name((bk7259_gimbal_scene_t)scene), result, io.pan, io.pitch, controller.motion_count);
        assert(result == 0);
        assert(near_target(io.pan, 0) && near_target(io.pitch, 0) && !controller.active);
        assert(controller.motion_count > 10 && controller.motion_count < 200);
    }
    ++groups;
}

static void replace_same_scene(void) { assert(bk7259_gimbal_request(BK7259_GIMBAL_SCENE_SURPRISED) == 0); }
static void hang_up(void) { assert(bk7259_gimbal_set_conversation(false) == 0); }
static void test_segment_cancellation(void)
{
    reset_controller(); s_gimbal.prepared = s_gimbal.accepting = s_gimbal.conversation = true;
    s_gimbal.revision = 4; controller.move_hook = replace_same_scene;
    assert(play_scene(&s_gimbal.io, BK7259_GIMBAL_SCENE_SURPRISED, 4) == 1);
    assert(s_gimbal.pending_scene && s_gimbal.revision == 5 && !controller.active && controller.motion_count == 1);
    assert(play_scene(&s_gimbal.io, s_gimbal.scene, s_gimbal.revision) == 0);
    assert(near_target(s_gimbal.io.pan, 0));
    reset_controller(); s_gimbal.prepared = s_gimbal.accepting = s_gimbal.conversation = true;
    s_gimbal.revision = 4; controller.move_hook = hang_up;
    assert(play_scene(&s_gimbal.io, BK7259_GIMBAL_SCENE_SURPRISED, 4) == 1);
    assert(!s_gimbal.conversation && s_gimbal.pending_center && !s_gimbal.pending_scene && !controller.active);
    assert(center(&s_gimbal.io, s_gimbal.revision, false) == 0 && near_target(s_gimbal.io.pan, 0));
    ++groups;
}

static unsigned observation(bool finished, bk7259_gimbal_scene_t scene)
{
    pthread_mutex_lock(&observations_lock);
    unsigned value = finished ? completed[scene] : started[scene];
    pthread_mutex_unlock(&observations_lock);
    return value;
}
static void wait_observation(bool finished, bk7259_gimbal_scene_t scene, unsigned wanted)
{
    uint64_t deadline = monotonic_ns() + 5000000000ULL;
    while (observation(finished, scene) < wanted && monotonic_ns() < deadline) host_sleep_us(1000);
    assert(observation(finished, scene) >= wanted);
}
static void shutdown_and_join(void)
{
    beken_thread_t thread = s_gimbal.worker;
    assert(bk7259_gimbal_shutdown() == 0);
    assert(pthread_join(thread->id, NULL) == 0);
    free(thread);
    assert(!controller.uart_open && !controller.tx_mapped && !controller.rx_mapped && !s_gimbal.prepared);
}

static void test_worker_lifecycle(void)
{
    reset_controller();
    assert(bk7259_gimbal_prepare() == 0 && bk7259_gimbal_prepare() == 0);
    assert(aosl_references == 2 && s_gimbal.aosl_owned);
    assert(bk7259_gimbal_request(BK7259_GIMBAL_SCENE_HAPPY) < 0);
    assert(bk7259_gimbal_set_conversation(true) == 0);
    assert(bk7259_gimbal_request(BK7259_GIMBAL_SCENE_SURPRISED) == 0);
    wait_observation(false, BK7259_GIMBAL_SCENE_SURPRISED, 1);
    assert(bk7259_gimbal_request(BK7259_GIMBAL_SCENE_SURPRISED) == 0);
    wait_observation(false, BK7259_GIMBAL_SCENE_SURPRISED, 2);
    wait_observation(true, BK7259_GIMBAL_SCENE_SURPRISED, 1);
    assert(bk7259_gimbal_request(BK7259_GIMBAL_SCENE_SAD) == 0);
    wait_observation(false, BK7259_GIMBAL_SCENE_SAD, 1);
    assert(bk7259_gimbal_request(BK7259_GIMBAL_SCENE_ANGRY) == 0);
    wait_observation(true, BK7259_GIMBAL_SCENE_ANGRY, 1);
    assert(bk7259_gimbal_request(BK7259_GIMBAL_SCENE_HAPPY) == 0);
    wait_observation(false, BK7259_GIMBAL_SCENE_HAPPY, 1);
    assert(bk7259_gimbal_set_conversation(false) == 0);
    assert(bk7259_gimbal_center_wait(3000) == 0);
    assert(bk7259_gimbal_request(BK7259_GIMBAL_SCENE_HAPPY) < 0);
    deny_done_wait = true;
    assert(bk7259_gimbal_shutdown() < 0);
    assert(s_gimbal.prepared && controller.uart_open && controller.tx_mapped && controller.rx_mapped);
    assert(aosl_references == 2 && s_gimbal.aosl_owned);
    aosl_dtor(); /* Product drops its reference while the worker is retained. */
    assert(aosl_references == 1);
    shutdown_and_join();
    assert(aosl_references == 0 && !s_gimbal.aosl_owned);
    aosl_ctor(); /* Product reference for the next preparation. */
    /* Protocol failure disables future scenes; it must not retry 0x61. */
    reset_controller(); controller.drop_reply = true;
    assert(bk7259_gimbal_prepare() == 0 && bk7259_gimbal_set_conversation(true) == 0);
    assert(bk7259_gimbal_request(BK7259_GIMBAL_SCENE_HAPPY) == 0);
    bool fault = false; uint64_t deadline = monotonic_ns() + 3000000000ULL;
    while (!fault && monotonic_ns() < deadline) {
        rtos_lock_mutex(&s_lock); fault = s_gimbal.fault; unlock(); host_sleep_us(1000);
    }
    assert(fault && controller.motion_count == 1 && controller.stop_count == 1 &&
           bk7259_gimbal_request(BK7259_GIMBAL_SCENE_HAPPY) < 0);
    shutdown_and_join();
    assert(aosl_references == 1 && !s_gimbal.aosl_owned);
    ++groups;
}

static void test_aosl_reference_failure(void)
{
    reset_controller(); deny_thread_create = true;
    assert(bk7259_gimbal_prepare() < 0);
    assert(aosl_references == 1 && !s_gimbal.aosl_owned && !controller.uart_open);
    reset_controller(); deny_thread_create = deny_uart_deinit = true;
    assert(bk7259_gimbal_prepare() < 0);
    assert(aosl_references == 2 && s_gimbal.aosl_owned && !s_gimbal.prepared && controller.uart_open);
    /* Retry releases the failed-init reference before acquiring a new one. */
    assert(bk7259_gimbal_prepare() == 0 && aosl_references == 2);
    shutdown_and_join();
    assert(aosl_references == 1 && !s_gimbal.aosl_owned);
    ++groups;
}

static unsigned command_count(void)
{
    pthread_mutex_lock(&controller.mutex);
    unsigned count = controller.command_count;
    pthread_mutex_unlock(&controller.mutex);
    return count;
}
static void wait_command_count(unsigned wanted)
{
    uint64_t deadline = monotonic_ns() + 3000000000ULL;
    while (command_count() < wanted && monotonic_ns() < deadline) host_sleep_us(1000);
    assert(command_count() >= wanted);
}
static void wait_command_idle(void)
{
    bool busy = true;
    uint64_t deadline = monotonic_ns() + 3000000000ULL;
    while (busy && monotonic_ns() < deadline) {
        rtos_lock_mutex(&s_lock); busy = s_gimbal.pending_command || s_gimbal.command_running; unlock();
        if (busy) host_sleep_us(1000);
    }
    assert(!busy);
}
static void queue_command(void)
{
    int ret;
    uint64_t deadline = monotonic_ns() + 3000000000ULL;
    do {
        ret = bk7259_gimbal_send_next_command();
        if (ret == -2) host_sleep_us(100);
    } while (ret == -2 && monotonic_ns() < deadline);
    assert(ret == 0);
}
static void configure_command(manual_reply_t behavior)
{
    pthread_mutex_lock(&controller.mutex); controller.command_behavior = behavior; pthread_mutex_unlock(&controller.mutex);
}
static void prepare_manual(void)
{
    reset_controller(); controller.manual_wire = true;
    assert(bk7259_gimbal_prepare() == 0);
}
static void test_manual_sequence(void)
{
    prepare_manual();
    rtos_delay_milliseconds(250); assert(command_count() == 0);
    unsigned queries = controller.query_count;
    configure_command(TEST_DROP);
    queue_command();
    size_t cursor = s_gimbal.next_command;
    assert(bk7259_gimbal_send_next_command() == -2 && s_gimbal.next_command == cursor);
    wait_command_count(1);
    assert(bk7259_gimbal_send_next_command() == -2 && s_gimbal.next_command == cursor);
    wait_command_idle();
    configure_command(TEST_FRAGMENTED);
    for (unsigned i = 1; i < 8; ++i) {
        queue_command(); wait_command_count(i + 1); wait_command_idle();
    }
    assert(command_count() == 8 && s_gimbal.next_command == 0);
    for (unsigned i = 0; i < 8; ++i) assert(controller.command_indices[i] == i % 4);
    rtos_delay_milliseconds(5500); assert(command_count() == 8);
    assert(controller.query_count == queries && controller.stop_count == 0);
    assert(bk7259_gimbal_center_wait(100) == -1);
    shutdown_and_join();
    assert(bk7259_gimbal_send_next_command() == -1);
    ++groups;
}
static void test_manual_failures(void)
{
    const manual_reply_t failures[] = {TEST_DROP, TEST_REJECT, TEST_BAD_CRC,
                                      TEST_WRONG_COMMAND, TEST_WRONG_LENGTH, TEST_TX_FAIL};
    for (size_t i = 0; i < ARRAY_COUNT(failures); ++i) {
        prepare_manual(); configure_command(failures[i]);
        queue_command(); wait_command_count(1); wait_command_idle();
        if (failures[i] == TEST_BAD_CRC) assert(s_gimbal.io.command_bad_crc > 0);
        configure_command(TEST_GOOD);
        queue_command(); wait_command_count(2); wait_command_idle();
        assert(controller.command_indices[0] == 0 && controller.command_indices[1] == 1);
        assert(controller.stop_count == 0);
        shutdown_and_join();
    }
    prepare_manual();
    for (unsigned i = 0; i < 3; ++i) { queue_command(); wait_command_count(i + 1); wait_command_idle(); }
    configure_command(TEST_DROP);
    queue_command(); wait_command_count(4); wait_command_idle();
    rtos_delay_milliseconds(2500); assert(command_count() == 4 && controller.command_indices[3] == 3);
    configure_command(TEST_GOOD); queue_command(); wait_command_count(5); wait_command_idle();
    assert(controller.command_indices[4] == 0); /* User advances; no automatic relative retry. */
    shutdown_and_join();
    ++groups;
}
static void test_manual_mcp_and_fault(void)
{
    reset_controller(); assert(bk7259_gimbal_prepare() == 0);
    assert(bk7259_gimbal_set_conversation(true) == 0);
    assert(bk7259_gimbal_request(BK7259_GIMBAL_SCENE_SURPRISED) == 0);
    wait_observation(false, BK7259_GIMBAL_SCENE_SURPRISED, 1);
    assert(bk7259_gimbal_send_next_command() == -2 && !s_gimbal.manual_session && s_gimbal.next_command == 0);
    wait_observation(true, BK7259_GIMBAL_SCENE_SURPRISED, 1);
    assert(bk7259_gimbal_set_conversation(false) == 0 && bk7259_gimbal_center_wait(3000) == 0);
    pthread_mutex_lock(&controller.mutex); controller.manual_wire = true; pthread_mutex_unlock(&controller.mutex);
    rtos_lock_mutex(&s_lock); s_gimbal.fault = true; unlock();
    queue_command(); wait_command_count(1); wait_command_idle();
    assert(s_gimbal.fault && s_gimbal.manual_session);
    assert(bk7259_gimbal_set_conversation(true) == 0);
    assert(bk7259_gimbal_request(BK7259_GIMBAL_SCENE_HAPPY) == -1);
    size_t cursor = s_gimbal.next_command;
    assert(bk7259_gimbal_send_next_command() == -2 && s_gimbal.next_command == cursor);
    assert(bk7259_gimbal_set_conversation(false) == 0);
    rtos_delay_milliseconds(1000); assert(command_count() == 1 && controller.stop_count == 0);
    shutdown_and_join();
    reset_controller(); assert(bk7259_gimbal_prepare() == 0 && !s_gimbal.manual_session && s_gimbal.next_command == 0);
    shutdown_and_join();
    ++groups;
}
static void wait_receive_gate(void)
{
    bool waiting = false;
    uint64_t deadline = monotonic_ns() + 3000000000ULL;
    while (!waiting && monotonic_ns() < deadline) {
        pthread_mutex_lock(&controller.mutex); waiting = controller.rx_waiting; pthread_mutex_unlock(&controller.mutex);
        if (!waiting) host_sleep_us(1000);
    }
    assert(waiting);
}
static void test_manual_shutdown(void)
{
    prepare_manual(); configure_command(TEST_DROP);
    queue_command(); wait_command_count(1);
    deny_done_wait = true; assert(bk7259_gimbal_shutdown() < 0);
    assert(s_gimbal.prepared && controller.uart_open && aosl_references == 2);
    shutdown_and_join(); assert(command_count() == 1 && controller.stop_count == 0);

    reset_controller(); controller.manual_wire = controller.drop_startup_query = controller.hold_receive = true;
    assert(bk7259_gimbal_prepare() == 0); wait_receive_gate();
    uint32_t began = rtos_get_time();
    queue_command();
    pthread_mutex_lock(&controller.mutex); controller.hold_receive = false; pthread_mutex_unlock(&controller.mutex);
    wait_command_count(1); wait_command_idle();
    assert(rtos_get_time() - began < STATUS_TIMEOUT_MS + 550U && controller.query_count == 1);
    shutdown_and_join();

    reset_controller(); controller.manual_wire = controller.drop_startup_query = controller.hold_receive = true;
    assert(bk7259_gimbal_prepare() == 0); wait_receive_gate();
    queue_command(); shutdown_and_join();
    assert(command_count() == 0 && !s_gimbal.pending_command && controller.stop_count == 0);
    ++groups;
}

static void wait_cli_scene_idle(void)
{
    bool busy = true;
    uint64_t deadline = monotonic_ns() + 5000000000ULL;
    while (busy && monotonic_ns() < deadline) {
        rtos_lock_mutex(&s_lock);
        busy = s_gimbal.pending_cli_scene || s_gimbal.cli_scene_running;
        unlock();
        if (busy) host_sleep_us(1000);
    }
    assert(!busy);
}
static void wait_motion_count(unsigned wanted)
{
    uint64_t deadline = monotonic_ns() + 3000000000ULL;
    unsigned count;
    do {
        pthread_mutex_lock(&controller.mutex); count = controller.motion_count; pthread_mutex_unlock(&controller.mutex);
        if (count < wanted) host_sleep_us(1000);
    } while (count < wanted && monotonic_ns() < deadline);
    assert(count >= wanted);
}
static void test_cli_scenes_and_pose(void)
{
    for (int i = 0; i < BK7259_GIMBAL_SCENE_COUNT; ++i) {
        reset_controller();
        /* ACK=00 alone is insufficient: omit every completion notification. */
        controller.complete_reply = false; controller.extra_motion_ms = 40;
        assert(bk7259_gimbal_prepare() == 0);
        assert(bk7259_gimbal_request_cli_scene((bk7259_gimbal_scene_t)i) == 0);
        wait_cli_scene_idle();
        assert(observation(true, (bk7259_gimbal_scene_t)i) == 1);
        assert(!s_gimbal.conversation && s_gimbal.manual_session && !controller.active);
        assert(controller.motion_count > 10 && controller.busy_queries > 0);
        assert(near_target(controller.pan, 0) && near_target(controller.pitch, 0));
        unsigned moves = controller.motion_count, queries = controller.query_count;
        rtos_delay_milliseconds(1500);
        assert(controller.motion_count == moves && controller.query_count == queries);
        shutdown_and_join();
    }
    /* A raw command leaves stale cached pose; the full scene must query first. */
    prepare_manual(); queue_command(); wait_command_count(1); wait_command_idle();
    assert(s_gimbal.io.pan == 0);
    pthread_mutex_lock(&controller.mutex);
    controller.manual_wire = false; controller.pan = -300; controller.pitch = 0;
    unsigned queries = controller.query_count;
    pthread_mutex_unlock(&controller.mutex);
    assert(bk7259_gimbal_request_cli_scene(BK7259_GIMBAL_SCENE_SURPRISED) == 0);
    wait_cli_scene_idle();
    assert(controller.query_count > queries && controller.moves[0].pan < -250);
    assert(observation(true, BK7259_GIMBAL_SCENE_SURPRISED) == 1 && near_target(controller.pan, 0));
    shutdown_and_join();
    ++groups;
}
static void test_cli_scene_admission(void)
{
    reset_controller();
    assert(bk7259_gimbal_request_cli_scene((bk7259_gimbal_scene_t)-1) == -1);
    assert(bk7259_gimbal_request_cli_scene(BK7259_GIMBAL_SCENE_COUNT) == -1);
    assert(bk7259_gimbal_request_cli_scene(BK7259_GIMBAL_SCENE_HAPPY) == -1);
    prepare_manual(); configure_command(TEST_DROP); queue_command(); wait_command_count(1);
    size_t cursor = s_gimbal.next_command;
    assert(bk7259_gimbal_request_cli_scene(BK7259_GIMBAL_SCENE_HAPPY) == -2 && s_gimbal.next_command == cursor);
    wait_command_idle(); shutdown_and_join();

    reset_controller(); controller.drop_startup_query = controller.hold_receive = true;
    assert(bk7259_gimbal_prepare() == 0); wait_receive_gate();
    assert(bk7259_gimbal_set_conversation(true) == 0);
    assert(bk7259_gimbal_request_cli_scene(BK7259_GIMBAL_SCENE_HAPPY) == -2 && !s_gimbal.manual_session);
    assert(bk7259_gimbal_set_conversation(false) == 0);
    assert(bk7259_gimbal_request_cli_scene(BK7259_GIMBAL_SCENE_HAPPY) == 0);
    assert(bk7259_gimbal_request_cli_scene(BK7259_GIMBAL_SCENE_SAD) == -2);
    assert(bk7259_gimbal_send_next_command() == -2 && s_gimbal.next_command == 0);
    pthread_mutex_lock(&controller.mutex);
    controller.drop_startup_query = controller.hold_receive = false;
    controller.extra_motion_ms = 40;
    pthread_mutex_unlock(&controller.mutex);
    wait_observation(false, BK7259_GIMBAL_SCENE_HAPPY, 1);
    assert(bk7259_gimbal_send_next_command() == -2);
    assert(bk7259_gimbal_request_cli_scene(BK7259_GIMBAL_SCENE_ANGRY) == -2);
    assert(bk7259_gimbal_set_conversation(true) == 0 && bk7259_gimbal_request(BK7259_GIMBAL_SCENE_HAPPY) == -1);
    wait_cli_scene_idle();
    assert(observation(true, BK7259_GIMBAL_SCENE_HAPPY) == 1);
    assert(bk7259_gimbal_set_conversation(false) == 0);
    shutdown_and_join();
    ++groups;
}
static void test_cli_scene_status_rejection(void)
{
    for (unsigned kind = 0; kind < 7; ++kind) {
        reset_controller();
        controller.drop_startup_query = kind == 0;
        controller.corrupt_status = kind == 1;
        controller.flow = kind == 2 ? 1 : 2;
        controller.status_busy = kind == 3;
        controller.pan = kind == 4 ? 805 : 0;
        controller.pitch = kind == 5 ? -205 : kind == 6 ? 105 : 0;
        assert(bk7259_gimbal_prepare() == 0);
        rtos_lock_mutex(&s_lock); s_gimbal.fault = true; unlock();
        assert(bk7259_gimbal_request_cli_scene(BK7259_GIMBAL_SCENE_SAD) == 0);
        wait_cli_scene_idle();
        assert(controller.motion_count == 0 && controller.stop_count == 0 && s_gimbal.fault);
        assert(observation(false, BK7259_GIMBAL_SCENE_SAD) == 0);
        pthread_mutex_lock(&controller.mutex); controller.manual_wire = true; pthread_mutex_unlock(&controller.mutex);
        queue_command(); wait_command_count(1); wait_command_idle();
        assert(s_gimbal.fault && controller.command_indices[0] == 0);
        shutdown_and_join();
    }
    ++groups;
}
static void stop_cli_scene_thread(void *unused)
{
    int *result = unused;
    *result = bk7259_gimbal_shutdown();
}
static void *stop_cli_scene_entry(void *argument)
{ stop_cli_scene_thread(argument); return NULL; }
static void test_cli_scene_shutdown(void)
{
    reset_controller(); controller.drop_startup_query = controller.hold_receive = true;
    assert(bk7259_gimbal_prepare() == 0); wait_receive_gate();
    assert(bk7259_gimbal_request_cli_scene(BK7259_GIMBAL_SCENE_HAPPY) == 0);
    shutdown_and_join();
    assert(controller.motion_count == 0 && controller.stop_count == 0 && !s_gimbal.pending_cli_scene);

    for (unsigned closing_only = 0; closing_only < 2; ++closing_only) {
        reset_controller(); controller.hold_motion = true;
        assert(bk7259_gimbal_prepare() == 0);
        assert(bk7259_gimbal_request_cli_scene(BK7259_GIMBAL_SCENE_HAPPY) == 0);
        wait_motion_count(1);
        int stopped = 99; pthread_t stop_thread;
        if (closing_only) {
            rtos_lock_mutex(&s_lock); s_gimbal.accepting = false; assert(!s_gimbal.stopping); unlock();
        } else {
            deny_done_wait = true;
            assert(pthread_create(&stop_thread, NULL, stop_cli_scene_entry, &stopped) == 0);
            bool stopping_now = false;
            uint64_t deadline = monotonic_ns() + 1000000000ULL;
            while (!stopping_now && monotonic_ns() < deadline) {
                rtos_lock_mutex(&s_lock); stopping_now = s_gimbal.stopping; unlock();
                if (!stopping_now) host_sleep_us(100);
            }
            assert(stopping_now);
            assert(pthread_join(stop_thread, NULL) == 0 && stopped == -1);
            assert(s_gimbal.prepared && controller.uart_open && aosl_references == 2);
        }
        pthread_mutex_lock(&controller.mutex); controller.hold_motion = false; pthread_mutex_unlock(&controller.mutex);
        if (closing_only) wait_cli_scene_idle();
        shutdown_and_join();
        assert(controller.motion_count == 1 && controller.stop_count == 0 && !controller.active);
    }
    ++groups;
}


int main(void)
{
    clock_origin = monotonic_ns();
    aosl_ctor();
    assert(rtos_init_mutex(&s_lock) == BK_OK && rtos_init_semaphore(&s_rx, 1) == BK_OK &&
           rtos_init_semaphore(&s_wake, 1) == BK_OK && rtos_init_semaphore(&s_done, 1) == BK_OK &&
           rtos_init_semaphore(&s_centered, 1) == BK_OK);
    test_crc_and_stream();
    test_completion_and_failures();
    test_presets_and_serial_execution();
    test_segment_cancellation();
    test_worker_lifecycle();
    test_aosl_reference_failure();
    test_manual_sequence();
    test_manual_failures();
    test_manual_mcp_and_fault();
    test_manual_shutdown();
    test_cli_scenes_and_pose();
    test_cli_scene_admission();
    test_cli_scene_status_rejection();
    test_cli_scene_shutdown();
    beken_semaphore_t *events[] = {&s_rx, &s_wake, &s_done, &s_centered};
    for (size_t i = 0; i < ARRAY_COUNT(events); ++i) {
        pthread_mutex_destroy(&(*events[i])->mutex);
        free(*events[i]); *events[i] = NULL;
    }
    pthread_mutex_destroy(s_lock); free(s_lock); s_lock = NULL;
    aosl_dtor();
    assert(aosl_references == 0 && aosl_constructors == aosl_destructors);
    printf("PASS: %u groups (six motion groups, four raw-command groups, four CLI-scene groups)\n", groups);
    return 0;
}
