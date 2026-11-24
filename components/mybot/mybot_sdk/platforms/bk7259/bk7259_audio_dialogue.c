/* SPDX-License-Identifier: Apache-2.0 */
#include "bk7259_audio_dialogue.h"

#include <os/os.h>
#include <string.h>

/* The R2 playback callback normally submits up to 60 ms at 16 kHz. */
#define ANALYSIS_FRAMES 960U
#define SILENCE_MAGNITUDE 160U
#define FULL_LEVEL_MAGNITUDE 8192U
#define LEVEL_STALE_MS 160U

/* All fields share the BK SMP critical section. Sample analysis runs outside it. */
static struct {
    uint32_t generation;
    uint32_t updated_at;
    unsigned level;
    bool speaking;
    bool playback_running;
} s_dialogue;

static void set_active(bool *current, bool active)
{
    uint32_t flags = rtos_enter_global_critical();
    if (*current != active) {
        *current = active;
        if (++s_dialogue.generation == 0) {
            ++s_dialogue.generation;
        }
        s_dialogue.level = 0;
    }
    rtos_exit_global_critical(flags);
}

void bk7259_audio_dialogue_set_speaking(bool active)
{
    set_active(&s_dialogue.speaking, active);
}

void bk7259_audio_dialogue_set_playback_running(bool running)
{
    set_active(&s_dialogue.playback_running, running);
}

uint32_t bk7259_audio_dialogue_begin_write(void)
{
    uint32_t flags = rtos_enter_global_critical();
    uint32_t generation = s_dialogue.speaking && s_dialogue.playback_running ?
                          s_dialogue.generation : 0;
    rtos_exit_global_critical(flags);
    return generation;
}

static unsigned frame_level(const void *pcm, unsigned frames)
{
    const unsigned char *bytes = pcm;
    unsigned total = 0;
    unsigned count = frames < ANALYSIS_FRAMES ? frames : ANALYSIS_FRAMES;

    for (unsigned i = 0; i < count; ++i) {
        int16_t sample;
        memcpy(&sample, bytes + i * sizeof(sample), sizeof(sample));
        total += sample < 0 ? (unsigned)-(int)sample : (unsigned)sample;
    }
    unsigned mean = total / count;
    if (mean <= SILENCE_MAGNITUDE) {
        return 0;
    }
    if (mean >= FULL_LEVEL_MAGNITUDE) {
        return 100;
    }
    return (mean - SILENCE_MAGNITUDE) * 100U /
           (FULL_LEVEL_MAGNITUDE - SILENCE_MAGNITUDE);
}

void bk7259_audio_dialogue_accept(uint32_t generation, const void *pcm, unsigned frames)
{
    if (!generation || !frames) {
        return;
    }
    unsigned level = frame_level(pcm, frames);
    uint32_t now = rtos_get_time();
    uint32_t flags = rtos_enter_global_critical();
    if (s_dialogue.generation == generation) {
        unsigned previous = now - s_dialogue.updated_at < LEVEL_STALE_MS ? s_dialogue.level : 0;
        /* Clear immediately on silence and favor a quick rise for speech. */
        if (!level) {
            s_dialogue.level = 0;
        } else if (level >= previous) {
            s_dialogue.level = (3U * level + previous + 3U) / 4U;
        } else {
            s_dialogue.level = (3U * previous + level) / 4U;
        }
        s_dialogue.updated_at = now;
    }
    rtos_exit_global_critical(flags);
}

unsigned bk7259_audio_dialogue_level(void)
{
    uint32_t flags = rtos_enter_global_critical();
    uint32_t updated_at = s_dialogue.updated_at;
    unsigned level = s_dialogue.level;
    rtos_exit_global_critical(flags);

    return level && rtos_get_time() - updated_at < LEVEL_STALE_MS ? level : 0;
}
