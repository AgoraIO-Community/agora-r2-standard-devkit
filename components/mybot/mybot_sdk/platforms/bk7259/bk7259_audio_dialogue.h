/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BK7259_AUDIO_DIALOGUE_H_
#define BK7259_AUDIO_DIALOGUE_H_

#include <sdkconfig.h>
#include <stdbool.h>
#include <stdint.h>

#if defined(CONFIG_MYBOT_GIMBAL) && CONFIG_MYBOT_GIMBAL
/* Audio energy from accepted mono PCM, scaled to 0–100. */
unsigned bk7259_audio_dialogue_level(void);
void bk7259_audio_dialogue_set_speaking(bool active);

/* Playback takes a generation before writing and publishes only accepted frames. */
void bk7259_audio_dialogue_set_playback_running(bool running);
uint32_t bk7259_audio_dialogue_begin_write(void);
void bk7259_audio_dialogue_accept(uint32_t generation, const void *pcm, unsigned frames);
#else
static inline unsigned bk7259_audio_dialogue_level(void) { return 0; }
static inline void bk7259_audio_dialogue_set_speaking(bool active) { (void)active; }
#endif

#endif /* BK7259_AUDIO_DIALOGUE_H_ */
