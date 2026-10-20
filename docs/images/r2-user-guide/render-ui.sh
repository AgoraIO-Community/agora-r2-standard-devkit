#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
build_dir=${R2_MANUAL_UI_BUILD_DIR:-$(mktemp -d /tmp/r2-manual-ui.XXXXXX)}
cmake -S "$source_dir" -B "$build_dir" -G Ninja
cmake --build "$build_dir" --target r2_manual_ui --parallel "${R2_MANUAL_UI_JOBS:-8}"
mkdir -p -- "$build_dir/screenshots"
"$build_dir/r2_manual_ui" "$build_dir/screenshots"
python3 - "$build_dir/screenshots" "$source_dir" <<'PY'
from pathlib import Path
import sys
from PIL import Image

source = Path(sys.argv[1])
destination = Path(sys.argv[2])
for ppm in sorted(source.glob("*.ppm")):
    with Image.open(ppm) as image:
        assert image.size == (640, 480)
        image.save(destination / f"{ppm.stem}.png", optimize=True)
print(f"PNG screenshots: {destination}")
PY
