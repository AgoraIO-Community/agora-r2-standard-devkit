/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <common/bk_err.h>
#include <pthread.h>
typedef pthread_mutex_t *beken_mutex_t;
typedef struct host_semaphore *beken_semaphore_t;
typedef struct host_thread *beken_thread_t;
#define BEKEN_APPLICATION_PRIORITY 5
bk_err_t rtos_init_mutex(beken_mutex_t *mutex);
bk_err_t rtos_lock_mutex(beken_mutex_t *mutex);
bk_err_t rtos_unlock_mutex(beken_mutex_t *mutex);
bk_err_t rtos_init_semaphore(beken_semaphore_t *semaphore, int max_count);
bk_err_t rtos_set_semaphore(beken_semaphore_t *semaphore);
bk_err_t rtos_get_semaphore(beken_semaphore_t *semaphore, uint32_t timeout_ms);
uint32_t rtos_get_time(void);
bk_err_t rtos_delay_milliseconds(uint32_t milliseconds);
bk_err_t rtos_create_psram_thread(beken_thread_t *thread, uint8_t priority,
                                const char *name, void (*function)(void *),
                                uint32_t stack_size, void *argument);
bk_err_t rtos_delete_thread(beken_thread_t *thread);
