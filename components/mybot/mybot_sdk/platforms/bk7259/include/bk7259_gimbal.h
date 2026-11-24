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

typedef enum {
    BK7259_GIMBAL_PHASE_NEUTRAL = 0,
    BK7259_GIMBAL_PHASE_LISTENING,
    BK7259_GIMBAL_PHASE_THINKING,
    BK7259_GIMBAL_PHASE_SPEAKING,
} bk7259_gimbal_phase_t;

enum { BK7259_GIMBAL_NOT_READY = -4 };

#if defined(CONFIG_MYBOT_GIMBAL) && CONFIG_MYBOT_GIMBAL
/* Product-owned UART2 worker. Preparation does not wait for motor homing. */
int bk7259_gimbal_prepare(void);
/* Publish dialogue activity and phase. Duplicate phases preserve timing.
 * While centering, active updates retain the phase and return NOT_READY. */
int bk7259_gimbal_update_dialogue(bool active, bk7259_gimbal_phase_t phase);
/* Queue the next of four absolute targets or a scene, replacing older motion.
 * Returns 0 queued, -1 unavailable, or BK7259_GIMBAL_NOT_READY while centering. */
int bk7259_gimbal_send_next_command(void);
int bk7259_gimbal_request(bk7259_gimbal_scene_t scene);
/* Queue a link diagnostic. Returns 0 queued, -2 pending, or -1 unavailable. */
int bk7259_gimbal_request_probe(void);
/* Cancel motion and wait for confirmed center. */
int bk7259_gimbal_center_wait(uint32_t timeout_ms);
/* Queue a charging policy change. The worker starts and stops the heartbeat. */
int bk7259_gimbal_set_charging(bool enabled);
/* Disable charging for product shutdown and wait for an explicit stop ACK
 * after any enable attempt. Enabling is blocked until platform preparation. */
int bk7259_gimbal_stop_charging_wait(uint32_t timeout_ms);
/* A failed worker-exit wait retains UART and its resources for a later retry. */
int bk7259_gimbal_shutdown(void);
#else
static inline int bk7259_gimbal_prepare(void) { return 0; }
static inline int bk7259_gimbal_update_dialogue(bool active, bk7259_gimbal_phase_t phase)
{ (void)active; (void)phase; return 0; }
static inline int bk7259_gimbal_send_next_command(void) { return -1; }
static inline int bk7259_gimbal_request_probe(void) { return -1; }
static inline int bk7259_gimbal_request(bk7259_gimbal_scene_t scene)
{ (void)scene; return 0; }
static inline int bk7259_gimbal_center_wait(uint32_t timeout_ms)
{ (void)timeout_ms; return 0; }
static inline int bk7259_gimbal_set_charging(bool enabled)
{ (void)enabled; return -1; }
static inline int bk7259_gimbal_stop_charging_wait(uint32_t timeout_ms)
{ (void)timeout_ms; return 0; }
static inline int bk7259_gimbal_shutdown(void) { return 0; }
#endif

#ifdef __cplusplus
}
#endif
#endif /* BK7259_GIMBAL_H_ */
