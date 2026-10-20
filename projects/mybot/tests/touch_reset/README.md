# Touch reset owner checks

Run `bash projects/mybot/tests/touch_reset/run.sh`. Optional ASAN/UBSAN:
`ASAN_OPTIONS=detect_leaks=0 TOUCH_RESET_TEST_SANITIZERS=1 bash projects/mybot/tests/touch_reset/run.sh`.

The harness compiles the production request mailbox, GPIO key adapter and application
owner together. SDK lifecycle calls, GPIO, RTOS and EasyFlash storage are fault-injected.
It checks request coalescing before SDK/key initialization, request survival across key
prepare/shutdown, debounce, boot-held release, the 5000 ms ignore boundary, clock wrap,
conversation suppression during reset, persistent intent, STOPPED/network cleanup gates,
flash/delete failures, explicit touch retries, boot resume, invalid intent, and the
recovery entry after network or SDK startup failure. It asserts the vendor calibration
record survives and that only the product owner deletes after writers have stopped.

These deterministic checks do not verify touch hardware or LCD rendering. Those require
the independent touch/display checks and on-device validation.
