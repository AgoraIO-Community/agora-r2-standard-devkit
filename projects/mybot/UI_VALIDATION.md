# BK7259 Firmware Validation

**English | [简体中文](UI_VALIDATION.zh-CN.md)**

## Scope

This page records the current reproducible checks and remaining release blockers.
It replaces the former chronological development log. Older intermediate
diagnostics remain available in Git history and are not current instructions.

The status below was reviewed on 2026-10-22. No target-board test was performed
during this documentation update.

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
python3 scripts/build_all.py
```

It builds both languages, both cores, the combined image, and the OTA package.
The latest observed runtime was about 1 minute 9 seconds on the development host.
Generated endpoints and configuration were verified by the build script.

Latest local build footprint:

| Item | Chinese | English |
| --- | ---: | ---: |
| AP Flash | 1,932,736 B | 1,932,752 B |
| AP RAM | 157,740 B | 157,740 B |
| AP DTCM | 35,744 B | 35,744 B |
| CP Flash | 1,295,948 B | 1,295,948 B |
| Combined image | 3,440,064 B | 3,440,096 B |
| OTA package | 2,057,808 B | 2,057,408 B |

The OTA partition is 2,097,152 bytes. The Chinese package has only 39,344 bytes
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
- Factory-reset persistence and storage-failure injection.
- CST7XX touch parsing, GPIO capabilities, lifecycle, and event bounds.
- Native LVGL touch flows, both languages, disabled animations, and 23 expressions.
- Expression and voiceprint indicator policy.
- HTTPS retry, timeout, EOF, socket-error isolation, and clock wraparound.
- EB521 protocol, trajectories, CLI admission, and worker cleanup.

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
- Exercise repeated SDK start/stop and conversation teardown.
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
