/* Modified for MyBot BK7259: preserve AOSL stack and priority requirements. */
#include <common/bk_include.h>
#include <FreeRTOS_POSIX.h>
#include <posix/pthread.h>
#include <os/os.h>
#include <assert.h>
#include <string.h>

#include <api/aosl_mm.h>
#include <api/aosl_log.h>
#include <api/aosl_defs.h>
#include <hal/aosl_hal_thread.h>

// Verify that AOSL_STATIC_MUTEX_SIZE is large enough for pthread_mutex_t
aosl_static_assert(sizeof(pthread_mutex_t) <= AOSL_STATIC_MUTEX_SIZE, 
                   static_mutex_size_check);

// Wrapper structure to pass pthread-style entry function to FreeRTOS task
typedef struct {
  void *(*entry)(void *);
  void *arg;
} thread_wrapper_args_t;

static uint8_t thread_priority_to_beken(aosl_thread_proiority_e priority)
{
  switch (priority) {
    case AOSL_THRD_PRI_LOW:
      return BEKEN_APPLICATION_PRIORITY;
    case AOSL_THRD_PRI_HIGH:
      return BEKEN_DEFAULT_WORKER_PRIORITY - 1;
    case AOSL_THRD_PRI_HIGHEST:
      return BEKEN_DEFAULT_WORKER_PRIORITY - 2;
    case AOSL_THRD_PRI_RT:
      return BEKEN_DEFAULT_WORKER_PRIORITY - 3;
    case AOSL_THRD_PRI_DEFAULT:
    case AOSL_THRD_PRI_NORMAL:
    default:
      return BEKEN_DEFAULT_WORKER_PRIORITY;
  }
}

// Wrapper function to convert pthread-style entry to FreeRTOS task
static void thread_wrapper(void *arg)
{
  thread_wrapper_args_t *wrapper_args = (thread_wrapper_args_t *)arg;
  void *(*entry)(void *) = wrapper_args->entry;
  void *user_arg = wrapper_args->arg;
  
  // Free the wrapper args
  aosl_free(wrapper_args);
  
  // Call the pthread-style entry function
  void *retval = entry(user_arg);
  
  // FreeRTOS tasks should not return, so delete the task
  (void)retval;
  rtos_delete_thread(NULL);
}

int aosl_hal_thread_create(aosl_thread_t *thread, aosl_thread_param_t *param,
                           void *(*entry)(void *), void *arg)
{
  bk_err_t ret;
  beken_thread_t beken_thread = NULL;

  assert(sizeof(beken_thread_t) <= sizeof(aosl_thread_t));

  if (!thread || !param || !entry || param->stack_size <= 0) {
    return -1;
  }
  *thread = (aosl_thread_t)0;
  
  // Allocate wrapper args to pass both entry function and user args
  thread_wrapper_args_t *wrapper_args = aosl_malloc(sizeof(thread_wrapper_args_t));
  if (!wrapper_args) {
    return -1;
  }
  wrapper_args->entry = entry;
  wrapper_args->arg = arg;
  
  ret = rtos_create_psram_thread(&beken_thread, thread_priority_to_beken(param->priority),
                                 param->name, (beken_thread_function_t)thread_wrapper,
                                 (uint32_t)param->stack_size,
                                 (beken_thread_arg_t)wrapper_args);
  if (ret != kNoErr) {
    AOSL_LOG_ERR("thread create failed, name=%s, err=%d",
                 param->name ? param->name : "(null)", ret);
    aosl_free(wrapper_args);
    return -1;
  }

  *thread = (aosl_thread_t)beken_thread;
  return 0;
}

void aosl_hal_thread_destroy(aosl_thread_t thread)
{
  beken_thread_t beken_thread = (beken_thread_t)thread;
  rtos_delete_thread(&beken_thread);
}

void aosl_hal_thread_exit(void *retval)
{
  rtos_delete_thread(NULL);
}

aosl_thread_t aosl_hal_thread_self()
{
  return (aosl_thread_t)rtos_get_current_thread();
}

int aosl_hal_thread_set_name(const char *name)
{
  return 0;
}

int aosl_hal_thread_get_name(char *name, size_t size)
{
  (void)name;
  (void)size;
  return -1;
}

int aosl_hal_thread_set_priority(aosl_thread_proiority_e priority)
{
  return 0;
}

int aosl_hal_thread_join(aosl_thread_t thread, void **retval)
{
  return 0;
}

void aosl_hal_thread_detach(aosl_thread_t thread)
{
  return;
}

