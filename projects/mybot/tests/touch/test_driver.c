/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "tp_driver.c"
#include "board_gpio_fixture.h"

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    unsigned count;
} fake_semaphore_t;
static unsigned semaphore_count;
static unsigned bus_refs, active_reads, workers, callbacks;
static unsigned callback_count, reset_delay_count;
static bool fail_bus, fail_detect, fail_sensor, fail_thread, fail_irq, fail_disable;
static bool fail_release_bus, fail_exit_wait, block_read, fail_read;
static void (*registered_irq)(gpio_id_t);
static sw_i2c_handle_t fake_bus;
static unsigned gpio_modes[72];
static bool irq_output_high;
static unsigned irq_output_configs, irq_input_configs;
tp_sensor_detect_func_t __tp_sensor_detect_array_start;
extern tp_sensor_detect_func_t __tp_sensor_detect_array_end
    __attribute__((alias("__tp_sensor_detect_array_start")));

static void pause_ms(unsigned milliseconds) {
    struct timespec delay = {.tv_sec = milliseconds / 1000,
                             .tv_nsec = (milliseconds % 1000) * 1000000L};
    nanosleep(&delay, NULL);
}
static unsigned load(unsigned *value) { return __atomic_load_n(value, __ATOMIC_ACQUIRE); }
static void wait_nonzero(unsigned *value) {
    for (unsigned i = 0; !load(value) && i < 1000; ++i) pause_ms(1);
    assert(load(value));
}
static void wait_zero(unsigned *value) {
    for (unsigned i = 0; load(value) && i < 1000; ++i) pause_ms(1);
    assert(!load(value));
}
bk_err_t rtos_init_semaphore_ex(beken_semaphore_t *semaphore, int max_count, int initial) {
    assert(max_count == 1 && initial == 0);
    fake_semaphore_t *native = calloc(1, sizeof(*native));
    assert(native && pthread_mutex_init(&native->lock, NULL) == 0);
    assert(pthread_cond_init(&native->changed, NULL) == 0);
    *semaphore = native;
    semaphore_count++;
    return BK_OK;
}
bk_err_t rtos_set_semaphore(beken_semaphore_t *semaphore) {
    fake_semaphore_t *native = *semaphore;
    assert(native && pthread_mutex_lock(&native->lock) == 0);
    native->count = 1;
    pthread_cond_signal(&native->changed);
    pthread_mutex_unlock(&native->lock);
    return BK_OK;
}
bk_err_t rtos_get_semaphore(beken_semaphore_t *semaphore, uint32_t timeout) {
    if (semaphore == &tp_exited && timeout > 0 && fail_exit_wait) {
        fail_exit_wait = false;
        return kTimeoutErr;
    }
    fake_semaphore_t *native = *semaphore;
    assert(native && pthread_mutex_lock(&native->lock) == 0);
    if (!native->count && timeout) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += timeout / 1000;
        deadline.tv_nsec += (long)(timeout % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) { deadline.tv_nsec -= 1000000000L; deadline.tv_sec++; }
        while (!native->count) {
            int result = pthread_cond_timedwait(&native->changed, &native->lock, &deadline);
            if (result == ETIMEDOUT) break;
            assert(result == 0);
        }
    }
    const bool ready = native->count > 0;
    if (ready) native->count--;
    pthread_mutex_unlock(&native->lock);
    return ready ? BK_OK : kTimeoutErr;
}
typedef struct { beken_thread_function_t entry; beken_thread_arg_t arg; } thread_start_t;
static void *run_thread(void *arg) {
    thread_start_t start = *(thread_start_t *)arg;
    free(arg);
    start.entry(start.arg);
    assert(false);
    return NULL;
}
bk_err_t rtos_create_thread(beken_thread_t *thread, uint8_t priority, const char *name,
                           beken_thread_function_t entry, uint32_t stack, beken_thread_arg_t arg) {
    assert(priority == 4 && stack == 2048);
    if (fail_thread) return BK_FAIL;
    thread_start_t *start = malloc(sizeof(*start));
    assert(start);
    *start = (thread_start_t){entry, arg};
    pthread_t native;
    __atomic_add_fetch(&workers, 1, __ATOMIC_RELEASE);
    assert(pthread_create(&native, NULL, run_thread, start) == 0);
    assert(pthread_detach(native) == 0);
    *thread = (void *)1;
    return BK_OK;
}
bk_err_t rtos_delete_thread(beken_thread_t *thread) {
    assert(!thread); /* Never force-delete another task holding the I2C lock. */
    __atomic_sub_fetch(&workers, 1, __ATOMIC_RELEASE);
    pthread_exit(NULL);
    return BK_OK;
}
void rtos_delay_milliseconds(uint32_t delay) { if (delay == 20) reset_delay_count++; }
uint32_t rtos_get_time(void) { return 123; }
bk_err_t bk_gpio_set_config(gpio_id_t pin, const gpio_config_t *config) {
    assert(pin >= 0 && (unsigned)pin < sizeof(gpio_modes) / sizeof(gpio_modes[0]));
    if (!board_gpio_mode_allowed((unsigned)pin, config->io_mode)) {
        return BK_FAIL;
    }
    gpio_modes[pin] = (unsigned)config->io_mode;
    if (pin == CONFIG_TP_INT_PIN) {
        if (config->io_mode == GPIO_OUTPUT_ENABLE) {
            irq_output_high = false;
            irq_output_configs++;
        } else {
            assert(config->io_mode == GPIO_INPUT_ENABLE && irq_output_high);
            irq_input_configs++;
        }
    }
    return BK_OK;
}
bk_err_t bk_gpio_set_output_high(gpio_id_t pin) {
    assert(gpio_modes[pin] == GPIO_OUTPUT_ENABLE);
    if (pin == CONFIG_TP_INT_PIN) irq_output_high = true;
    return BK_OK;
}
bk_err_t bk_gpio_set_output_low(gpio_id_t pin) {
    assert(gpio_modes[pin] == GPIO_OUTPUT_ENABLE);
    return BK_OK;
}
bk_err_t bk_gpio_disable_interrupt(gpio_id_t pin) { return fail_disable ? BK_FAIL : BK_OK; }
bk_err_t bk_gpio_enable_interrupt(gpio_id_t pin) { return fail_irq ? BK_FAIL : BK_OK; }
bk_err_t bk_gpio_register_isr(gpio_id_t pin, void (*callback)(gpio_id_t)) {
    assert(pin == CONFIG_TP_INT_PIN && gpio_modes[pin] == GPIO_INPUT_ENABLE && irq_output_high);
    registered_irq = callback;
    return BK_OK;
}
bk_err_t bk_gpio_set_interrupt_type(gpio_id_t pin, gpio_int_type_t type) { return BK_OK; }
sw_i2c_handle_t *sw_i2c_init(const sw_i2c_config_t *config) {
    assert(config->sda_pin == 65 && config->scl_pin == 64);
    if (fail_bus || !board_gpio_soft_i2c_allowed((unsigned)config->sda_pin,
                                                (unsigned)config->scl_pin)) return NULL;
    assert(!bus_refs);
    bus_refs++;
    return &fake_bus;
}
bk_err_t sw_i2c_deinit(sw_i2c_handle_t *handle) {
    assert(handle == &fake_bus && bus_refs == 1 && !load(&active_reads) && !load(&callbacks));
    if (fail_release_bus) return BK_FAIL;
    bus_refs--;
    return BK_OK;
}
bk_err_t sw_i2c_memory_read(sw_i2c_handle_t *handle, i2c_mem_param_t *param) { return BK_OK; }
bk_err_t sw_i2c_memory_write(sw_i2c_handle_t *handle, i2c_mem_param_t *param) { return BK_OK; }
bk_err_t sw_i2c_master_read(sw_i2c_handle_t *handle, uint32_t addr, uint8_t *data,
                           uint32_t len, uintptr_t timeout) { return BK_OK; }
