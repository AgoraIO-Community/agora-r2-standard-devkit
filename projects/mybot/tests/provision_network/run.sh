#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
test_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd -- "$test_dir/../../../.." && pwd)
platform_dir="$repo_dir/components/mybot/mybot_sdk/platforms/bk7259"
json_dir="$repo_dir/bk_avdk_smp/ap/components/json"
test_binary=$(mktemp /tmp/bk7259-provision-network.XXXXXX)
trap 'rm -f -- "$test_binary"' EXIT
flags=(-std=c11 -D_POSIX_C_SOURCE=200809L -DCONFIG_CJSON_USE=1 -Wall -Wextra -Werror -g -pthread)
if [[ ${PROVISION_NETWORK_TEST_SANITIZERS:-0} == 1 ]]; then
    flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
"${CC:-cc}" "${flags[@]}" -I"$test_dir/mocks" -I"$platform_dir" \
    -I"$platform_dir/include" -I"$repo_dir/components/mybot/mybot_sdk/include" -I"$json_dir" \
    "$test_dir/test_provision_network.c" "$test_dir/test_env.c" "$json_dir/cJSON.c" \
    -lm -o "$test_binary"
timeout 15s "$test_binary"
