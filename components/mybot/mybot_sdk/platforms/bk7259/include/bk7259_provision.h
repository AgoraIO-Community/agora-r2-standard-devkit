/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BK7259_PROVISION_H_
#define BK7259_PROVISION_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Product requests; UI callbacks only post. The application owns SDK stop,
 * network setup and restart. Factory reset always takes precedence. */
bool bk7259_provision_request(void);
/* Claim a pending request, preserving a cancellation already posted for it. */
bool bk7259_provision_take_request(void);
/* 1: begin an automatic portal session; 0: owner already claimed a session;
 * negative: reset, cancellation, or a pending explicit request takes priority. */
int bk7259_provision_begin(void);
/* Accept cancellation only before credential commit. False leaves state alone. */
bool bk7259_provision_cancel(void);
bool bk7259_provision_busy(void);

#define BK7259_PROVISION_CANCELLED (-3)
#define BK7259_PROVISION_REQUESTED (-4)
/* 0 or one of the above; factory reset returns BK7259_NETWORK_CANCELLED. */
int bk7259_provision_abort_reason(void);
/* Serialize cancellation with credential commit. Once claimed, a successful
 * network change completes instead of accepting a misleading late cancel. */
bool bk7259_provision_try_commit(void);
void bk7259_provision_commit_failed(void);
/* Finish the owner's current session; do not erase an unclaimed new request. */
void bk7259_provision_finish(void);

typedef enum {
    BK7259_PROVISION_NONE = 0,
    BK7259_PROVISION_PREPARING,
    BK7259_PROVISION_ACTIVE,
    BK7259_PROVISION_CANCELLING,
    BK7259_PROVISION_RECONNECTING,
    BK7259_PROVISION_FAILED,
    BK7259_PROVISION_FINISHING,
} bk7259_provision_screen_t;

/* Copied presentation snapshot, safe from the product/network owner. NONE
 * restores the latest SDK/workflow content. Reset retains priority. */
int bk7259_lcd_show_provision(bk7259_provision_screen_t screen);

/* Connect only a valid saved network; never fall back into a new portal.
 * Returns 0 when IPv4 is ready, -1 for no usable saved network, or an abort. */
int bk7259_wifi_restore_saved(const char *device_id);

#ifdef __cplusplus
}
#endif
#endif /* BK7259_PROVISION_H_ */
