# LCD expression policy regression tests

Run from the firmware repository root:

```sh
bash projects/mybot/tests/expression_policy/run.sh
EXPRESSION_POLICY_TEST_SANITIZERS=1 bash projects/mybot/tests/expression_policy/run.sh
```

In environments that run tests under `ptrace`, LeakSanitizer cannot inspect
threads. Set `ASAN_OPTIONS=detect_leaks=0` for that sanitizer invocation; address
and undefined-behavior checking remain enabled. The policy does not allocate.

The C99 harness compiles the production display policy against the real public
LCD and product expression headers. It needs only a native C compiler; it has
no hardware, RTOS, MyBot runtime, LVGL, or HAL stubs.

Checks cover the visible expression and replay decision: workflow priority,
conversation opening, phase transitions and continuous thinking duration,
ordinary refreshes, all four MCP durations and replacement/replay behavior,
restoring the latest phase, persistent local overrides, conversation teardown,
detach/reset cancellation, failed display submission, disabled transitions,
and 32-bit monotonic-clock wraparound. A foreground hold starts only after the
display owner reports successful submission.

This suite does not validate LCD scanout, frame timing, RTM deduplication, or
the SDK's emotion-to-LCD dispatch; these require their respective integration
checks and device verification.