bk_err_t sw_i2c_master_write(sw_i2c_handle_t *handle, uint32_t addr, const uint8_t *data,
                            uint32_t len, uintptr_t timeout) { return BK_OK; }
bk_err_t bk_i2c_init(int id, i2c_config_t *config) { return BK_OK; }
static bool detect(const tp_i2c_callback_t *bus) { return !fail_detect; }
static int sensor_init(const tp_i2c_callback_t *bus, tp_sensor_user_config_t *config) {
    assert(config->x_size == 480 && config->y_size == 640 && config->tp_num == 2);
    return fail_sensor ? BK_FAIL : BK_OK;
}
static int sensor_read(const tp_i2c_callback_t *bus, uint8_t max, uint8_t *data) {
    assert(max == 2);
    __atomic_add_fetch(&active_reads, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&block_read, __ATOMIC_ACQUIRE)) pause_ms(1);
    __atomic_sub_fetch(&active_reads, 1, __ATOMIC_RELEASE);
    return fail_read ? BK_FAIL : BK_OK;
}
static const tp_sensor_config_t sensor = {
    .name = "test-cst7xx", .detect = detect, .init = sensor_init, .read_tp_info = sensor_read,
};
static void on_frame(const tp_data_t *data, uint8_t count, void *arg) {
    assert(count == 2 && arg == &sensor);
    __atomic_add_fetch(&callbacks, 1, __ATOMIC_RELEASE);
    if (fail_read) assert(!data);
    else assert(data);
    __atomic_add_fetch(&callback_count, 1, __ATOMIC_RELEASE);
    __atomic_sub_fetch(&callbacks, 1, __ATOMIC_RELEASE);
}
static tp_config_t config = {
    .ppi = (480U << 16) | 640U, .int_type = TP_INT_TYPE_FALLING_EDGE,
    .refresh_rate = 20, .tp_num = 2,
};
static void stopped(void) {
    assert(bk_tp_driver_deinit() == BK_OK);
    wait_zero(&workers);
    assert(!bus_refs && !tp_thread_handle && !tp_i2c_handle);
    assert(bk_tp_driver_set_callback(on_frame, (void *)&sensor) == BK_OK);
}
static void test_board_direction_contract(void) {
    const struct { unsigned pin; int mode; } required[] = {
        {CONFIG_TP_INT_PIN, GPIO_OUTPUT_ENABLE},
        {CONFIG_TP_INT_PIN, GPIO_INPUT_ENABLE},
        {CONFIG_TP_RST_PIN, GPIO_OUTPUT_ENABLE},
        {CONFIG_TP_I2C_SCL_PIN, GPIO_OUTPUT_ENABLE},
        {CONFIG_TP_I2C_SDA_PIN, GPIO_OUTPUT_ENABLE},
        {CONFIG_TP_I2C_SDA_PIN, GPIO_INPUT_ENABLE},
    };
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        assert(board_gpio_mode_allowed(required[i].pin, required[i].mode));
    }
    assert(board_gpio_soft_i2c_allowed(CONFIG_TP_I2C_SDA_PIN, CONFIG_TP_I2C_SCL_PIN));
    /* Mutate only the host copy. Each missing declaration must make the real
     * TP initialization fail, including the original input-only IRQ defect. */
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        board_gpio_restore();
        assert(board_gpio_remove_capability(required[i].pin, required[i].mode));
        assert(bk_tp_driver_init(&config) != BK_OK);
        stopped();
    }
    board_gpio_restore();
    const unsigned outputs_before = irq_output_configs;
    const unsigned inputs_before = irq_input_configs;
    assert(bk_tp_driver_init(&config) == BK_OK);
    assert(irq_output_configs == outputs_before + 1 && irq_input_configs == inputs_before + 1);
    stopped();
    puts("GPIO board contract: actual AP table permits reset OUTPUT-high to IRQ INPUT and shared I2C; missing abilities rejected");
}
int main(void) {
    const tp_sensor_config_t *sensors[] = {&sensor};
    bk_tp_set_sensor_devices_list(sensors, 1);
    assert(bk_tp_driver_set_callback(on_frame, (void *)&sensor) == BK_OK);
    board_gpio_restore();
    test_board_direction_contract();
    bool *failures[] = {&fail_bus, &fail_detect, &fail_sensor, &fail_thread, &fail_irq};
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        *failures[i] = true;
        assert(bk_tp_driver_init(&config) != BK_OK);
        *failures[i] = false;
        stopped();
    }
    assert(bk_tp_driver_init(&config) == BK_OK);
    wait_nonzero(&callback_count); /* Initial idle frame needs no GPIO IRQ. */
    assert(bk_tp_driver_set_callback(NULL, NULL) != BK_OK);
    assert(bk_tp_driver_init(&config) != BK_OK);
    unsigned before = load(&callback_count);
    fail_read = true;
    registered_irq(43);
    for (unsigned i = 0; load(&callback_count) == before && i < 1000; ++i) pause_ms(1);
    assert(load(&callback_count) > before);
    fail_read = false;
    stopped();
    registered_irq(43); /* Late dispatched IRQ is safe after stop. */
    block_read = true;
    assert(bk_tp_driver_init(&config) == BK_OK);
    wait_nonzero(&active_reads);
    fail_exit_wait = true;
    assert(bk_tp_driver_deinit() != BK_OK);
    assert(bus_refs == 1 && tp_i2c_handle && tp_thread_handle);
    __atomic_store_n(&block_read, false, __ATOMIC_RELEASE);
    stopped(); /* Cooperatively waits until I2C returns, then releases the bus. */
    assert(bk_tp_driver_init(&config) == BK_OK);
    fail_disable = true;
    assert(bk_tp_driver_deinit() != BK_OK && bus_refs == 1);
    fail_disable = false;
    stopped();
    assert(bk_tp_driver_init(&config) == BK_OK);
    fail_release_bus = true;
    assert(bk_tp_driver_deinit() != BK_OK && bus_refs == 1 && !tp_thread_handle);
    fail_release_bus = false;
    stopped();
    for (unsigned i = 0; i < 10; ++i) {
        assert(bk_tp_driver_init(&config) == BK_OK);
        stopped();
    }
    assert(semaphore_count == 2 && reset_delay_count > 10);
    assert(bk_tp_driver_set_callback(NULL, NULL) == BK_OK);
    puts("TP driver: init faults, initial read, batch errors, cooperative stop, retained retries and 10 cycles passed");
    return 0;
}
