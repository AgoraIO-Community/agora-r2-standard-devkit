/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

struct cli_command {
    const char *name;
    const char *help;
    void (*function)(char *buffer, int length, int argc, char **argv);
};

int cli_register_commands(const struct cli_command *commands, int count);
