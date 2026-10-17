/* SPDX-License-Identifier: Apache-2.0 */
/* Test the actual registered CLI callbacks without UART or a motor worker. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../ap/bk7259_gimbal_cli.c"

static const struct cli_command *registered;
static int registered_count, register_calls, register_result = -4;
static int request_result, raw_calls, scene_calls;
static bk7259_gimbal_scene_t requested_scene;

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

int bk7259_gimbal_request_cli_scene(bk7259_gimbal_scene_t scene)
{
    ++scene_calls;
    requested_scene = scene;
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
    invoke("gimbal_scene", argument, NULL,
           result == 0 ? "queued" : result == -2 ? "busy" : "unavailable");
    assert(scene_calls == before + 1 && requested_scene == expected);
}

int main(void)
{
    assert(bk7259_gimbal_cli_init() == -4 && register_calls == 1);
    register_result = 0;
    assert(bk7259_gimbal_cli_init() == 0 && register_calls == 2);
    assert(bk7259_gimbal_cli_init() == 0 && register_calls == 2);
    assert(registered_count == 2);

    invoke("gimbal_cmd", "happy", NULL, "Usage:");
    assert(raw_calls == 0 && scene_calls == 0);
    const int results[] = {0, -2, -1};
    const char *const replies[] = {"queued", "busy", "unavailable"};
    for (int index = 0; index < 3; ++index) {
        request_result = results[index];
        invoke("gimbal_cmd", NULL, NULL, replies[index]);
        assert(raw_calls == index + 1 && scene_calls == 0);
    }

    expect_scene(NULL, -2, BK7259_GIMBAL_SCENE_HAPPY);
    expect_scene(NULL, -1, BK7259_GIMBAL_SCENE_HAPPY);
    invoke("gimbal_scene", "Happy", NULL, "Usage:");
    invoke("gimbal_scene", "happy", "sad", "Usage:");
    assert(scene_calls == 2);
    for (int index = 0; index < 5; ++index)
        expect_scene(NULL, 0, (bk7259_gimbal_scene_t)(index % 4));

    expect_scene("angry", -2, BK7259_GIMBAL_SCENE_ANGRY);
    expect_scene(NULL, 0, BK7259_GIMBAL_SCENE_SAD);
    expect_scene("surprised", 0, BK7259_GIMBAL_SCENE_SURPRISED);
    expect_scene("sad", -1, BK7259_GIMBAL_SCENE_SAD);
    expect_scene(NULL, 0, BK7259_GIMBAL_SCENE_HAPPY);
    expect_scene("happy", 0, BK7259_GIMBAL_SCENE_HAPPY);
    expect_scene("sad", 0, BK7259_GIMBAL_SCENE_SAD);
    expect_scene("angry", 0, BK7259_GIMBAL_SCENE_ANGRY);
    expect_scene(NULL, 0, BK7259_GIMBAL_SCENE_SURPRISED);
    assert(raw_calls == 3);

    puts("PASS: CLI registration, arguments, scene order and admission cursor");
    return 0;
}
