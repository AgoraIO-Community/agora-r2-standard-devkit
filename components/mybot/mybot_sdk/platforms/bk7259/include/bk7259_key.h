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
void bk7259_key_dispatch_conversation(bool start);

#ifdef __cplusplus
}
#endif

#endif /* BK7259_KEY_H_ */
