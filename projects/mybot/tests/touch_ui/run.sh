#!/usr/bin/env bash
set -euo pipefail
test_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
test_build_dir=${TOUCH_UI_BUILD_DIR:-$(mktemp -d /tmp/bk7259-touch-ui.XXXXXX)}
cmake -S "$test_dir" -B "$test_build_dir" -G Ninja \
    -DTOUCH_UI_SANITIZERS="${TOUCH_UI_TEST_SANITIZERS:-OFF}"
cmake --build "$test_build_dir" --parallel "${TOUCH_UI_JOBS:-8}"
for variant in zh en noanim; do
    output_dir="${TOUCH_UI_OUTPUT_DIR:-$test_build_dir/output}/$variant"
    mkdir -p -- "$output_dir"
    ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" \
        timeout 30s "$test_build_dir/touch_ui_$variant" "$output_dir"
done
printf 'Touch UI native build and previews: %s\n' "$test_build_dir"
