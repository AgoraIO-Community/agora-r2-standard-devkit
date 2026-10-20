# Touch recovery UI regression tests

Run from the firmware repository root:

```sh
bash projects/mybot/tests/touch_ui/run.sh
TOUCH_UI_TEST_SANITIZERS=ON bash projects/mybot/tests/touch_ui/run.sh
```

The native CMake build uses the pinned official AVDK LVGL sources, the product
LVGL configuration, and the actual `lvgl_view.cc`, fonts and expression mapping.
Only BK allocation/assert helpers and the reset-request sink are substituted.
The test delivers pointer samples through a real LVGL input device; it does not
implement a substitute UI or call private button handlers.

Chinese, English and disabled-animation variants check:

- Settings on every SDK workflow page and cancel revealing the latest page.
- Release-only actions, one touch opening only one layer, repeated confirmation
  posting once, dragging away and back, and dragging from background onto buttons.
- `LV_EVENT_CANCEL`, input reset/wait-release, and loss of touch availability
  while a button is held. The input reset sequence is the touch adapter's contract
  for multi-touch, invalid samples, I2C failure and queue overflow.
- Disabled entry/hint when touch is unavailable, and a reset-failure page with
  one-shot Retry. Recovery cannot return to ordinary pages or accept SDK updates.
- Separate Wi-Fi setup confirmation, request rejection, release-only start,
  immediate PREPARING protection, copied 32-byte hotspot SSID and portal address,
  accepted cancellation retaining CANCELLING across delayed ACTIVE snapshots,
  late cancellation rejected after commit, progress-only finishing/reconnect,
  failed-setup retry, and factory reset taking priority over setup snapshots.
- All 23 expression layouts, a six-digit pair code with leading zeros, glyph
  coverage, text bounds, fixed widget/timer counts, and destroy/recreate cleanup.

The run prints its temporary native build path and writes RGB PPM previews into
`output/{zh,en,noanim}`. Set `TOUCH_UI_BUILD_DIR` to reuse a build, or
`TOUCH_UI_OUTPUT_DIR` to choose the preview destination. Native ASan/UBSan builds
disable leak detection by default for environments using `ptrace`; override
`ASAN_OPTIONS` where LeakSanitizer is supported.

The suite validates the view's LVGL event behavior. Physical CST7XX coordinates,
multi-touch frame parsing, task synchronization, flash erasure and hardware
reboot belong to their respective integration and device tests.

Provisioning request/cancel/busy/abort mocks expose only the public mailbox
contract and selectable outcomes. The actual mailbox and network cleanup have
their own production-source fault tests; this harness does not reproduce them.
