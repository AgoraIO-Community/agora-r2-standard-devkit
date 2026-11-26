# EB521 V1.2 Tests

The default R2 AP firmware enables `CONFIG_MYBOT_GIMBAL`. These tests run the
same V1.2 driver with a simulated UART controller.

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
production UART worker.

The controller model uses UART2 with GPIO2 TX and GPIO3 RX. It separates the
`E1` motion acceptance and completion replies, accepts `0x66` during motion,
and can delay or drop replies, reject commands, or report an already-reached
target.

Host time runs ten times faster than device time.

## Coverage

The suite covers:

- CRC vectors, fragmented frames, glued frames, invalid lengths, and bad CRCs.
- Boot with a missing or uninitialized board, hot unplug and reconnection,
  and confirmed homing at 1000 pps.
- Ordinary moves waiting for a matching ACK or a 250 ms deadline, with charging and homing serviced independently.
- Default charging, heartbeats during motion, stop acknowledgement and timeout.
- Shared voice and CLI motion admission, dialogue phases, and CLI argument validation.
- Worker start/stop, AOSL references, retained resources, and cleanup retry.

The runner has a 60-second timeout so a lock regression cannot wait indefinitely.
AddressSanitizer and UndefinedBehaviorSanitizer cover the available paths.
LeakSanitizer is disabled under the current traced environment.

## Manual Use

CLI commands, wiring, and protocol behavior are documented in
[the EB521 guide](../../../../docs/eb521-gimbal-plan.zh-CN.md). Keep test coverage in
this file and user-facing behavior in that guide.

On-target checks cover UART wiring and electrical levels, physical direction,
travel limits, charging current, and the bottom board's 3000 ms charging timeout.
