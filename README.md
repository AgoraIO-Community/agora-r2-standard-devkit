# mybot-bk7259

[![License](https://img.shields.io/github/license/junlon2006/mybot-bk7259)](LICENSE)
[![Upstream](https://img.shields.io/badge/upstream-mybot-0969da)](https://github.com/junlon2006/mybot)
[![Platform](https://img.shields.io/badge/platform-BK7259-2f6f4e)](https://github.com/bekencorp/bk_avdk_smp)

**[简体中文](README.zh-CN.md) | English**

`mybot-bk7259` is a reference firmware integration of the
[MyBot SDK](https://github.com/junlon2006/mybot) for the Beken BK7259 AP/CP platform.
The `release/v4.0.1-mybot-r2` branch targets the Yunqu V1.0 R2 board. It provides board
initialization, network ownership, peripheral adapters, firmware build files, and local
product policy; the MyBot SDK remains the owner of device-service registration,
authentication, pairing, conversation state, RTC/RTM, and voiceprint behavior.

This is a reference implementation, not a production-ready product image. Board adaptation,
security review, target testing, and third-party licensing review are still required before
deployment.

## Features

- BK7259 AP/CP startup, Flash partitions, and SRAM/PSRAM layout.
- Product-owned STA reconnection and APSTA Wi-Fi provisioning with a captive portal.
- Three-input analog capture with an appended playback reference for AEC.
- 16 kHz mono signed 16-bit audio playback and persistent volume control.
- Optional MIPI CSI camera capture, hardware H.264 encoding, and Agora RTC video uplink.
- GC9503CV MIPI display, CST7XX touch, local status UI, Wi-Fi setup, and factory reset.
- One GPIO conversation key, EasyFlash key-value storage, HTTPS transport, and derived
  device identity.
- Embedded Chinese and English prompt assets, with no SD card or separate asset partition.
- Managed builds that produce both Chinese and English AP/CP images, a combined flash
  image, and an OTA package.

The device service and cloud AI agent are external dependencies and are not part of this
repository.

## Architecture

| Component | Responsibility |
| --- | --- |
| CP (`bk7259`) | Initializes the BK runtime and starts the AP system. |
| AP (`bk7259_ap`) | Runs the product control loop, APSTA/STA networking, MyBot lifecycle, audio, video, display, touch, and keys. |
| MyBot SDK | Owns the platform-independent device service and conversation state machine. |
| BK7259 platform adapters | Implement the MyBot platform `ops` using BK components and board resources. |
| AOSL | Provides the portable runtime used by MyBot and RTSA. |

The product control loop is [projects/mybot/ap/ap_main.c](projects/mybot/ap/ap_main.c).
It derives the device ID from the chip UID, prepares product-owned hardware, restores or
provisions Wi-Fi, starts MyBot only after STA IPv4 connectivity is available, and serializes
Wi-Fi setup and factory-reset operations with the SDK lifecycle.

BK platform sources access MyBot only through public platform headers under
`<mybot/platform/...>`. The AP application entry point owns the public MyBot lifecycle calls.

## Repository Layout

| Path | Contents |
| --- | --- |
| `bk_avdk_smp/` | Pinned BK7259 AP/CP SDK submodule |
| `bk_solution_ai/` | Pinned BK solution submodule |
| `components/mybot/mybot_sdk/` | MyBot SDK snapshot and BK7259 platform adapters |
| `components/mybot/mybot_aosl/` | Vendored AOSL runtime |
| `components/mybot/mybot_rtsa/` | Agora RTSA headers, package metadata, and target library |
| `projects/mybot/` | AP/CP entry points, board configuration, partitions, assets, and tests |
| `patches/` | Reviewed vendor patches applied only to generated build copies |
| `scripts/` | Dependency preparation, source verification, build, and test helpers |
| `docs/` | Dependency, operation, and validation documentation |

Generated directories such as `build/` and `releases/` are ignored. Local reference
directories are not build inputs.

## Pinned Dependencies

| Dependency | Revision |
| --- | --- |
| `bk_avdk_smp` | `584b947920c414230f9909375f008e7d6a2c6c6b` |
| `bk_solution_ai` | `03aadb49756a5109bfede04002b9b6820f407c3f` |
| MyBot SDK snapshot | `58f0ef9c14be9f159b1dc167bf0f035983b25117`, version 1.3.0 |
| AOSL base | `84e086084ebcd0ae2455a0ce5721950c5fe2e656` |
| Agora RTSA | v1.10.1 build `1278380` |

The parent repository pins exact submodule gitlinks in `vendor.lock.json`, records target
source modifications in `SDK_REVISION` and `AOSL_REVISION`, and records the RTSA package in
`components/mybot/mybot_rtsa/PACKAGE_INFO`. The build applies the recorded vendor patches to
isolated copies; it does not modify the official submodule checkouts.

See [Official dependencies and isolated builds](docs/OFFICIAL_DEPENDENCIES.md) for patch
provenance, digests, upgrade rules, and verification boundaries.

## Requirements

- Linux with Git submodule support.
- Python 3 and the build dependencies required by `bk_avdk_smp`.
- The BK7259 Arm GNU toolchain selected by `bk_avdk_smp`.
- A Yunqu V1.0 R2 board, or hardware with an equivalent board configuration.
- A compatible MyBot device service and an Agora-enabled deployment.

## Get the Source

```bash
git clone --branch release/v4.0.1-mybot-r2 \
  --recurse-submodules https://github.com/junlon2006/mybot-bk7259.git
cd mybot-bk7259
```

For an existing clone:

```bash
git submodule sync --recursive
git submodule update --init --recursive
git submodule status
```

Normal development uses the gitlinks pinned by the parent repository. Do not use
`git submodule update --remote` to follow moving branch heads.

## Build

From the repository root:

```bash
python3 scripts/check_sources.py
python3 scripts/build_all.py
```

`check_sources.py` verifies the submodule revisions, tracked-tree cleanliness, patch hashes,
MyBot and AOSL content digests, RTSA artifact hashes, and product source boundaries.
`build_all.py` prepares isolated vendor workspaces, builds the CP and AP for each requested
language, verifies the generated configuration and service endpoint, and publishes outputs
only after all requested variants succeed.

Useful options:

```bash
python3 scripts/build_all.py --language zh-CN
python3 scripts/build_all.py --language en-US
python3 scripts/build_all.py --dry-run
python3 scripts/build_all.py --no-clean
```

Only `MYBOT_AUDIO_PTIME_MS=60` is supported by the pinned RTSA package.

### Outputs

The default outputs are:

```text
releases/bk7259-zh-CN-video.bin
releases/bk7259-en-US-video.bin
```

If video is disabled in the AP defconfig, `-video` is omitted. If HTTPS is disabled,
`-http` is added.

Generated artifacts for the Chinese default build are under:

```text
build/build-bk7259-zh-CN-video/solution/projects/mybot/build/bk7259/mybot/package/
```

| File | Purpose |
| --- | --- |
| `all-app.bin` | Combined bootloader, CP, and AP flash image |
| `app_pack.rbl` | OTA package containing the CP and AP images |
| `build_summary.txt` | Flash and RAM usage report |

The individual CP and AP images are under `bk7259/app.bin` and
`bk7259_ap/app.bin` in the same generated build tree.

To invoke the vendor build directly, first prepare a generated project:

```bash
python3 scripts/prepare_vendor.py --language zh-CN
```

Then run the complete `make` command printed by the script. Do not build the source project
against an unpatched vendor checkout.

## Flash and Use

Flash `all-app.bin` with the Beken flashing tool. Board-specific flashing instructions are
provided with `bk_avdk_smp`.

Typical R2 flow:

1. With no saved Wi-Fi credentials, the device starts an open `mybot-xxxx` SoftAP.
2. Connect to it and open `http://192.168.4.1/`, then submit the target network credentials.
3. After the STA interface obtains IPv4, provisioning stops and MyBot starts.
4. An unclaimed device displays and announces a pairing code. Enter the complete code,
   including leading zeros, in the device-service console.
5. In `READY`, short press and release `GPIO47` to start a conversation; repeat to stop it.
6. Touch **Settings** to change Wi-Fi or confirm a factory reset.

The current R2 hardware handles long-press power on/off outside the firmware conversation
key path. GPIO and peripheral assignments are defined in
[usr_gpio_cfg.h](projects/mybot/ap/config/bk7259_ap/usr_gpio_cfg.h).

## Configuration

Main configuration files:

| File | Purpose |
| --- | --- |
| `projects/mybot/ap/config/bk7259_ap/defconfig` | AP features, language, service transport, display, audio, and video |
| `projects/mybot/cp/config/bk7259/defconfig` | CP features |
| `projects/mybot/partitions/bk7259/auto_partitions.csv` | Flash layout |
| `projects/mybot/partitions/bk7259/ram_regions.csv` | SRAM/PSRAM layout |

Important AP options:

- `CONFIG_MYBOT_LANGUAGE_ZH_CN` or `CONFIG_MYBOT_LANGUAGE_EN_US`: selects UI text, prompt
  assets, and the service endpoint. Exactly one must be enabled.
- `CONFIG_MYBOT_HTTPS`: selects HTTPS or HTTP for device-service REST requests.
- `CONFIG_MYBOT_VIDEO`: enables camera capture and H.264 uplink.
- `CONFIG_MYBOT_LVGL_UI_ANIMATIONS`: enables face transitions and cursor animation.
- `CONFIG_MYBOT_KEY_GPIO` and `CONFIG_MYBOT_KEY_ACTIVE_LEVEL`: configure the conversation key.

The default service endpoints are:

```text
https://botstation.shengwang.cn/api   # zh-CN
https://botstation.agora.io/api       # en-US
```

Disabling HTTPS changes only the URL scheme; it does not change RTC/RTM or the local
provisioning portal. Verify the deployed certificate chain and service configuration before
production use.

After changing project configuration, rebuild without `--no-clean` so generated CMake state
does not retain previous values.

## Tests and Validation

Source and host-side checks:

```bash
python3 scripts/check_sources.py
for script in projects/mybot/tests/*/run.sh; do
  bash "$script"
done
(cd scripts && python3 -m unittest test_build_all.py test_vendor_workspace.py)
```

The test suites cover build preparation, provisioning ownership and cancellation, touch and
factory reset, expression policy, HTTPS retry/deadline handling, gimbal protocol logic, and
the native LVGL view. Host tests do not replace target validation.

Before production deployment, validate at least:

- Cold boot and repeated SDK start/stop on the R2 board.
- Wi-Fi provisioning, cancellation, reconnect, and power-loss recovery.
- Bidirectional RTC audio and repeated conversation teardown.
- Camera capture, H.264 encoding, bitrate adaptation, and repeated video sessions.
- Display, touch, factory reset, and storage failure behavior.
- Concurrent audio, video, UI, HTTP, and RTC load with measured stack, heap, PSRAM, and
  timing headroom.
- Certificate-chain, hostname, expiry, and timeout behavior for the deployed service.

Current limitations:

- The provisioning SoftAP is open and serves its local setup page over HTTP.
- Wi-Fi and device credentials are stored in EasyFlash without at-rest encryption.
- The target header defines separate RTC and RTM token concepts; confirm the service contract
  before claiming end-to-end RTM interoperability.
- The current three-input AEC configuration and complete video path still require target
  hardware validation.
- Wake words are not enabled, and the EB521 gimbal is disabled in the default AP
  configuration.
- Third-party redistribution terms must be resolved before publishing firmware.

Detailed board behavior and implementation notes are in the
[project guide](projects/mybot/README.md). Current validation evidence and remaining
target checks are in [UI_VALIDATION.md](projects/mybot/UI_VALIDATION.md).

## Documentation

- [R2 operation manual (Chinese PDF)](docs/R2_操作手册.pdf)
- [BK7259 MyBot project guide](projects/mybot/README.md)
- [Official dependencies and isolated builds](docs/OFFICIAL_DEPENDENCIES.md)
- [MyBot SDK](https://github.com/junlon2006/mybot)
- [MyBot porting guide](https://github.com/junlon2006/mybot/blob/main/docs/PORTING.md)
- [MyBot embedded integration guide](https://github.com/junlon2006/mybot/blob/main/docs/EMBEDDED.md)
- [BK7259 SDK and solution documentation](bk_solution_ai/docs/bk7259/)

## Contributing

- Send platform-independent SDK and public API changes to the upstream MyBot project.
- Send BK7259 board, adapter, project, and build changes to this repository.
- Keep vendor changes in the recorded patch set.
- Include the hardware, configuration, and validation performed with each change.
- Do not commit generated build products, local references, credentials, or tokens.

## License and Third-Party Dependencies

Original content in this top-level repository is licensed under the
[Apache License 2.0](LICENSE). This does not relicense the submodules, vendored snapshots,
fonts, audio assets, or prebuilt binaries.

The combined project includes components with separate or additional terms:

- [BK AVDK license](bk_avdk_smp/LICENSE)
- [BK Solution license](bk_solution_ai/LICENSE)
- [AOSL license and additional terms](components/mybot/mybot_aosl/aosl/LICENSE)
- [LVGL license](bk_avdk_smp/ap/components/lvgl/LICENCE.txt)
- [LVGL view, font, and simulator provenance](components/mybot/mybot_sdk/platforms/bk7259/display/SOURCES.md)
- [BK7259 third-party notices](components/mybot/mybot_sdk/THIRD_PARTY_NOTICES.md)
- [MyBot third-party notices](https://github.com/junlon2006/mybot/blob/main/THIRD_PARTY_NOTICES.md)

The AOSL license contains conditions in addition to Apache-2.0, so the combined project is
not uniformly Apache-2.0 licensed. Embedded prompt and visual assets also have separate
provenance requirements.

The repository contains the Agora RTSA v1.10.1 build `1278380` input under
`components/mybot/mybot_rtsa/`. The package has no standalone redistribution grant in this
repository. Obtain the required written rights before publishing the binary or firmware that
contains it.
