#!/usr/bin/env bash
set -euo pipefail
test_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
sdk_dir="$test_dir/../../../../components/mybot/mybot_sdk"
test_binary=$(mktemp /tmp/bk7259-gimbal-test.XXXXXX)
cli_binary=$(mktemp /tmp/bk7259-gimbal-cli-test.XXXXXX)
trap 'rm -f -- "$test_binary" "$cli_binary"' EXIT
extra_flags=()
if [[ "${GIMBAL_TEST_SANITIZERS:-0}" == 1 ]]; then
    extra_flags=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
"${CC:-cc}" -std=c99 -D_POSIX_C_SOURCE=200809L -DCONFIG_MYBOT_GIMBAL=1 \
    -Wall -Wextra -Werror -Wno-unused-parameter -pthread \
    "${extra_flags[@]}" \
    -I"$test_dir/stubs" -I"$sdk_dir/platforms/bk7259/include" \
    "$test_dir/test_gimbal.c" -lm -o "$test_binary"
timeout 60s "$test_binary"
"${CC:-cc}" -std=c99 -DCONFIG_CLI=1 -DCONFIG_MYBOT_GIMBAL=1 \
    -Wall -Wextra -Werror "${extra_flags[@]}" \
    -I"$test_dir/stubs" -I"$sdk_dir/platforms/bk7259/include" \
    "$test_dir/test_cli.c" -o "$cli_binary"
timeout 5s "$cli_binary"
