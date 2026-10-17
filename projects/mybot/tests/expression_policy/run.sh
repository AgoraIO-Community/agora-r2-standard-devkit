#!/usr/bin/env bash
set -euo pipefail
test_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
sdk_dir="$test_dir/../../../../components/mybot/mybot_sdk"
test_binary=$(mktemp /tmp/bk7259-expression-policy-test.XXXXXX)
trap 'rm -f -- "$test_binary"' EXIT
extra_flags=()
if [[ "${EXPRESSION_POLICY_TEST_SANITIZERS:-0}" == 1 ]]; then
    extra_flags=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -pedantic "${extra_flags[@]}" \
    -I"$sdk_dir/include" -I"$sdk_dir/platforms/bk7259/include" \
    -I"$sdk_dir/platforms/bk7259/display" \
    "$test_dir/test_expression_policy.c" -o "$test_binary"
timeout 5s "$test_binary"
