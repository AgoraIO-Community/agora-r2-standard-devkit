/* SPDX-License-Identifier: Apache-2.0 */
#include "bk7259_expression.h"

#include <stddef.h>

static const char *const s_expression_names[BK7259_EXPRESSION_COUNT] = {
    "NEUTRAL", "GREETING", "HAPPY", "EXCITED", "CURIOUS", "CONFUSED",
    "THINKING", "LISTENING", "SURPRISED", "SHY", "PROUD", "LOVE",
    "SLEEPY", "BORED", "SAD", "PLEADING", "APOLOGY", "WORRIED",
    "ANNOYED", "REFUSE", "PROCESSING", "ERROR", "SLEEP",
};

const char *bk7259_expression_name(bk7259_expression_t expression) {
    return expression >= BK7259_EXPRESSION_NEUTRAL && expression < BK7259_EXPRESSION_COUNT
               ? s_expression_names[expression]
               : NULL;
}

static unsigned char ascii_upper(unsigned char value) {
    return value >= 'a' && value <= 'z' ? (unsigned char)(value - ('a' - 'A')) : value;
}

int bk7259_expression_from_name(const char *name, bk7259_expression_t *expression) {
    if (!name || !expression) {
        return -1;
    }
    for (unsigned int i = 0; i < BK7259_EXPRESSION_COUNT; ++i) {
        const char *expected = s_expression_names[i];
        size_t j = 0;
        while (expected[j] && ascii_upper((unsigned char)name[j]) == (unsigned char)expected[j]) {
            ++j;
        }
        if (!expected[j] && !name[j]) {
            *expression = (bk7259_expression_t)i;
            return 0;
        }
    }
    return -1;
}
