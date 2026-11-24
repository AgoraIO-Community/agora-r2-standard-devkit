/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BK7259_KEY_H_
#define BK7259_KEY_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The application decides start/stop from the SDK's public state, then the
 * key adapter delivers the corresponding event through its SDK callback. */
bool bk7259_key_take_conversation_request(void);
bool bk7259_key_dispatch_conversation(bool start);

typedef enum {
    BK7259_TOUCH_CONVERSATION_DISABLED = 0,
    BK7259_TOUCH_CONVERSATION_START,
    BK7259_TOUCH_CONVERSATION_STOP,
} bk7259_touch_conversation_t;

/* UI posts the intent associated with the touched page. The AP accepts it
 * only while that operation is available in the SDK's authoritative state. */
bool bk7259_key_request_touch_conversation(bool start);
void bk7259_key_allow_touch_conversation(bk7259_touch_conversation_t allowed);
bk7259_touch_conversation_t bk7259_key_take_touch_conversation(void);

#ifdef __cplusplus
}
#endif

#endif /* BK7259_KEY_H_ */
