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

static void gimbal_command(char *buffer, int length, int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        command_reply(buffer, length, "Usage: gimbal_cmd (send the next one of four commands)");
        return;
    }
    /* The UART owner sends and receives asynchronously; the CLI never does I/O. */
    int result = bk7259_gimbal_send_next_command();
    if (result == 0)
        command_reply(buffer, length, "gimbal_cmd queued; TX/RX follow in bk7259_gimbal logs");
    else if (result == -2)
        command_reply(buffer, length, "gimbal_cmd busy; retry after the current command or conversation");
    else
        command_reply(buffer, length, "gimbal_cmd unavailable; UART worker is not running");
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

    int result = bk7259_gimbal_request_cli_scene(scene);
    if (result == 0) {
        s_next_scene = (bk7259_gimbal_scene_t)(((unsigned)scene + 1U) % BK7259_GIMBAL_SCENE_COUNT);
        command_reply(buffer, length, "gimbal_scene queued; one full scene and return to center; results follow in bk7259_gimbal logs");
    } else if (result == -2) {
        command_reply(buffer, length, "gimbal_scene busy; retry after the current command, scene or conversation");
    } else {
        command_reply(buffer, length, "gimbal_scene unavailable; UART worker is not running");
    }
}

static const struct cli_command k_commands[] = {
    {"gimbal_cmd", "gimbal_cmd: send one command, cycling through four motions", gimbal_command},
    {"gimbal_scene", "gimbal_scene [happy|sad|angry|surprised]: play one full scene, then center", gimbal_scene_command},
};

int bk7259_gimbal_cli_init(void)
{
    /* Product startup serializes registration; the command table lives forever. */
    if (s_registered)
        return 0;
    int result = cli_register_commands(k_commands, sizeof(k_commands) / sizeof(k_commands[0]));
    if (result == 0) {
        s_registered = true;
        BK_LOGI(TAG, "ready: ap_cmd gimbal_cmd sends one command per call\r\n");
        BK_LOGI(TAG, "ready: ap_cmd gimbal_scene [happy|sad|angry|surprised] plays one full scene per call\r\n");
    }
    return result;
}
#else
int bk7259_gimbal_cli_init(void)
{
    return 0;
}
#endif
