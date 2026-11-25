/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <assert.h>
#include <stdbool.h>

typedef bool aosl_static_lock_t;
#define AOSL_STATIC_LOCK_INIT false

static inline int aosl_static_lock_lock(aosl_static_lock_t *lock) {
  assert(!*lock);
  *lock = true;
  return 0;
}

static inline int aosl_static_lock_unlock(aosl_static_lock_t *lock) {
  assert(*lock);
  *lock = false;
  return 0;
}
