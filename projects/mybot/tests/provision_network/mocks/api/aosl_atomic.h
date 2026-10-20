/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdatomic.h>
#include <stdint.h>
typedef _Atomic(intptr_t) aosl_atomic_t;
static inline intptr_t aosl_atomic_read(const aosl_atomic_t *value) {
  return atomic_load(value);
}
static inline intptr_t aosl_atomic_cmpxchg(aosl_atomic_t *value, intptr_t old,
                                           intptr_t next) {
  atomic_compare_exchange_strong(value, &old, next);
  return old;
}
