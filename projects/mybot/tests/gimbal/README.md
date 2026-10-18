# EB521 motion tests

The R2 firmware currently disables `CONFIG_MYBOT_GIMBAL`; these host regression tests
exercise the retained implementation without enabling UART ownership on the device.

Run from the firmware repository root:

```sh
bash projects/mybot/tests/gimbal/run.sh
GIMBAL_TEST_SANITIZERS=1 bash projects/mybot/tests/gimbal/run.sh
```

The motion test includes the actual `bk7259_gimbal.c` in one normal configuration.
Small UART/GPIO/RTOS headers replace the hardware dependencies, while
pthread runs the production worker. Host time runs 10 times faster than device
time. A separate CLI test includes the actual AP command registration and
callbacks. Both binaries are created under `/tmp` and removed on exit.

The controller model accepts binary EB521 frames, quantizes positions to 0.315°,
and requires UART2 with TX GPIO2/RX GPIO3 (TXD/RXD function codes 100/99).
It sends `00` acceptance separately from `06` completion. It can omit completion,
delay movement, report busy status, reject commands or drop replies. Every motion
write asserts that the previous movement has finished.

Coverage includes known CRC vectors, fragmented and glued replies, malformed
lengths and CRCs, stopped/ready/at-target validation, status-query completion,
bounded failure without retransmission, all four scene envelopes and final
centering, repeated scenes, segment cancellation, hangup, restart and cleanup.
Delayed-response cases exercise 300 ms status/acceptance replies and lost
acceptance even when the controller physically executes the command.
The SAD test exercises the boundary where a 10° target quantizes to 10.1°.
A failed worker-exit wait verifies that UART/GPIO ownership remains available
for a successful shutdown retry.
AOSL reference counters cover ordinary shutdown, retained workers after product
shutdown, worker-creation failure, cleanup failure and successful retry.

Six groups cover the production motion behavior and resource lifetime.

Four additional groups cover the manual command interface: eight accepted calls
cycle through the four exact frames twice, busy submissions preserve the cursor,
matched E1/E2 ACKs and fragmented replies, rejection/CRC/payload errors permit
the next explicit call, and a relative timeout never causes automatic retry.
Tests also cover startup-query handoff, normal MCP ownership, fault preservation,
queued/in-flight cancellation and resource retention after a failed worker wait.
Simulated external callbacks run on a separate pthread, matching actual task
boundaries instead of recursively acquiring the UART owner's mutex. The runner
has a 60-second timeout so a lock regression cannot leave it waiting indefinitely.

Four CLI-scene groups complete the 14 motion groups: all four full trajectories
without a conversation, fresh pose synchronization after raw commands,
admission and mutual exclusion, bad/missing/busy/out-of-range status rejection,
fault preservation, one-shot execution and queued/in-flight shutdown. Completion
ACKs are omitted in the scene test so acceptance alone cannot finish a segment;
fresh idle position feedback must confirm it. The CLI callback test checks
registration retry/idempotency, invalid arguments, all named scenes, the default
round-robin order and cursor retention on busy/unavailable submissions.

## Manual UART commands

From the existing log console, send:

```text
ap_cmd gimbal_cmd
```

Each accepted invocation queues exactly one next command on the existing UART2
owner (GPIO2 TX/GPIO3 RX, 115200 8N1). There are no command parameters or raw-hex
inputs. The fifth accepted call returns to command 1. A conversation or an
already queued/running command returns busy without advancing the sequence.
There is no timer or autonomous transmission.

| Order | Command |
| --- | --- |
| 1 | Pan left 30°, absolute |
| 2 | Pitch down 10°, absolute; reduced to the confirmed physical limit |
| 3 | Pitch up 20°, absolute |
| 4 | Pan right 10°, relative, once per accepted call |

The exact frames, in the same order, are:

```text
03 61 00 08 D4 FE 00 00 F4 01 F4 01 0A AB
03 61 00 08 00 00 64 00 F4 01 F4 01 11 EF
03 61 00 08 00 00 38 FF F4 01 F4 01 09 67
03 62 00 06 00 00 64 00 F4 01 CF 5A
```

Logs show each TX frame and raw RX bytes (up to 128 bytes per exchange).
Only a CRC-valid, one-byte result-00 ACK with matching E1/E2 reports success.
This checks command acceptance, not physical completion. Failure or a nonzero
ACK is logged; another explicit invocation remains available and sends the next
frame. The cursor advances on acceptance of the CLI request, and no frame is
automatically retransmitted, including the relative command.

The first accepted CLI call latches a manual session until reboot or platform
shutdown/preparation. Within this session, autonomous gimbal queries, MCP motion
and automatic centering are suspended. Explicit full-scene commands below may
still query status and move. Shutdown cancels queued/in-flight work and exits
without a centering command. An existing motion fault is retained. Until the
first CLI call, the normal four-scene behavior is unchanged.

## Complete expression trajectories

MCP emotion commands render LCD expressions only. Gimbal trajectories remain
available through the explicit CLI commands below.

Use one command to run one complete existing trajectory and return to center:

```text
ap_cmd gimbal_scene
ap_cmd gimbal_scene happy
ap_cmd gimbal_scene sad
ap_cmd gimbal_scene angry
ap_cmd gimbal_scene surprised
```

The parameterless command cycles happy → sad → angry → surprised. A successfully
queued named command also sets the next default scene to its successor. Busy,
unavailable or invalid requests leave this cursor unchanged. Scenes do not
advance the separate `gimbal_cmd` frame cursor. There is no automatic repetition.

Full scenes reuse the normal worker and production `play_scene()` trajectories;
they do not require an active conversation. They also work after a raw CLI
command has latched the manual session. An active conversation, queued/running
raw command, or queued/running scene returns busy.

The minimum planned motion period is 40 ms. Happy uses 50 samples over its
unchanged 2-second timeline; each 280 ms centering phase uses seven samples.
Sad keeps its existing 50 ms period. Angry/surprised retain their exact turning
points by merging short remainders into the preceding segment, so no segment
is shorter than 40 ms. Communication delays may still extend actual execution.

Before starting each scene, the worker obtains fresh CRC-valid E3 status and
requires flow 2, both axes stopped and a pose within pan ±80° / pitch −20°..10°
(allowing 0.4° feedback quantization). No movement or stop frame is sent if this
initial validation fails. A previously recorded motion fault stays recorded;
raw CLI commands remain available after the failed scene.

Logs identify scene queuing, pose synchronization and the final result.
CLI scenes also print each TX frame, raw RX bytes (up to 128 bytes per
transaction), discarded old input, target angles, speeds and response timing.
Status queries and movement ACKs each allow 1000 ms. A 250 ms receive slice
without data does not end the ACK transaction; missing replies still fail
within a finite deadline without retransmitting movement. Status confirmation
inside a movement also respects that movement's remaining completion budget.
Each trajectory centers first, performs the selected expression and centers at
the end. Result-00 acceptance is separate from physical completion: following
segments wait for completion/idle position confirmation. Shutdown cancels the
remaining trajectory after the current segment and sends no new 61/64 motion
frames once closing starts; status queries may finish that in-flight segment.

If LeakSanitizer is unavailable under a traced execution environment, use
`ASAN_OPTIONS=detect_leaks=0` with the sanitizer command. AddressSanitizer and
UndefinedBehaviorSanitizer still check the actual code in this mode.

Set `GIMBAL_TEST_LOGS=1` to print production logs. These tests validate software
control and protocol handling; UART wiring, physical directions and motor homing
still require the target board.
