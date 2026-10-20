#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
test_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$test_dir/../../../.." && pwd)
test_work=$(mktemp -d /tmp/bk7259-touch-test.XXXXXX)
trap 'rm -rf -- "$test_work"' EXIT
# Compile the exact patch applied to the locked, read-only official source.
git -C "$repo_root/bk_avdk_smp" archive HEAD \
    ap/components/bk_peripheral/src/tp ap/components/bk_peripheral/include/tp_sensor_devices.h \
    ap/include/driver/tp.h ap/include/driver/tp_types.h | tar -x -C "$test_work"
git -C "$test_work" apply --check "$repo_root/patches/bk_avdk_smp/0004-r2-cst7xx-touch.patch"
git -C "$test_work" apply "$repo_root/patches/bk_avdk_smp/0004-r2-cst7xx-touch.patch"
port="$repo_root/components/mybot/mybot_sdk/platforms/bk7259"
extra_flags=()
if [[ "${TOUCH_TEST_SANITIZERS:-0}" == 1 ]]; then
    extra_flags=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
# Compile the board fixture with real BK7259 GPIO enum/bitfield headers and the
# current AP table. Only unrelated MMIO/log includes are replaced for the host.
"${CC:-cc}" -std=c99 -DCONFIG_USR_GPIO_CFG_EN=1 -DCONFIG_MYBOT_GIMBAL=1 \
    -Wall -Wextra -Werror "${extra_flags[@]}" \
    -I"$test_dir/board_stubs" -I"$repo_root/bk_avdk_smp/ap/include" \
    -I"$repo_root/bk_avdk_smp/ap/middleware/driver/bk7259_ap" -I"$test_dir/stubs" \
    -I"$repo_root/projects/mybot/ap/config/bk7259_ap" \
    -c "$test_dir/board_gpio_fixture.c" -o "$test_work/board_gpio_fixture.o"
for case in touch cst7xx driver; do
    case_objects=()
    if [[ "$case" == driver ]]; then
        case_objects=("$test_work/board_gpio_fixture.o")
    fi
    "${CC:-cc}" -std=c99 -D_POSIX_C_SOURCE=200809L \
        -DCONFIG_TP_CST7XX=1 -DCONFIG_TP_RST_PIN=44 -DCONFIG_TP_INT_PIN=43 \
        -DCONFIG_TP_I2C_SDA_PIN=65 -DCONFIG_TP_I2C_SCL_PIN=64 \
        -Wall -Wextra -Werror -Wno-unused-parameter -pthread \
        -ffunction-sections -fdata-sections -Wl,--gc-sections "${extra_flags[@]}" \
        -include "$test_dir/stubs/os/os.h" \
        -I"$test_dir/stubs" -I"$port" -I"$port/include" \
        -I"$test_work/ap/include" -I"$test_work/ap/components/bk_peripheral/src/tp" \
        "$test_dir/test_$case.c" "${case_objects[@]}" -o "$test_work/test_$case"
    "$test_work/test_$case"
done
