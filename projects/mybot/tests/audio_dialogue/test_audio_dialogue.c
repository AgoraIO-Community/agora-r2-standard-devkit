/* SPDX-License-Identifier: Apache-2.0 */
#include "bk7259_audio_dialogue.h"

#include <assert.h>
#include <stdio.h>

#if CONFIG_MYBOT_GIMBAL
static uint32_t now;
static bool critical;
static const int16_t loud[] = {INT16_MIN, INT16_MAX};

uint32_t rtos_get_time(void)
{
    assert(!critical);
    return now;
}

uint32_t rtos_enter_global_critical(void)
{
    assert(!critical);
    critical = true;
    return 1;
}

void rtos_exit_global_critical(uint32_t flags)
{
    assert(flags == 1 && critical);
    critical = false;
}

static void test_stale_and_phase_transitions(void)
{
    bk7259_audio_dialogue_set_playback_running(true);
    uint32_t prompt = bk7259_audio_dialogue_begin_write();
    assert(prompt == 0);
    bk7259_audio_dialogue_set_speaking(true);
    bk7259_audio_dialogue_accept(prompt, loud, 2);
    assert(bk7259_audio_dialogue_level() == 0);

    uint32_t generation = bk7259_audio_dialogue_begin_write();
    assert(generation != 0);
    now = UINT32_MAX - 40U;
    bk7259_audio_dialogue_accept(generation, loud, 2);
    assert(bk7259_audio_dialogue_level() == 75);
    now += 159U;
    assert(bk7259_audio_dialogue_level() == 75);
    ++now;
    assert(bk7259_audio_dialogue_level() == 0);
    bk7259_audio_dialogue_accept(generation, loud, 2);
    assert(bk7259_audio_dialogue_level() == 75);
    bk7259_audio_dialogue_set_speaking(true);
    assert(bk7259_audio_dialogue_level() == 75);

    bk7259_audio_dialogue_set_speaking(false);
    assert(bk7259_audio_dialogue_level() == 0);
    bk7259_audio_dialogue_set_speaking(true);
    bk7259_audio_dialogue_accept(generation, loud, 2);
    assert(bk7259_audio_dialogue_level() == 0);
    assert(bk7259_audio_dialogue_begin_write() != generation);
}

static void test_accepted_prefix_silence_and_playback_stop(void)
{
    bk7259_audio_dialogue_set_speaking(false);
    bk7259_audio_dialogue_set_playback_running(true);
    bk7259_audio_dialogue_set_speaking(true);
    uint32_t generation = bk7259_audio_dialogue_begin_write();
    const int16_t partial_write[] = {120, -120, INT16_MIN, INT16_MAX};
    bk7259_audio_dialogue_accept(generation, partial_write, 2);
    assert(bk7259_audio_dialogue_level() == 0);
    bk7259_audio_dialogue_accept(generation, partial_write, 4);
    assert(bk7259_audio_dialogue_level() == 75);
    bk7259_audio_dialogue_accept(generation, loud, 2);
    assert(bk7259_audio_dialogue_level() == 94);
    const int16_t quiet[] = {2048, -2048};
    bk7259_audio_dialogue_accept(generation, quiet, 2);
    assert(bk7259_audio_dialogue_level() > 23 && bk7259_audio_dialogue_level() < 94);
    bk7259_audio_dialogue_accept(generation, partial_write, 2);
    assert(bk7259_audio_dialogue_level() == 0);

    bk7259_audio_dialogue_accept(generation, loud, 2);
    bk7259_audio_dialogue_set_playback_running(false);
    assert(bk7259_audio_dialogue_level() == 0 && bk7259_audio_dialogue_begin_write() == 0);
    bk7259_audio_dialogue_set_playback_running(true);
    bk7259_audio_dialogue_accept(generation, loud, 2);
    assert(bk7259_audio_dialogue_level() == 0);
    bk7259_audio_dialogue_accept(bk7259_audio_dialogue_begin_write(), loud, 2);
    assert(bk7259_audio_dialogue_level() == 75);
}

static void test_analysis_window(void)
{
    bk7259_audio_dialogue_set_playback_running(true);
    bk7259_audio_dialogue_set_speaking(true);
    uint32_t generation = bk7259_audio_dialogue_begin_write();
    int16_t pcm[1920] = {0};
    for (unsigned i = 960; i < sizeof(pcm) / sizeof(pcm[0]); ++i) {
        pcm[i] = INT16_MIN;
    }
    bk7259_audio_dialogue_accept(generation, pcm, sizeof(pcm) / sizeof(pcm[0]));
    assert(bk7259_audio_dialogue_level() == 0);
}
#endif

int main(void)
{
#if CONFIG_MYBOT_GIMBAL
    test_stale_and_phase_transitions();
    test_accepted_prefix_silence_and_playback_stop();
    test_analysis_window();
    puts("Dialogue audio: accepted PCM, smoothing, expiry, phase and playback fencing passed");
#else
    bk7259_audio_dialogue_set_speaking(true);
    assert(bk7259_audio_dialogue_level() == 0);
    puts("Dialogue audio: disabled build has no tracker dependency");
#endif
    return 0;
}
