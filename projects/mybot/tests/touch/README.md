# R2 CST7XX touch tests

Run from the firmware repository root:

```sh
bash projects/mybot/tests/touch/run.sh
ASAN_OPTIONS=detect_leaks=0 TOUCH_TEST_SANITIZERS=1 bash projects/mybot/tests/touch/run.sh
```

The harness exports only the needed files from the pinned official AVDK into a
temporary directory and applies `0004-r2-cst7xx-touch.patch`. It compiles the real
platform adapter, patched CST7XX parser and patched TP lifecycle separately.
Official checkouts and generated firmware trees are not modified.

The native driver test also links a board fixture compiled from the actual AP
`usr_gpio_cfg.h`, using the pinned BK7259 GPIO enums and `gpio_default_map_t`
bitfield declaration. GPIO stubs enforce the vendor's declared-direction filter
instead of always returning success. The real TP initialization must drive the
interrupt pin as OUTPUT/high before switching it to INPUT; the reset pin needs
OUTPUT, and the shared software-I2C pads need SCL OUTPUT and SDA INPUT/OUTPUT.
Removing each capability from a host copy of the table must make initialization
fail. This catches an input-only GPIO43 declaration before flashing.

Coverage includes the first tap after an initial idle read, fingers held across
boot, raw-to-logical rotation, final UP coordinates without an intervening MOVE,
dual-finger cancellation, malformed frames, coordinates outside the panel,
I2C errors, five consecutive read failures and idle recovery, bounded queue
overflow, LVGL reset/wait-for-release, failed init and repeated prepare/shutdown.
Native driver tests use real host threads and semaphores to verify a blocked I2C
read retains resources after a stop timeout; retry releases them only after the
task leaves its I2C work and borrowed callback. No test permits force-deleting a
worker. GPIO disable and bus-release failures also preserve retry ownership.

The TP driver keeps two coalescing semaphores for the process lifetime to cover
cross-core give/IRQ completion. The adapter likewise keeps one mutex and a fixed
16-entry event queue; it does not allocate per touch. LeakSanitizer is disabled in
the command above because the current ptrace environment cannot run it. These
tests validate the native algorithms/lifecycle, not physical I2C timing or the
panel's actual coordinate orientation; those require the target board.

The CST7XX parser comes from the reviewed BK v4.0.1 AVDK baseline and preserves
its Apache-2.0 copyright/license. The maintained patch additionally validates a
complete frame before updating retained contacts and preserves explicit UP
coordinates. Kconfig, source registration, sensor ID and 20 ms reset timing are
carried in the same patch. The TP task delivers two-contact batches through an
explicit callback, bypassing the vendor `drv_tp` heap queue.
