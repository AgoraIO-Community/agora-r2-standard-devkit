#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
test_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd -- "$test_dir/../../../.." && pwd)
platform_dir="$repo_dir/components/mybot/mybot_sdk/platforms/bk7259"
easyflash_dir="$repo_dir/bk_avdk_smp/ap/components/easy_flash/easy_flash_V4.X/inc"
test_binary=$(mktemp /tmp/bk7259-provision-owner.XXXXXX)
trap 'rm -f -- "$test_binary"' EXIT
flags=(-std=c11 -Wall -Wextra -Werror -g)
if [[ ${PROVISION_OWNER_TEST_SANITIZERS:-0} == 1 ]]; then
    flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
"${CC:-cc}" "${flags[@]}" -I"$test_dir/../touch_reset/stubs" -I"$platform_dir" \
    -I"$platform_dir/include" -I"$repo_dir/components/mybot/mybot_sdk/include" \
    -I"$easyflash_dir" "$test_dir/test_provision_owner.c" -o "$test_binary"
timeout 10s "$test_binary"
