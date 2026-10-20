/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BK7259_FACTORY_RESET_H_
#define BK7259_FACTORY_RESET_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A confirmed UI action posts a coalesced request without blocking, even before
 * the SDK attaches. Network setup observes without consuming; only the product
 * application task consumes. Posting never stops the SDK or writes flash. */
void bk7259_factory_reset_request(void);
bool bk7259_factory_reset_requested(void);
bool bk7259_factory_reset_take_request(void);

/* Network setup yields to reset and attempts teardown before returning this
 * cancellation. A failed driver stop retains ownership; the product owner must
 * retry disconnect_network() successfully before clearing persistent records. */
#define BK7259_NETWORK_CANCELLED (-2)

/* pending: 1=resume durable reset, 0=no intent, -1=invalid/unreadable intent.
 * begin persists intent. clear verifies removal of the three product records
 * and then intent; call it only after SDK/provisioning writers have stopped. */
int bk7259_factory_reset_pending(void);
int bk7259_factory_reset_begin(void);
int bk7259_factory_reset_clear(void);
int bk7259_factory_reset_disconnect_network(void);

typedef enum {
    BK7259_FACTORY_RESET_RESTORING = 0,
    BK7259_FACTORY_RESET_FAILED,
    BK7259_FACTORY_RESET_REBOOTING,
} bk7259_factory_reset_screen_t;

/* Publish a copied product-owned status to the existing display worker. */
int bk7259_lcd_show_factory_reset(bk7259_factory_reset_screen_t screen);

#ifdef __cplusplus
}
#endif

#endif /* BK7259_FACTORY_RESET_H_ */
