# Product provisioning owner tests

Run from the firmware repository root:

```sh
bash projects/mybot/tests/provision_owner/run.sh
ASAN_OPTIONS=detect_leaks=0 PROVISION_OWNER_TEST_SANITIZERS=1 \
  bash projects/mybot/tests/provision_owner/run.sh
```

The harness compiles the real application owner, provisioning mailbox, factory
reset implementation and physical-key adapter. It reuses the reset hardware /
EasyFlash fixture and first runs the existing durable-reset regressions. SDK
lifecycle, network operations and display publication are fault-injected; the
separate Wi-Fi tests validate their actual driver ownership implementation.

The owner tests exercise successful setup and SDK restart, quick cancellation
before stop without interrupting the current conversation, cancellation while
stopping, both SDK stopped-state guards, cleanup failure and explicit retry,
one saved-network reconnect after cancellation/error, and unavailable saved
credentials without reopening the portal. They also cover settings recovery
after SDK startup/runtime failure, initial network waits interrupted by pending
requests (including an already accepted cancellation), and a new request arriving
between retiring an old session and clearing its display override.

Factory reset takes priority before initial networking, during the portal and
during saved-network reconnect. Setup-time short presses do not leak into the
next SDK instance. Storage assertions verify ordinary setup/cancellation never
erase device authentication, volume or unrelated vendor data; actual factory
reset still follows its intent/stop/network-clear/verified-erase/reboot sequence.

The SDK start stub requires the product override to be NONE and the ordinary
workflow to be STARTING_SERVICES, matching the display adapter's init guard.
These tests check native sequencing and faults, not physical Wi-Fi, touch input
or real SDK thread teardown. LeakSanitizer is disabled above for the current
ptrace environment; ASAN and UBSAN remain enabled.
