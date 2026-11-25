# BK7259 Firmware Validation

**English | [简体中文](UI_VALIDATION.zh-CN.md)**

## Scope

This page records the current reproducible checks and remaining release blockers.
It replaces the former chronological development log. Older intermediate
diagnostics remain available in Git history and are not current instructions.

The original board checklist was reviewed on 2026-10-22. On 2026-11-25 the
Chinese R2 build was verified with EB521 V1.2 enabled. On the preceding test
image, two consecutive `happy` runs completed and returned to center on hardware.

## Current Host and Build Checks

The pinned source set passes:

```bash
python3 scripts/check_sources.py
```

This verifies:

- AVDK and Solution gitlinks and tracked-tree cleanliness.
- Patch hashes and the MyBot/AOSL content digests.
- RTSA header and static-library hashes.
- Product include boundaries.

The managed clean build passes:

```bash
python3 scripts/build_all.py --language zh-CN
```

The Chinese build produces both cores, the combined image, and the OTA package.
It completed in 34 seconds on the development host.
Generated endpoints and configuration were verified by the build script.

Local build footprint (Chinese with EB521, English from the earlier build):

| Item | Chinese | English |
| --- | ---: | ---: |
| AP Flash | 1,952,200 B | 1,933,176 B |
| AP RAM | 158,004 B | 157,740 B |
| AP DTCM | 35,744 B | 35,744 B |
| CP Flash | 1,295,948 B | 1,295,948 B |
| Combined image | 3,459,552 B | 3,440,512 B |
| OTA package | 2,072,032 B | 2,057,616 B |

The OTA partition is 2,097,152 bytes. The Chinese package has 25,120 bytes
of headroom, so new assets or code need an explicit size review.

## Host Test Matrix

Run the product suites:

```bash
for script in projects/mybot/tests/*/run.sh; do
  bash "$script"
done
```

Run build-preparation tests:

```bash
(cd scripts && python3 -m unittest test_build_all.py test_vendor_workspace.py)
```

Coverage includes:

- Build preparation and dependency workspace ownership.
- Provisioning owner, cancellation, credential commit, and reset priority.
- Persistent servo and power preferences, save failure and retry, and factory-reset recovery.
- CST7XX touch parsing, GPIO capabilities, lifecycle, and event bounds.
- Native LVGL touch flows, both languages, disabled animations, unavailable gimbal controls, the power action,
  and 23 expressions.
- Expression and voiceprint indicator policy.
- HTTPS retry, timeout, EOF, socket-error isolation, and clock wraparound.
- EB521 V1.2 probing, four motion scenes, noise and timeout handling, charging
  heartbeats during motion, charge-stop fencing, and worker cleanup.

The host suites and available ASan/UBSan variants pass. LeakSanitizer is disabled
in the current ptrace environment, so leak detection is not included in that
result.

## Release Blockers

These items require code or contract changes, not only additional testing:

1. **Certificate dates:** the adapter requests certificate and hostname
   verification, but the active MbedTLS configuration does not enable
   `MBEDTLS_HAVE_TIME_DATE`. Certificate expiry and not-before checks must be
   restored and tested.
2. **Credential storage:** Wi-Fi and device credentials are stored in EasyFlash
   without at-rest encryption. Production storage protection still needs a
   product decision.
3. **RTSA distribution:** the repository contains the proprietary RTSA input, but
   no standalone redistribution grant is recorded. Obtain written authorization
   or remove it from public artifacts.
4. **OTA capacity:** the current OTA package has less than 2% partition headroom.
   Add a size gate or revise the partition plan before enabling additional assets.

## Target Board Checks

Before production:

- Cold boot the combined `all-app.bin` and verify both cores.
- Exercise repeated MyBot start/stop and conversation teardown.
- Validate Wi-Fi setup, cancellation, saved-network recovery, and power loss.
- Validate three-input capture, AEC, volume, and bidirectional RTC audio.
- Validate camera detection, H.264 output, frame pacing, bitrate changes, IDR,
  and repeated video sessions.
- Validate LCD, touch, factory reset, and storage-failure pages.
- Measure stack, heap, PSRAM, and timing headroom with audio, video, UI, HTTP, and
  RTC active together.
- Validate the real service certificate chain, hostname, expiry, and timeout
  behavior.
- Run long-duration and watchdog-recovery tests.

Host tests and firmware compilation are not evidence of target hardware behavior.
