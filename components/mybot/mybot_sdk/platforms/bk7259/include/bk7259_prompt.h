/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_BK7259_PROMPT_H_
#define MYBOT_BK7259_PROMPT_H_

/* Play the local provisioning prompts synchronously.  These functions are
 * intended for the product AP control task while the SDK media pipeline is
 * stopped; they return after the temporary speaker pipeline is destroyed. */
int bk7259_prompt_play_provisioning(void);
int bk7259_prompt_play_success(void);

#endif /* MYBOT_BK7259_PROMPT_H_ */
