/* SPDX-License-Identifier: Apache-2.0 */
#ifndef TOUCH_RESET_HOST_H_
#define TOUCH_RESET_HOST_H_
#include <mybot/platform/mybot_key.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MYBOT_BK7259_OPS_H_
#define BK7259_PLATFORM_LOG_H_
#define BK_OK 0
#define BEKEN_WAIT_FOREVER UINT32_MAX
#define BEKEN_DEFAULT_WORKER_PRIORITY 5
#define AOSL_LOG_NOTICE 5
#define CONFIG_MYBOT_KEY_GPIO 47
#define CONFIG_MYBOT_KEY_ACTIVE_LEVEL 0
#define BK_LOGI(tag, ...) host_log(__VA_ARGS__)
#define BK_LOGW(tag, ...) host_log(__VA_ARGS__)
#define BK_LOGE(tag, ...) host_log(__VA_ARGS__)
#define MYBOT_LOGI(tag, ...) host_log(__VA_ARGS__)
#define MYBOT_LOGW(tag, ...) host_log(__VA_ARGS__)
#define MYBOT_LOGE(tag, ...) host_log(__VA_ARGS__)
typedef int bk_err_t;
typedef int gpio_id_t;
typedef void *beken_mutex_t;
typedef void *beken_semaphore_t;
typedef void *beken_thread_t;
typedef void *beken_thread_arg_t;
typedef void (*beken_thread_function_t)(beken_thread_arg_t);
typedef struct {
  bool started;
  void (*callback)(void *);
  void *arg;
} beken_timer_t;

void host_log(const char *, ...);
int rtos_init_mutex(beken_mutex_t *);
int rtos_deinit_mutex(beken_mutex_t *);
int rtos_lock_mutex(beken_mutex_t *);
int rtos_unlock_mutex(beken_mutex_t *);
int rtos_init_semaphore(beken_semaphore_t *, int);
int rtos_deinit_semaphore(beken_semaphore_t *);
int rtos_get_semaphore(beken_semaphore_t *, uint32_t);
int rtos_set_semaphore(beken_semaphore_t *);
int rtos_init_timer(beken_timer_t *, uint32_t, void (*)(void *), void *);
int rtos_start_timer(beken_timer_t *);
int rtos_stop_timer(beken_timer_t *);
int rtos_deinit_timer(beken_timer_t *);
uint32_t rtos_get_time(void);
int rtos_delay_milliseconds(uint32_t);
int rtos_create_psram_thread(beken_thread_t *, uint8_t, const char *,
                             beken_thread_function_t, uint32_t,
                             beken_thread_arg_t);
int rtos_delete_thread(beken_thread_t *);
int bk_gpio_get_input(gpio_id_t);
int bk_gpio_disable_output(gpio_id_t);
int bk_gpio_enable_input(gpio_id_t);
int bk_gpio_enable_pull(gpio_id_t);
int bk_gpio_pull_up(gpio_id_t);
int bk_gpio_pull_down(gpio_id_t);
int bk_init(void);
int bk_uid_get_data(uint8_t *);
int media_service_init(void);
void bk_reboot(void);
void aosl_ctor(void);
void aosl_dtor(void);
void aosl_set_log_level(int);
#endif
