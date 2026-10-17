/*
 * BK7259 AOSL atomic HAL.
 *
 * All AOSL allocations go through psram_malloc() (see aosl_hal_memory.c), so the
 * objects that carry AOSL atomics (struct mp_queue and its ->count /
 * ->kick_q_count fields, ref objects, timers, ...) normally live in PSRAM.
 *
 * On BK7259 the PSRAM controller is not on the bus that supports the ARMv8-M
 * LDREX/STREX exclusive monitors reliably, so GCC __atomic_* builtins (which
 * lower to LDREX/STREX for read-modify-write) must NOT be used on PSRAM
 * addresses: an RMW can silently lose a core's update or spin forever, and on
 * the dual-core SMP build the cross-core handshake that wakes a blocked mpq
 * worker (need_kicking / q->count / kick_q_count) is exactly such an RMW.
 *
 * This mirrors the reference Agora BK725x port (agora-iot-sdk
 * hal/aosl/platform/src/bk725x/aosl_hal_atomic.c): every atomic is performed as
 * a plain volatile access inside a global critical section, so mutual exclusion
 * comes from the RTOS lock (kept in SRAM) instead of the hardware exclusive
 * monitor.  rtos_enter_global_critical() is the SMP-aware critical section this
 * SDK provides (os.h redefines GLOBAL_INT_DISABLE to it under CONFIG_SOC_SMP):
 * it disables the local core's interrupts and serializes against the other core
 * through the FreeRTOS kernel spinlock, so it is safe regardless of which core
 * the calling thread is running on.
 */
#include <stdint.h>

#include <os/os.h>

#include <hal/aosl_hal_atomic.h>

intptr_t aosl_hal_atomic_read(const intptr_t *v)
{
  uint32_t f = rtos_enter_global_critical();
  intptr_t x = *(const volatile intptr_t *)v;
  __sync_synchronize();
  rtos_exit_global_critical(f);
  return x;
}

void aosl_hal_atomic_set(intptr_t *v, intptr_t i)
{
  uint32_t f = rtos_enter_global_critical();
  *(volatile intptr_t *)v = i;
  __sync_synchronize();
  rtos_exit_global_critical(f);
}

intptr_t aosl_hal_atomic_inc(intptr_t *v)
{
  uint32_t f = rtos_enter_global_critical();
  intptr_t prev = *(volatile intptr_t *)v;
  *(volatile intptr_t *)v = prev + 1;
  __sync_synchronize();
  rtos_exit_global_critical(f);
  return prev;
}

intptr_t aosl_hal_atomic_dec(intptr_t *v)
{
  uint32_t f = rtos_enter_global_critical();
  intptr_t prev = *(volatile intptr_t *)v;
  *(volatile intptr_t *)v = prev - 1;
  __sync_synchronize();
  rtos_exit_global_critical(f);
  return prev;
}

intptr_t aosl_hal_atomic_add(intptr_t i, intptr_t *v)
{
  uint32_t f = rtos_enter_global_critical();
  intptr_t next = *(volatile intptr_t *)v + i;
  *(volatile intptr_t *)v = next;
  __sync_synchronize();
  rtos_exit_global_critical(f);
  return next;
}

intptr_t aosl_hal_atomic_sub(intptr_t i, intptr_t *v)
{
  uint32_t f = rtos_enter_global_critical();
  intptr_t next = *(volatile intptr_t *)v - i;
  *(volatile intptr_t *)v = next;
  __sync_synchronize();
  rtos_exit_global_critical(f);
  return next;
}

intptr_t aosl_hal_atomic_cmpxchg(intptr_t *v, intptr_t old, intptr_t new)
{
  uint32_t f = rtos_enter_global_critical();
  intptr_t cur = *(volatile intptr_t *)v;
  if (cur == old) {
    *(volatile intptr_t *)v = new;
  }
  __sync_synchronize();
  rtos_exit_global_critical(f);
  return cur;
}

intptr_t aosl_hal_atomic_xchg(intptr_t *v, intptr_t new)
{
  uint32_t f = rtos_enter_global_critical();
  intptr_t prev = *(volatile intptr_t *)v;
  *(volatile intptr_t *)v = new;
  __sync_synchronize();
  rtos_exit_global_critical(f);
  return prev;
}

void aosl_hal_mb(void)
{
  __sync_synchronize();
}

void aosl_hal_rmb(void)
{
  __sync_synchronize();
}

void aosl_hal_wmb(void)
{
  __sync_synchronize();
}
