/* SPDX-License-Identifier: Apache-2.0 */
/* Test the actual registered CLI callbacks without UART or a motor worker. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../ap/bk7259_gimbal_cli.c"

static const struct cli_command *registered;
static int registered_count, register_calls, register_result = -4;
static int request_result, raw_calls, scene_calls, charge_calls, probe_calls, dialogue_calls;
static bk7259_gimbal_scene_t requested_scene;
static bk7259_gimbal_phase_t requested_phase;
static bool requested_charging;
static bool requested_dialogue;

int cli_register_commands(const struct cli_command *commands, int count)
{
    ++register_calls;
    if (register_result == 0) {
        registered = commands;
        registered_count = count;
    }
    return register_result;
}

int bk7259_gimbal_send_next_command(void)
{
    ++raw_calls;
    return request_result;
}

int bk7259_gimbal_request(bk7259_gimbal_scene_t scene)
{
    ++scene_calls;
    requested_scene = scene;
    return request_result;
}

int bk7259_gimbal_update_dialogue(bool active, bk7259_gimbal_phase_t phase)
{
    ++dialogue_calls;
    requested_dialogue = active;
    requested_phase = phase;
    return request_result;
}

int bk7259_gimbal_set_charging(bool enabled)
{
    ++charge_calls;
    requested_charging = enabled;
    return request_result;
}

int bk7259_gimbal_request_probe(void)
{
    ++probe_calls;
    return request_result;
}

static const struct cli_command *find_command(const char *name)
{
    for (int index = 0; index < registered_count; ++index)
        if (strcmp(registered[index].name, name) == 0)
            return &registered[index];
    assert(!"missing registered command");
    return NULL;
}

static void invoke(const char *name, const char *argument, const char *extra,
                   const char *expected_reply)
{
    char response[192] = {0};
    char *arguments[] = {(char *)name, (char *)argument, (char *)extra};
    int argc = extra ? 3 : argument ? 2 : 1;
    find_command(name)->function(response, sizeof(response), argc, arguments);
    assert(strstr(response, expected_reply));
}

static void expect_scene(const char *argument, int result,
                         bk7259_gimbal_scene_t expected)
{
    const int before = scene_calls;
    request_result = result;
    const char *reply = result == 0 ? "queued" : "unavailable";
    if (result == BK7259_GIMBAL_NOT_READY) {
        reply = "not ready";
    } else if (result == BK7259_GIMBAL_MOTION_DISABLED) {
        reply = "disabled by settings";
    }
    invoke("gimbal_scene", argument, NULL, reply);
    assert(scene_calls == before + 1 && requested_scene == expected);
}

int main(void)
{
    assert(bk7259_gimbal_cli_init() == -4 && register_calls == 1);
    register_result = 0;
    assert(bk7259_gimbal_cli_init() == 0 && register_calls == 2);
    assert(bk7259_gimbal_cli_init() == 0 && register_calls == 2);
    assert(registered_count == 5);

    invoke("gimbal_cmd", "happy", NULL, "Usage:");
    assert(raw_calls == 0 && scene_calls == 0);
    const int results[] = {0, BK7259_GIMBAL_NOT_READY, BK7259_GIMBAL_MOTION_DISABLED, -1};
    const char *const replies[] = {"queued", "not ready", "disabled by settings", "unavailable"};
    for (size_t index = 0; index < sizeof(results) / sizeof(results[0]); ++index) {
        request_result = results[index];
        invoke("gimbal_cmd", NULL, NULL, replies[index]);
        assert(raw_calls == (int)index + 1 && scene_calls == 0);
    }
    expect_scene(NULL, BK7259_GIMBAL_NOT_READY, BK7259_GIMBAL_SCENE_HAPPY);
    expect_scene(NULL, BK7259_GIMBAL_MOTION_DISABLED, BK7259_GIMBAL_SCENE_HAPPY);
    expect_scene(NULL, -1, BK7259_GIMBAL_SCENE_HAPPY);
    invoke("gimbal_scene", "Happy", NULL, "Usage:");
    invoke("gimbal_scene", "happy", "sad", "Usage:");
    assert(scene_calls == 3);
    for (int index = 0; index < 5; ++index)
        expect_scene(NULL, 0, (bk7259_gimbal_scene_t)(index % 4));

    expect_scene("angry", BK7259_GIMBAL_NOT_READY, BK7259_GIMBAL_SCENE_ANGRY);
    expect_scene(NULL, 0, BK7259_GIMBAL_SCENE_SAD);
    expect_scene("surprised", 0, BK7259_GIMBAL_SCENE_SURPRISED);
    expect_scene("sad", -1, BK7259_GIMBAL_SCENE_SAD);
    expect_scene(NULL, 0, BK7259_GIMBAL_SCENE_HAPPY);
    expect_scene("happy", 0, BK7259_GIMBAL_SCENE_HAPPY);
    expect_scene("sad", 0, BK7259_GIMBAL_SCENE_SAD);
    expect_scene("angry", 0, BK7259_GIMBAL_SCENE_ANGRY);
    expect_scene(NULL, 0, BK7259_GIMBAL_SCENE_SURPRISED);
    assert(raw_calls == 4);

    invoke("gimbal_probe", "extra", NULL, "Usage:");
    assert(probe_calls == 0);
    const int probe_results[] = {0, -2, -1};
    const char *const probe_replies[] = {"queued", "busy", "unavailable"};
    for (int index = 0; index < 3; ++index) {
        request_result = probe_results[index];
        invoke("gimbal_probe", NULL, NULL, probe_replies[index]);
        assert(probe_calls == index + 1);
    }

    invoke("gimbal_dialogue", NULL, NULL, "Usage:");
    invoke("gimbal_dialogue", "Listening", NULL, "Usage:");
    invoke("gimbal_dialogue", "start", "end", "Usage:");
    assert(dialogue_calls == 0);
    const struct {
        const char *argument;
        bool active;
        bk7259_gimbal_phase_t phase;
    } dialogue[] = {
        {"start", true, BK7259_GIMBAL_PHASE_NEUTRAL},
        {"neutral", true, BK7259_GIMBAL_PHASE_NEUTRAL},
        {"listening", true, BK7259_GIMBAL_PHASE_LISTENING},
        {"thinking", true, BK7259_GIMBAL_PHASE_THINKING},
        {"speaking", true, BK7259_GIMBAL_PHASE_SPEAKING},
        {"end", false, BK7259_GIMBAL_PHASE_NEUTRAL},
    };
    request_result = 0;
    for (size_t i = 0; i < sizeof(dialogue) / sizeof(dialogue[0]); ++i) {
        invoke("gimbal_dialogue", dialogue[i].argument, NULL, "queued");
        assert(dialogue_calls == (int)i + 1);
        assert(requested_dialogue == dialogue[i].active && requested_phase == dialogue[i].phase);
    }
    request_result = BK7259_GIMBAL_NOT_READY;
    invoke("gimbal_dialogue", "start", NULL, "not ready");
    request_result = BK7259_GIMBAL_MOTION_DISABLED;
    invoke("gimbal_dialogue", "thinking", NULL, "disabled by settings");
    assert(requested_dialogue && requested_phase == BK7259_GIMBAL_PHASE_THINKING);
    invoke("gimbal_dialogue", "end", NULL, "disabled by settings");
    assert(!requested_dialogue);
    request_result = -1;
    invoke("gimbal_dialogue", "end", NULL, "unavailable");

    invoke("gimbal_charge", NULL, NULL, "Usage:");
    invoke("gimbal_charge", "ON", NULL, "Usage:");
    invoke("gimbal_charge", "on", "off", "Usage:");
    assert(charge_calls == 0);
    request_result = 0;
    invoke("gimbal_charge", "on", NULL, "queued");
    assert(charge_calls == 1 && requested_charging);
    invoke("gimbal_charge", "off", NULL, "queued");
    assert(charge_calls == 2 && !requested_charging);
    request_result = -1;
    invoke("gimbal_charge", "on", NULL, "unavailable");
    assert(charge_calls == 3 && requested_charging);

    puts("PASS: CLI registration, arguments, scene faults, UART probes, and charging control");
    return 0;
}
