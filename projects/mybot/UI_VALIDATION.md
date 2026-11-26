# BK7259 Firmware Validation

**English | [简体中文](UI_VALIDATION.zh-CN.md)**

## Scope

This page records the current reproducible checks and remaining release blockers.
It replaces the former chronological development log. Older intermediate
diagnostics remain available in Git history and are not current instructions.

On 2026-11-26, the Chinese and English video builds passed with EB521 V1.2
enabled. On an earlier hardware test image, two consecutive `happy` runs completed
and returned to center. The current builds still require board validation.

## Current Host and Build Checks

The pinned source set passes:

```bash
python3 scripts/check_sources.py
```

This verifies:

- AVDK and Solution gitlinks and tracked-tree cleanliness.
- Patch hashes and the SDK/AOSL content digests.
- RTSA header and static-library hashes.
- Product include boundaries.

The managed clean build passes:

```bash
python3 scripts/build_all.py --language both
```

It builds both languages, both cores, the combined image, and the OTA package.
The build script verified the generated configuration and service endpoint for each language.

| Item | Chinese | English |
| --- | ---: | ---: |
| AP Flash | 1,960,112 B | 1,960,104 B |
| AP RAM | 164,724 B | 164,724 B |
| AP DTCM | 35,744 B | 35,744 B |
| CP Flash | 1,295,948 B | 1,295,948 B |
| Combined image | 3,467,456 B | 3,467,456 B |
| OTA package | 2,076,624 B | 2,076,224 B |
| OTA headroom | 20,528 B | 20,928 B |

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
- EB521 V1.2 probing, four motion scenes, motion ACK matching and the 250 ms timeout, charging
  heartbeats during motion, charge-stop fencing, and worker cleanup.

All nine product host suites and 21 build-tool tests passed.

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
