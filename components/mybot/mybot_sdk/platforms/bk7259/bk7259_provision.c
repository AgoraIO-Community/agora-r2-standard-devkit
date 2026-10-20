/* SPDX-License-Identifier: Apache-2.0 */
#include "bk7259_provision.h"
#include "bk7259_factory_reset.h"

#include <api/aosl_atomic.h>

/* One CAS word serializes GUI requests/cancellation with the network owner's
 * credential commit. No UI callback waits for the SDK, network or flash. */
enum {
    PROVISION_IDLE = 0,
    PROVISION_REQUEST,
    PROVISION_REQUEST_CANCELLED,
    PROVISION_ACTIVE,
    PROVISION_CANCELLED,
    PROVISION_COMMITTING,
};
static aosl_atomic_t s_provision;

bool bk7259_provision_request(void) {
    return !bk7259_factory_reset_requested() &&
           aosl_atomic_cmpxchg(&s_provision, PROVISION_IDLE, PROVISION_REQUEST) == PROVISION_IDLE;
}

bool bk7259_provision_take_request(void) {
    for (;;) {
        intptr_t state = aosl_atomic_read(&s_provision);
        if (state != PROVISION_REQUEST && state != PROVISION_REQUEST_CANCELLED) {
            return false;
        }
        intptr_t next = state == PROVISION_REQUEST ? PROVISION_ACTIVE : PROVISION_CANCELLED;
        if (aosl_atomic_cmpxchg(&s_provision, state, next) == state) {
            return true;
        }
    }
}

int bk7259_provision_begin(void) {
    for (;;) {
        int aborted = bk7259_provision_abort_reason();
        if (aborted) {
            return aborted;
        }
        intptr_t state = aosl_atomic_read(&s_provision);
        if (state == PROVISION_ACTIVE) {
            return 0;
        }
        if (state != PROVISION_IDLE) {
            /* Re-evaluate a racing request/cancel through the priority check. */
            if (state == PROVISION_COMMITTING) {
                return -1;
            }
            continue;
        }
        if (aosl_atomic_cmpxchg(&s_provision, PROVISION_IDLE, PROVISION_ACTIVE) == PROVISION_IDLE) {
            return 1;
        }
    }
}

bool bk7259_provision_cancel(void) {
    while (!bk7259_factory_reset_requested()) {
        intptr_t state = aosl_atomic_read(&s_provision);
        intptr_t next;
        if (state == PROVISION_REQUEST) {
            next = PROVISION_REQUEST_CANCELLED;
        } else if (state == PROVISION_ACTIVE) {
            next = PROVISION_CANCELLED;
        } else {
            return false;
        }
        if (aosl_atomic_cmpxchg(&s_provision, state, next) == state) {
            return true;
        }
    }
    return false;
}

bool bk7259_provision_busy(void) {
    return aosl_atomic_read(&s_provision) != PROVISION_IDLE;
}

int bk7259_provision_abort_reason(void) {
    if (bk7259_factory_reset_requested()) {
        return BK7259_NETWORK_CANCELLED;
    }
    intptr_t state = aosl_atomic_read(&s_provision);
    if (state == PROVISION_CANCELLED || state == PROVISION_REQUEST_CANCELLED) {
        return BK7259_PROVISION_CANCELLED;
    }
    return state == PROVISION_REQUEST ? BK7259_PROVISION_REQUESTED : 0;
}

bool bk7259_provision_try_commit(void) {
    return !bk7259_factory_reset_requested() &&
           aosl_atomic_cmpxchg(&s_provision, PROVISION_ACTIVE, PROVISION_COMMITTING) ==
               PROVISION_ACTIVE;
}

void bk7259_provision_commit_failed(void) {
    (void)aosl_atomic_cmpxchg(&s_provision, PROVISION_COMMITTING, PROVISION_ACTIVE);
}

void bk7259_provision_finish(void) {
    for (;;) {
        intptr_t state = aosl_atomic_read(&s_provision);
        if (state == PROVISION_IDLE || state == PROVISION_REQUEST ||
            state == PROVISION_REQUEST_CANCELLED) {
            return;
        }
        if (aosl_atomic_cmpxchg(&s_provision, state, PROVISION_IDLE) == state) {
            return;
        }
    }
}
