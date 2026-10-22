# BK Dependencies and Vendor Patches

**English | [简体中文](OFFICIAL_DEPENDENCIES.zh-CN.md)**

## Source Roles

The repository has two source submodules:

| Dependency | Repository | Pinned commit |
| --- | --- | --- |
| AVDK | `https://github.com/bekencorp/bk_avdk_smp.git` | `584b947920c414230f9909375f008e7d6a2c6c6b` |
| Solution | `https://github.com/bekencorp/bk_solution_ai.git` | `03aadb49756a5109bfede04002b9b6820f407c3f` |

The parent repository owns:

- `projects/mybot/` for the AP/CP project and board integration.
- `components/mybot/mybot_sdk/` for the pinned agent-studio-device-firmware snapshot and
  BK7259 adapters.
- `components/mybot/mybot_aosl/` for the patched AOSL runtime.
- `components/mybot/mybot_rtsa/` for the target Agora RTSA input.
- `patches/` and `vendor.lock.json` for vendor modifications and their hashes.

The SDK snapshot is agent-studio-device-firmware v1.3.0 at
`32ac74bc59cb72db70e9a68337753561b59b13d0`, plus the BK7259 HTTPS body-logging
target patch. Its source and license digests are recorded in
`components/mybot/mybot_sdk/SDK_REVISION`; the target patch is recorded in
`patches/agent-studio-device-firmware/0001-bk7259-https-debug-log.patch`.
AOSL is based on `84e086084ebcd0ae2455a0ce5721950c5fe2e656`; its content digest
and BK7259 HAL modifications are recorded in `components/mybot/mybot_aosl/AOSL_REVISION`.

The selected RTSA input is v1.10.1 build `1278380`. Its package identity is in
`components/mybot/mybot_rtsa/PACKAGE_INFO`, and its public header and static
library hashes are pinned by `mybot_sdk/CMakeLists.txt`.

## Vendor Patches

Patches are applied only to generated vendor workspaces.

| Patch | Purpose |
| --- | --- |
| `patches/bk_avdk_smp/0001-captive-portal-dns.patch` | Captive-portal DNS behavior for AP and CP |
| `patches/bk_avdk_smp/0002-r2-gc9503cv-panel.patch` | GC9503CV MIPI panel support |
| `patches/bk_avdk_smp/0003-r2-trimic-aec.patch` | Three-input AEC integration and matching library |
| `patches/bk_avdk_smp/0004-r2-cst7xx-touch.patch` | CST7XX touch and bounded lifecycle |
| `patches/bk_solution_ai/0001-mybot-camera-helper.patch` | Camera helper, XCLK, and low-frame-rate integration |

Patch SHA256 values are stored in `vendor.lock.json`. The AEC archive is
591224 bytes with SHA256:

```text
754ad74651b43cbe2cee29a0a882ee572fa70cf2b16dc4e6dc7006e695e65f90
```

Do not patch the source submodules in place. When a patch is no longer needed,
remove it from the lock and build metadata only after verifying that upstream
contains an equivalent change.

## Build Isolation

The managed build:

1. Verifies the parent gitlinks and tracked-tree state of both submodules.
2. Verifies every patch hash.
3. Clones the pinned commits into `build/_vendor/<fingerprint>/`.
4. Applies the recorded patches and copies the parent-owned SDK component.
5. Builds AP/CP in a separate generated project for each requested language.
6. Verifies the generated configuration, service endpoint, image, and ELF.

Use:

```bash
python3 scripts/check_sources.py
python3 scripts/build_all.py
```

Direct vendor builds require a prepared project:

```bash
python3 scripts/prepare_vendor.py --language zh-CN
```

Then execute the full `make` command printed by the script. The explicit
`SDK_DIR` points to the patched generated AVDK, not the clean source submodule.

Generated workspaces and firmware are not source inputs and must not be committed.

## Updating Dependencies

For an AVDK, Solution, agent-studio-device-firmware, or AOSL update:

1. Select and review exact source commits.
2. Recheck the SDK port contract and the target RTSA ABI.
3. Rebase or remove every vendor patch and update its digest.
4. Update the gitlink, source revision records, and build metadata together.
5. Rebuild Chinese and English AP/CP from a clean generated workspace.
6. Verify the combined image, OTA package, map, architecture, and memory use.
7. Complete board regression before accepting the update.

Never use `git submodule update --remote` for normal development.

## Licensing and Validation

The submodules, SDK snapshot, AOSL, Agora RTSA, LVGL, fonts, and audio assets
retain their own licenses or commercial terms. AOSL has conditions in addition to
Apache-2.0. RTSA is proprietary and has no standalone redistribution grant in this
repository.

A successful build does not establish target behavior. Wi-Fi, audio/AEC, display,
touch, video, reset, and power-loss behavior require explicit device validation.
