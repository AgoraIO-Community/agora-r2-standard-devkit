/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BK7259_GIMBAL_H_
#define BK7259_GIMBAL_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BK7259_GIMBAL_SCENE_HAPPY = 0,
    BK7259_GIMBAL_SCENE_SAD,
    BK7259_GIMBAL_SCENE_ANGRY,
    BK7259_GIMBAL_SCENE_SURPRISED,
    BK7259_GIMBAL_SCENE_COUNT,
} bk7259_gimbal_scene_t;

#if defined(CONFIG_MYBOT_GIMBAL) && CONFIG_MYBOT_GIMBAL
/* Product-owned UART2 worker. Preparation does not wait for motor homing. */
int bk7259_gimbal_prepare(void);
/* Short mailbox operations; never call LCD or wait for physical movement.
 * false invalidates unsent scenes and schedules centering; repeated values
 * are a no-op. A new scene replaces the unsent part after the current move.
 * After a CLI command, manual control only records active and never moves. */
int bk7259_gimbal_set_conversation(bool active);
/* Each call queues the next of four fixed UART commands, wrapping after four.
 * 0 queued, -2 busy (conversation or UART work), -1 unavailable. A queued
 * command claims manual control until preparation after platform shutdown:
 * MCP actions and automatic centering remain suspended; explicit CLI scene
 * requests may execute their own status queries and complete trajectory.
 * Each accepted call advances once, including when its ACK later fails.
 * The worker prints TX/RX and never retries; failure permits another call. */
int bk7259_gimbal_send_next_command(void);
/* Queue one complete existing trajectory without starting a conversation.
 * Shares the UART owner/manual session with send_next_command(). Returns
 * 0 queued, -2 busy, -1 invalid/unavailable. Each run validates fresh idle
 * position feedback before moving and returns to center on success. */
int bk7259_gimbal_request_cli_scene(bk7259_gimbal_scene_t scene);
/* Requests are unavailable while manual control owns the gimbal. */
int bk7259_gimbal_request(bk7259_gimbal_scene_t scene);
/* Cancel scenes and wait a bounded time for centering, for reset/reboot.
 * Returns -1 after manual control; shutdown stops without extra movement. */
int bk7259_gimbal_center_wait(uint32_t timeout_ms);
/* A failed worker-exit wait retains UART and its resources for a later retry. */
int bk7259_gimbal_shutdown(void);
#else
static inline int bk7259_gimbal_prepare(void) { return 0; }
static inline int bk7259_gimbal_set_conversation(bool active)
{ (void)active; return 0; }
static inline int bk7259_gimbal_send_next_command(void) { return -1; }
static inline int bk7259_gimbal_request_cli_scene(bk7259_gimbal_scene_t scene)
{ (void)scene; return -1; }
static inline int bk7259_gimbal_request(bk7259_gimbal_scene_t scene)
{ (void)scene; return 0; }
static inline int bk7259_gimbal_center_wait(uint32_t timeout_ms)
{ (void)timeout_ms; return 0; }
static inline int bk7259_gimbal_shutdown(void) { return 0; }
#endif

#ifdef __cplusplus
}
#endif
#endif /* BK7259_GIMBAL_H_ */
