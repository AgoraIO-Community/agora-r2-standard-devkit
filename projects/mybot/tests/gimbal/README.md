# EB521 Motion Tests

The default R2 firmware disables `CONFIG_MYBOT_GIMBAL`. These tests exercise the
retained implementation without enabling UART ownership on the device.

## Run

From the repository root:

```bash
bash projects/mybot/tests/gimbal/run.sh
ASAN_OPTIONS=detect_leaks=0 GIMBAL_TEST_SANITIZERS=1 \
  bash projects/mybot/tests/gimbal/run.sh
```

Set `GIMBAL_TEST_LOGS=1` to print production logs.

## Harness

The tests compile the actual `bk7259_gimbal.c` and AP CLI registration. Small
UART/GPIO/RTOS headers replace hardware dependencies, while pthreads run the
production worker and external callback path.

The controller model:

- Quantizes positions to the EB521 0.315-degree step.
- Uses UART2 with GPIO2 TX and GPIO3 RX.
- Separates command acceptance from physical completion.
- Can delay replies, drop frames, report busy status, or reject commands.
- Asserts that a new motion is not written before the previous one completes.

Host time runs ten times faster than device time.

## Coverage

The suite covers:

- CRC vectors, fragmented frames, glued frames, invalid lengths, and bad CRCs.
- Status-query completion and rejection of stale or mismatched replies.
- All four scenes, final centering, repeated scenes, and segment cancellation.
- Bounded timeout without automatic motion retransmission.
- Busy/admission rules, CLI registration, argument validation, and scene order.
- Worker start/stop, AOSL references, retained resources, and cleanup retry.

The runner has a 60-second timeout so a lock regression cannot wait indefinitely.
AddressSanitizer and UndefinedBehaviorSanitizer cover the available paths.
LeakSanitizer is disabled under the current traced environment.

## Manual Use

CLI commands, wiring, and protocol behavior are documented in
[the EB521 guide](../../../../docs/eb521-gimbal-plan.zh-CN.md). Keep test coverage in
this file and user-facing behavior in that guide.

These tests validate protocol and software lifetime only. UART wiring, electrical
levels, homing, physical direction, travel limits, and long-duration motion still
require the target board.