aosl_mutex_t aosl_hal_mutex_create()
{
  pthread_mutex_t *n_mutex = aosl_calloc(1, sizeof(pthread_mutex_t));
  if (NULL == n_mutex) {
    return NULL;
  }

  int err = pthread_mutex_init(n_mutex, NULL);
  if (err != 0) {
    AOSL_LOG_ERR("mutex create failed, err=%d", err);
    aosl_free(n_mutex);
    return NULL;
  }

  return (aosl_mutex_t)n_mutex;
}

void aosl_hal_mutex_destroy(aosl_mutex_t mutex)
{
  pthread_mutex_t *n_mutex = (pthread_mutex_t *)mutex;
  if (NULL == n_mutex) {
    return;
  }

  pthread_mutex_destroy(n_mutex);
  aosl_free(n_mutex);
}

int aosl_hal_mutex_lock(aosl_mutex_t mutex)
{
  return pthread_mutex_lock((pthread_mutex_t *)mutex);
}

int aosl_hal_mutex_trylock(aosl_mutex_t mutex)
{
  return pthread_mutex_trylock((pthread_mutex_t *)mutex);
}

int aosl_hal_mutex_unlock(aosl_mutex_t mutex)
{
  return pthread_mutex_unlock((pthread_mutex_t *)mutex);
}

int aosl_hal_static_mutex_init(aosl_static_mutex_t *mutex)
{
  if (!mutex) {
    return -1;
  }

  // Initialize with PTHREAD_MUTEX_INITIALIZER and copy to opaque array
  pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER;
  memcpy(mutex->opaque, &init_mutex, sizeof(pthread_mutex_t));
  
  return 0;
}

void aosl_hal_static_mutex_fini(aosl_static_mutex_t *mutex)
{
  (void)mutex;
}

aosl_cond_t aosl_hal_cond_create(void)
{
  pthread_cond_t *n_cond = aosl_calloc(1, sizeof(pthread_cond_t));
  if (NULL == n_cond) {
    return NULL;
  }

  int err = pthread_cond_init(n_cond, NULL);
  if (err != 0) {
    AOSL_LOG_ERR("cond create failed, err=%d", err);
    aosl_free(n_cond);
    return NULL;
  }

  return (aosl_cond_t)n_cond;
}

void aosl_hal_cond_destroy(aosl_cond_t cond)
{
  pthread_cond_t *n_cond = (pthread_cond_t *)cond;
  if (NULL == n_cond) {
    return;
  }

  pthread_cond_destroy(n_cond);
  aosl_free(n_cond);
}

int aosl_hal_cond_signal(aosl_cond_t cond)
{
  pthread_cond_t *n_cond = (pthread_cond_t *)cond;
  if (NULL == n_cond) {
    return -1;
  }

  return pthread_cond_signal(n_cond);
}

int aosl_hal_cond_broadcast(aosl_cond_t cond)
{
  pthread_cond_t *n_cond = (pthread_cond_t *)cond;
  if (NULL == n_cond) {
    return -1;
  }

  return pthread_cond_broadcast(n_cond);
}

int aosl_hal_cond_wait(aosl_cond_t cond, aosl_mutex_t mutex)
{
  pthread_cond_t *n_cond = (pthread_cond_t *)cond;
  pthread_mutex_t *n_mutex = (pthread_mutex_t *)mutex;

  if (NULL == n_cond || NULL == n_mutex) {
    return -1;
  }

  return pthread_cond_wait(n_cond, n_mutex);
}

int aosl_hal_cond_timedwait(aosl_cond_t cond, aosl_mutex_t mutex, intptr_t timeout)
{
  pthread_cond_t *n_cond = (pthread_cond_t *)cond;
  pthread_mutex_t *n_mutex = (pthread_mutex_t *)mutex;

  if (NULL == n_cond || NULL == n_mutex) {
    return -1;
  }

  struct timespec timeo;
  struct timespec now;
  // Use CLOCK_REALTIME because pthread_cond_timedwait uses CLOCK_REALTIME by default
  clock_gettime (CLOCK_REALTIME, &now);
  timeo.tv_sec = now.tv_sec + timeout / 1000;
  timeo.tv_nsec = now.tv_nsec + (timeout % 1000) * 1000000;
  while (timeo.tv_nsec >= 1000000000) {
    timeo.tv_nsec -= 1000000000;
    timeo.tv_sec++;
  }

  return pthread_cond_timedwait(n_cond, n_mutex, &timeo);
}
