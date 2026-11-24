#!/usr/bin/env bash
set -euo pipefail

test_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
platform="$test_dir/../../../../components/mybot/mybot_sdk/platforms/bk7259"
binary=$(mktemp /tmp/mybot-audio-dialogue.XXXXXX)
trap 'rm -f -- "$binary"' EXIT

flags=(-std=c99 -Wall -Wextra -Werror -I"$test_dir/stubs" -I"$platform")
if [[ "${AUDIO_DIALOGUE_TEST_SANITIZERS:-0}" == 1 ]]; then
    flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
"${CC:-cc}" "${flags[@]}" -DCONFIG_MYBOT_GIMBAL=1 "$test_dir/test_audio_dialogue.c" \
    "$platform/bk7259_audio_dialogue.c" -o "$binary"
"$binary"
"${CC:-cc}" "${flags[@]}" -DCONFIG_MYBOT_GIMBAL=0 "$test_dir/test_audio_dialogue.c" -o "$binary"
"$binary"
