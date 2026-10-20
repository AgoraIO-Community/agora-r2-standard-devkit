/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <common/bk_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define __bk_weak __attribute__((weak))
#define BK_LOGE(...) ((void)0)
#define BK_LOGW(...) ((void)0)
#define BK_LOGI(...) ((void)0)
#define BK_LOGD(...) ((void)0)
#define BK_LOGV(...) ((void)0)
typedef void *beken_mutex_t;
typedef void *beken_semaphore_t;
typedef void *beken_thread_t;
typedef void *beken_thread_arg_t;
typedef void (*beken_thread_function_t)(beken_thread_arg_t);
bk_err_t rtos_init_mutex(beken_mutex_t *mutex);
bk_err_t rtos_lock_mutex(beken_mutex_t *mutex);
bk_err_t rtos_unlock_mutex(beken_mutex_t *mutex);
bk_err_t rtos_init_semaphore_ex(beken_semaphore_t *semaphore, int max_count, int count);
bk_err_t rtos_get_semaphore(beken_semaphore_t *semaphore, uint32_t timeout);
bk_err_t rtos_set_semaphore(beken_semaphore_t *semaphore);
bk_err_t rtos_create_thread(beken_thread_t *thread, uint8_t priority, const char *name,
                           beken_thread_function_t entry, uint32_t stack, beken_thread_arg_t arg);
bk_err_t rtos_delete_thread(beken_thread_t *thread);
void rtos_delay_milliseconds(uint32_t delay);
uint32_t rtos_get_time(void);
