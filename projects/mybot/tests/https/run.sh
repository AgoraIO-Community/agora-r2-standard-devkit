#!/usr/bin/env bash
set -euo pipefail
test_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
sdk_dir="$test_dir/../../../../components/mybot/mybot_sdk"
mbedtls_dir="$test_dir/../../../../bk_avdk_smp/ap/components/psa_mbedtls/mbedtls"
test_binary=$(mktemp /tmp/bk7259-https-test.XXXXXX)
trap 'rm -f -- "$test_binary"' EXIT
extra_flags=()
if [[ "${HTTPS_TEST_SANITIZERS:-0}" == 1 ]]; then
    # Keep unrelated TLS initialization sections discardable; this harness tests
    # socket transport and DNS request ownership, not the MbedTLS library itself.
    extra_flags=(-fsanitize=address,undefined -fno-omit-frame-pointer --param=asan-globals=0)
fi
"${CC:-cc}" -std=c99 -D_POSIX_C_SOURCE=200809L \
    -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -Wl,--gc-sections "${extra_flags[@]}" \
    -I"$test_dir/stubs" \
    -I"$sdk_dir/include" -I"$sdk_dir/platforms/bk7259/include" \
    -I"$mbedtls_dir/include" "$test_dir/test_https.c" -o "$test_binary"
"$test_binary"
