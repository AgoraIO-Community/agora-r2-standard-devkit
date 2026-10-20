/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdatomic.h>
#include <stdint.h>
typedef _Atomic(intptr_t) aosl_atomic_t;
static inline intptr_t aosl_atomic_read(const aosl_atomic_t *value) {
  return atomic_load(value);
}
static inline void aosl_atomic_set(aosl_atomic_t *value, intptr_t next) {
  atomic_store(value, next);
}
static inline intptr_t aosl_atomic_xchg(aosl_atomic_t *value, intptr_t next) {
  return atomic_exchange(value, next);
}
