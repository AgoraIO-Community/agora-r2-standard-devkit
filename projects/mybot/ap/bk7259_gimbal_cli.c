/* SPDX-License-Identifier: Apache-2.0 */
#include "bk7259_gimbal_cli.h"
#include <sdkconfig.h>

#if defined(CONFIG_CLI) && CONFIG_CLI && \
    defined(CONFIG_MYBOT_GIMBAL) && CONFIG_MYBOT_GIMBAL
#include <bk_private/bk_cli.h>
#include <bk7259_gimbal.h>
#include <components/log.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define TAG "gimbal_cmd"

static bool s_registered;
/* AP CLI callbacks are serialized by the shell. Advance only after admission. */
static bk7259_gimbal_scene_t s_next_scene = BK7259_GIMBAL_SCENE_HAPPY;
static const char *const k_scene_names[] = {"happy", "sad", "angry", "surprised"};

static void command_reply(char *buffer, int length, const char *message)
{
    if (buffer != NULL && length > 0)
        (void)snprintf(buffer, (size_t)length, "%s\r\n", message);
    else
        BK_LOGI(TAG, "%s\r\n", message);
}

static void motion_reply(char *buffer, int length, int result)
{
    if (result == 0)
        command_reply(buffer, length, "gimbal motion queued");
    else if (result == BK7259_GIMBAL_NOT_READY)
        command_reply(buffer, length, "gimbal not ready: waiting for center confirmation");
    else
        command_reply(buffer, length, "gimbal unavailable");
}

static void gimbal_command(char *buffer, int length, int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        command_reply(buffer, length, "Usage: gimbal_cmd (send the next one of four commands)");
        return;
    }
    motion_reply(buffer, length, bk7259_gimbal_send_next_command());
}

static void gimbal_scene_command(char *buffer, int length, int argc, char **argv)
{
    bk7259_gimbal_scene_t scene = s_next_scene;
    if (argc == 2) {
        size_t index = 0;
        for (; index < sizeof(k_scene_names) / sizeof(k_scene_names[0]); ++index)
            if (strcmp(argv[1], k_scene_names[index]) == 0)
                break;
        if (index == sizeof(k_scene_names) / sizeof(k_scene_names[0])) {
            command_reply(buffer, length, "Usage: gimbal_scene [happy|sad|angry|surprised]");
            return;
        }
        scene = (bk7259_gimbal_scene_t)index;
    } else if (argc != 1) {
        command_reply(buffer, length, "Usage: gimbal_scene [happy|sad|angry|surprised]");
        return;
    }

    int result = bk7259_gimbal_request(scene);
    if (result == 0)
        s_next_scene = (bk7259_gimbal_scene_t)(((unsigned)scene + 1U) % BK7259_GIMBAL_SCENE_COUNT);
    motion_reply(buffer, length, result);
}

static void gimbal_dialogue_command(char *buffer, int length, int argc, char **argv)
{
    static const struct {
        const char *name;
        bool active;
        bk7259_gimbal_phase_t phase;
    } events[] = {
        {"start", true, BK7259_GIMBAL_PHASE_NEUTRAL},
        {"listening", true, BK7259_GIMBAL_PHASE_LISTENING},
        {"thinking", true, BK7259_GIMBAL_PHASE_THINKING},
        {"speaking", true, BK7259_GIMBAL_PHASE_SPEAKING},
        {"neutral", true, BK7259_GIMBAL_PHASE_NEUTRAL},
        {"end", false, BK7259_GIMBAL_PHASE_NEUTRAL},
    };
    if (argc == 2) {
        for (size_t i = 0; i < sizeof(events) / sizeof(events[0]); ++i) {
            if (strcmp(argv[1], events[i].name) == 0) {
                motion_reply(buffer, length,
                             bk7259_gimbal_update_dialogue(events[i].active, events[i].phase));
                return;
            }
        }
    }
    command_reply(buffer, length, "Usage: gimbal_dialogue start|listening|thinking|speaking|neutral|end");
}

static void gimbal_probe_command(char *buffer, int length, int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        command_reply(buffer, length, "Usage: gimbal_probe");
        return;
    }
    int result = bk7259_gimbal_request_probe();
    if (result == 0)
        command_reply(buffer, length, "gimbal_probe queued; check the UART result in bk7259_gimbal logs");
    else if (result == -2)
        command_reply(buffer, length, "gimbal_probe busy");
    else
        command_reply(buffer, length, "gimbal_probe unavailable; UART worker is not running or is shutting down");
}

static void gimbal_charge_command(char *buffer, int length, int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0)) {
        command_reply(buffer, length, "Usage: gimbal_charge on|off");
        return;
    }

    bool enabled = strcmp(argv[1], "on") == 0;
    if (bk7259_gimbal_set_charging(enabled) == 0)
        command_reply(buffer, length, enabled ? "gimbal_charge on queued" : "gimbal_charge off queued");
    else
        command_reply(buffer, length, "gimbal_charge unavailable; check UART readiness and pending operations");
}

static const struct cli_command k_commands[] = {
    {"gimbal_cmd", "gimbal_cmd: send one command, cycling through four motions", gimbal_command},
    {"gimbal_scene", "gimbal_scene [happy|sad|angry|surprised]: play one full scene, then center", gimbal_scene_command},
    {"gimbal_dialogue", "gimbal_dialogue start|listening|thinking|speaking|neutral|end", gimbal_dialogue_command},
    {"gimbal_charge", "gimbal_charge on|off: control EB521 charging heartbeat", gimbal_charge_command},
    {"gimbal_probe", "gimbal_probe: check the EB521 UART link", gimbal_probe_command},
};

int bk7259_gimbal_cli_init(void)
{
    /* Product startup serializes registration; the command table lives forever. */
    if (s_registered)
        return 0;
    int result = cli_register_commands(k_commands, sizeof(k_commands) / sizeof(k_commands[0]));
    if (result == 0) {
        s_registered = true;
        BK_LOGI(TAG, "EB521 V1.2 commands: gimbal_dialogue, gimbal_scene, gimbal_cmd, gimbal_probe, gimbal_charge\r\n");
    }
    return result;
}
#else
int bk7259_gimbal_cli_init(void)
{
    return 0;
}
#endif
