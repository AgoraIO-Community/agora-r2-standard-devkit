# agora-r2-standard-devkit Project (BK7259)

- [中文](./README_CN.md)

## 1. Overview

`projects/mybot` is the BK7259 AP/CP firmware integration for
[agent-studio-device-firmware](https://github.com/AgoraIO-Community/agent-studio-device-firmware). It combines
device-service control, bidirectional RTC audio, local provisioning and UI, and
optional one-way encoded video uplink in one conversation. Video is enabled by default.

The `agora-r2-standard-devkit` firmware targets the Yunqu V1.0 R2 board with a
GC9503CV MIPI LCD and KEY1/GPIO47. It uses three analog microphone
inputs and an appended digital AEC reference. The product lives in the
parent repository; official BK dependencies receive versioned patches in isolated build copies.
Its normal UI renders a gray pixel-glasses face.

The vendored SDK pins [agent-studio-device-firmware v1.3.0](https://github.com/AgoraIO-Community/agent-studio-device-firmware/tree/32ac74bc59cb72db70e9a68337753561b59b13d0)
at commit `32ac74bc59cb72db70e9a68337753561b59b13d0`, with the existing HTTPS request/response body logging
patch already applied. The [recorded patch](../../patches/agent-studio-device-firmware/0001-bk7259-https-debug-log.patch) identifies
this target-specific delta; the firmware build does not apply it again. Platform interfaces,
RTC and media behavior retain the preceding snapshot's integration. MCP control over RTM requires one `message.user`
envelope, `data_type: "message"`, and string `content` containing DeviceCommand v1. It supports
`audio.set_volume` (`params.level`: integer 0–100) and `audio.adjust_volume` (`params.delta`:
integer -100–100). Positive deltas increase volume, negative deltas decrease it, and zero leaves
it unchanged; the result is clamped to 0–100. Bare commands, `audio.set_mute` and emotion commands
are not executed over RTM. Sender/channel/conversation/generation checks remain in effect;
`issued_at`/`expires_at` are optional and ignored, with no clock or expiry check.
The current session deduplicates the latest 16 distinct `command_id` values
in FIFO order: duplicates do not refresh their position or repeat a relative adjustment,
new IDs evict the oldest, and evicted IDs may execute again. MCP volume set/adjust routes
through the existing volume adapter; the command path still needs hardware validation.
Local expressions and existing voiceprint/phase indicators remain supported; cloud MCP
emotion input is disabled in this snapshot. Local LCD expressions do not trigger EB521
UART/gimbal movement.

The current audio comparison build sets `# CONFIG_MYBOT_GIMBAL is not set` in the
AP defconfig. The gimbal module, its UART2 pin mapping and CLI registration are
disabled. The following commands are available only after enabling
`CONFIG_MYBOT_GIMBAL=y` and rebuilding.

Enter `ap_cmd gimbal_cmd` on the log console to send one manual UART2 command.
Each accepted call advances through pan -30 degrees, pitch down 10 degrees,
pitch up 20 degrees and pan right 10 degrees relatively, then starts again at
the first command. TX/RX bytes and CRC-checked ACKs are logged. A busy call does
not advance the list. After a timeout, another call sends the next command;
commands are never retried automatically. The first manual call suspends
automatic centering until platform restart; local expressions and phase updates
continue to update the LCD independently.

Use `ap_cmd gimbal_scene happy`, `sad`, `angry` or `surprised` to execute one
complete existing gimbal trajectory and return to center. Without a name,
`ap_cmd gimbal_scene` selects the next scene in that order, advancing only when
the request is queued. Each scene first reads fresh controller position and
checks that homing has finished and both axes are idle; it does not move when
status feedback is missing or invalid. Scene and raw commands share one UART
owner and return busy while another operation or conversation is active.
Logs report scene start, completion, cancellation or failure. No timed commands
are sent.

Binding-status polls and stop notifications now run on a separate `service_mpq`,
so slow background requests do not delay local hangup or the next conversation
trigger. Conversation start, pairing-code and token-renewal requests remain
synchronous; SDK shutdown drains the background worker. The HTTPS adapter
supports independent connections and reads socket errors per connection.

SDK logs no longer use the 512-byte RTM preview or fixed 64-byte RTC error
storage; borrowed RTC error text is copied while valid and full content reaches
the logging layer. The target AOSL HAL's 8 KiB whole-line buffer may still
truncate larger lines. Ordinary RTM logs remain DEBUG, hidden at default NOTICE.

## 2. AP and CP Roles

| Core | Responsibility |
| --- | --- |
| AP (`bk7259_ap`) | Owns the product control loop, APSTA provisioning, agent-studio-device-firmware and AOSL lifecycle, HTTP(S)/RTC/RTM, audio, LCD, keys, camera capture, H.264 encoding, and video uplink. |
| CP (`bk7259`) | Initializes the BK runtime and starts the AP system. |

The `bk7259` project target builds and packages both cores.

## 3. Build

From the `agora-r2-standard-devkit` repository root, use the pinned official BK repositories and managed
build entry point:

```bash
python3 scripts/check_sources.py
python3 scripts/build_all.py
```

The default is a clean bilingual AP/CP build. Select one language with `--language zh-CN`
or `--language en-US`; use `--no-clean` only for incremental builds. Only 60 ms audio packet
time is supported. To invoke vendor `make` directly, first prepare the matching generated project:

```bash
python3 scripts/prepare_vendor.py --language zh-CN
```

Then run the complete command printed by that tool. Its explicit `SDK_DIR` points to the
patched generated AVDK, not the clean official checkout. Do not edit generated vendor copies.
See [official dependencies](../../docs/OFFICIAL_DEPENDENCIES.md) for versions and patch provenance.

Device-service REST requests use HTTPS by default (`CONFIG_MYBOT_HTTPS=y`). To
select HTTP, use `# CONFIG_MYBOT_HTTPS is not set` in
`projects/mybot/ap/config/bk7259_ap/defconfig` and rerun the managed build.
Restore `CONFIG_MYBOT_HTTPS=y` and rerun it to return to HTTPS.
Both modes use the same language-specific hostname and `/api` path:
`botstation.shengwang.cn` for Chinese and `botstation.agora.io` for English.
The build maps this option to SDK `MYBOT_ENABLE_HTTPS` and inverse
`MYBOT_ALLOW_INSECURE_HTTP`; RTC/RTM and the local HTTP provisioning portal are
unaffected. The existing ISRG Root X1 + DigiCert Global Root G2 trust store remains unchanged;
it does not cover a server chain requiring GoDaddy G2.

The main outputs are:

```text
<generated-project>/build/bk7259/mybot/package/all-app.bin
<generated-project>/build/bk7259/mybot/package/app_pack.rbl
```

The default generated projects are `build/build-bk7259-zh-CN-video/solution/projects/mybot/`
and the matching `en-US` directory. Merged images are published to
`releases/bk7259-<language>-video.bin`; HTTP builds add `-http`, and builds without video
omit `-video`. The per-core ELF files are under `build/bk7259/mybot/bk7259_ap/` and
`build/bk7259/mybot/bk7259/` relative to the project directory.

## 4. Startup

AP initializes the BK runtime and media service, then creates the `mybot_app`
product task. The task derives the stable `BK7259-...` ID from the chip UID,
initializes AOSL, and prepares the product-owned LCD, network manager, and key,
in that order.
It checks `mybot.factory_reset.v1` before platform registration, Wi-Fi setup, or
SDK startup: a valid marker resumes reset; a reported marker read/validation
error displays reset failure and waits for a fresh long press.

Without a reset marker, AP registers the platform and tries saved Wi-Fi. Missing
or unusable credentials lead to APSTA provisioning at `http://192.168.4.1/`.
After STA obtains IPv4 and the portal stops, the task starts the SDK for device
authentication and pairing as needed. Pairing codes are entered in the web
console. The product loop polls SDK state and handles key requests.

Network/SDK failures retain the prepared key and display for factory-reset
requests. Identity, platform-preparation, or descriptor-registration failures
can exit startup; that recovery path is not guaranteed for those earlier errors.
Descriptor-registration failure shuts down the prepared platform resources.

## 5. Keys and Factory Reset

| KEY1/GPIO47 action | Behavior |
| --- | --- |
| Short press and release | `READY`: start conversation; `IN_CONVERSATION`: stop conversation; other states ignore it |
| Hold for 5 seconds | Request factory reset once; release does not toggle conversation |
| Already held at platform preparation | Release first to arm a new gesture |

The active-low key uses 20 ms scanning and two-sample debounce. Long press
works after successful platform preparation, including provisioning and before
SDK key attachment. A key held while the platform prepares must first be
released to arm a new gesture. Five seconds is the gesture threshold; actual
completion also depends on SDK and driver teardown. The CEN button remains
hardware reset only; double-press, volume, and other key actions are not registered.

Normal conversation hangup stops local video/RTC and clears session PCM before
publishing READY and sending one HTTP(S) stop notification. Notification failure
does not restart media or retry. Stop notifications and binding polls run on the separate
`service_mpq`, so a slow background request does not block the next conversation trigger.

The key timer posts requests; `ap_main.c` executes reset in the product task.
Its order is:

1. Show restoring status and persist `mybot.factory_reset.v1`.
2. Stop the SDK and require `mybot_is_running() == false` and `STOPPED`.
3. Stop the portal/SoftAP, scan, and STA; require completed teardown.
4. Delete and verify absence of `mybot.device_auth`, `mybot.wifi.v1`, and
   `mybot.volume.v1`, then delete the reset marker last.
5. Show rebooting status and restart; normal startup enters Wi-Fi provisioning.

SDK and provisioning writers stop before deletion, including the volume
adapter's final persistence attempt. Only these product records are cleared;
the UID-derived device ID stays the same and no server-side unbind request is
sent. The next SDK startup uses the default speaker volume and registers/authenticates again according to
the server's retained binding state.

Failure displays a bilingual retry page and waits for a fresh five-second hold.
Incomplete cleanup keeps the established marker; a final marker-deletion error
attempts to restore it. Boot resumes valid pending resets before starting the
network or SDK. A reported marker error requires an explicit new long press.
The marker is written when the product task processes the request; loss of
power before that point does not establish a pending reset. Failed network
teardown keeps its resources owned for retry. Network cancellation returns control to the owner, which verifies all
teardown results before deletion. The existing LCD worker retains restoring/
failed/rebooting pages across SDK stop/detach.

Module declarations are independent of the platform abstraction:

- `platforms/bk7259/include/bk7259_key.h`: conversation request and event dispatch.
- `platforms/bk7259/include/bk7259_factory_reset.h`: reset requests, persistent
  intent/cleanup, network teardown, and reset display phases.
- `platforms/bk7259/include/mybot_bk7259_platform.h`: platform registration and
  preparation abstraction.

These paths are relative to `components/mybot/mybot_sdk/`. The first two APIs
use `bk7259_*` names. Reset storage is implemented in `bk7259_factory_reset.c`;
it does not erase the whole EasyFlash environment.

## 6. R2 Audio and Display

The current audio pipeline is configured in
[bk7259_audio.c](../../components/mybot/mybot_sdk/platforms/bk7259/bk7259_audio.c),
with board pin settings in [usr_gpio_cfg.h](ap/config/bk7259_ap/usr_gpio_cfg.h).
Capture enables three differential analog ADC inputs (`ch_bitmap=0x7`, `chl_num=3`),
and `adc_cfg.aec_en=3` appends a digital playback reference:

| Lane | Algorithm input | Analog gain | Digital gain |
| --- | --- | --- | --- |
| 0 / ADC0 | Microphone input 1 | 16 dB | 4 dB |
| 1 / ADC1 | Microphone input 2 | 16 dB | 4 dB |
| 2 / ADC2 | Microphone input 3 | 16 dB | 4 dB |
| 3 / appended | Digital playback reference | Not applicable | Not applicable |

DMIC is disabled and `hw_ref_ch=NONE` (`255`); no ADC lane is an in-channel reference.
Capture uses 640-byte/20 ms blocks and two blocks of buffering.

```text
onboard_mic_stream_v2 (ADC0 + ADC1 + ADC2 + appended digital reference)
  -> aec_v3_algorithm_v2 (hardware AEC, APPEND/AUTO reference, three-microphone processing)
  -> raw stream -> 16 kHz mono signed 16-bit PCM -> RTC uplink
```

AEC uses `adc_ch_num=3`, `dual_ch=2`, traditional noise suppression, VAD disabled
(`vad_enable=0`), `ec_only_output=1`, and `multi_output_use_ec_out=1`.
The paired AVDK supplies three-microphone deinterleaving, the trimic flag and SSL3
support. Its processing path still calls `aec_proc`; the Solution comment saying
AEC is temporarily disabled is stale. The wrapper, configuration/algorithm headers,
AP algorithm library and matching checksum manifest are maintained in
`patches/bk_avdk_smp/0003-r2-trimic-aec.patch`. The archive retains the previous R2
`20261014` version, and library verification remains enabled.
No ASR output port or UI direction callback is added.

GPIO12 uses the output/pull-down configuration in `usr_gpio_cfg.h` for discharging the
ADC internal comparator. GPIO34/35 use its secondary-function-disabled
GPIO-output declarations and remain HIGH-Z at startup. GPIO2/3 UART2 mapping is
enabled only with `CONFIG_MYBOT_GIMBAL=y`; the current comparison build disables
the gimbal module, mapping and CLI. GPIO47 remains the pulled-up conversation key.
There are no runtime capture-level logs. Actual three-input capture and AEC
behavior still require device testing.

Speaker playback uses the onboard DAC stream and active-high
GPIO50 amplifier enable. Volume changes apply speaker digital gain and save
`mybot.volume.v1` synchronously; failures retry on a later set or adapter
destruction. This board has no volume keys.

The GC9503CV panel is native 480x640 with a logical 640x480 RGB565 UI. GPIO20
enables the 3.3 V rail, GPIO29 the shared LCD/camera 2.8 V rail, GPIO42 resets the
panel, and GPIO40 drives its active-low backlight. GPIO27 drives high during
platform preparation to enable the external camera 1.2 V rail. The LCD
board-power owner retains both rails until full platform shutdown;
conversation/video teardown resets the sensor through GPIO30 without cutting
board power or blanking the display. The existing UI worker renders both SDK screens and
bilingual restoring/failed/rebooting pages; reset status takes precedence over
SDK stop/detach updates.

The LVGL 9.5.0 pixel-glasses face is implemented in
[lvgl_view.cc](../../components/mybot/mybot_sdk/platforms/bk7259/display/lvgl_view.cc)
and [pixel_expression_shapes.h](../../components/mybot/mybot_sdk/platforms/bk7259/display/pixel_expression_shapes.h): a fixed `#050708` black glass background and gray decorations
avoid visible dark-gradient banding in RGB565. The neutral
idle face uses `#f1f1ee` off-white glasses, two curved eyes, and a smile. The
greeting uses `#f1ca79` warm-gold glasses, a curved left eye, a round right eye,
and a smile.

| Product stage | Display |
| --- | --- |
| Startup, provisioning, authentication, or pairing | Bilingual operational guidance; pairing codes are entered in the web console |
| Actual SDK `READY` | Neutral idle face by default |
| Entering `IN_CONVERSATION` | SURPRISED for 300 ms after the entering transition, then the latest server phase |
| Conversation LISTENING | LISTENING face |
| Conversation THINKING | THINKING face; PROCESSING after three continuous seconds |
| Conversation SPEAKING or no active phase | NEUTRAL face |
| Hangup returning to `READY` | Neutral idle face |
| Network/startup error | Operational error guidance |
| Factory reset | Restoring, failed, or rebooting page, with priority over SDK display updates |

Wi-Fi provisioning does not change SDK readiness or bypass pairing. Normal
idle and conversation screens retain four terminal lines. In READY:

```text
> OIM / R2
> mood: neutral
> state: ready
> agora / realtime
```

The mood uses the selected expression's lowercase name independently of SDK
state. The third line reports the actual state. During a conversation it shows
`> state: listening | vp:pending`, with the phase following SDK
`listening`/`thinking`/`speaking` indicators, prioritizing speaking, then thinking,
then listening when several are active; no active phase falls back to
`in_conversation`. `vp:ok` means a successful voiceprint notification was
received in the current session; `vp:pending` means it is not yet confirmed.
Phase and voiceprint appear only when the runtime state and page are both
`IN_CONVERSATION`. Voiceprint updates affect text; phase updates also select the
underlying automatic face. Repeated phase updates do not reset its animation or
the three-second thinking clock. An active explicit expression is retained;
no toast is shown. SLEEP artwork does not turn the actual state into `standby`.
The `</>` signature and cursor on the last row remain. This UI does not control
gimbal movement.

The application publishes the SDK state through the native `bk7259_lcd.h`
copied snapshot interface from its existing 100 ms observation loop. Existing
SDK LCD callbacks supply phase and voiceprint indicators; no SDK changes or
new UI task are needed. Provisioning guidance remains visible while the SDK is
`STOPPED`. Synchronous SDK stop uses the STOPPING guidance page; after stop
returns, the application immediately publishes the observed stopped state.
Factory-reset guidance retains highest priority; its third row is
`> state: factory_reset` and omits session phase/VP fields. This label describes
the product flow, not an additional SDK state.

All 23 expressions are implemented in `bk7259_expression.h` enum order:

```text
NEUTRAL, GREETING, HAPPY, EXCITED, CURIOUS, CONFUSED, THINKING, LISTENING,
SURPRISED, SHY, PROUD, LOVE, SLEEPY, BORED, SAD, PLEADING, APOLOGY, WORRIED,
ANNOYED, REFUSE, PROCESSING, ERROR, SLEEP
```

The independent `include/bk7259_expression.h` module exposes local display
requests:

```c
#include "bk7259_expression.h"

int show_happy(void) {
    return bk7259_lcd_set_expression(BK7259_EXPRESSION_HAPPY);
}
```

`bk7259_expression_from_name()` converts an exact expression name while ignoring
ASCII letter case; `bk7259_expression_name()` returns its stable uppercase
name. `bk7259_lcd_clear_expression()` restores the latest automatic face for the
current workflow and server phase. Set/clear return `0` when the owner accepts the copied request,
not when scanout completes, and `-1` for an invalid expression or an unavailable
face page. They use the existing LCD snapshot lock and mailbox, never caller-side
LVGL. Requests are accepted only on prepared, running `READY`/`IN_CONVERSATION`
pages; provisioning, reset, and shutdown reject them. Workflow screen changes
clear overrides. A local override persists through phase updates until cleared or
the workflow changes. Requesting the same local expression does not replay its transition.

Cloud MCP emotion input is disabled in the current SDK snapshot. With animations enabled,
the opener's 300 ms hold excludes its entering 250 ms transition; with animations disabled,
only the hold applies. The hold starts when the UI owner displays the request. Ordinary
renders do not restart it. At expiry the UI restores the latest automatic face, including
PROCESSING if THINKING has continued for three seconds in the background. Leaving the
conversation cancels its foreground and deadlines; operational guidance and factory reset
always take precedence.

All expressions are LCD-only and do not drive the gimbal; manual UART CLI
control remains independent when `CONFIG_MYBOT_GIMBAL=y` and is disabled in the
current audio comparison build. There is no automatic demo cycling. Selecting
the ERROR or SLEEP artwork does not enter the SDK's error state or suspend the hardware.

The renderer's 1024x896 artwork coordinate space is scaled uniformly by 9/16 to a virtual
576x504 area at logical `(32,-12)` on the 640x480 display. The cropped top and
bottom contain only empty canvas space; visible text and face geometry stay
inside the rounded-bezel safe area. The glasses span about 345 pixels.
Terminal rows use a 37-pixel artwork-coordinate grid, and the signature
is left-aligned. Fixed LVGL widgets and sampled curves draw the face. Five small
immutable A8 masks (13631 bytes in Flash) render hearts, stars, filled ellipses, cheeks, and tears
without triangle-fan seams. Their geometry is defined in the committed mask
generator; no full-face images or additional canvas buffer are required.
`CONFIG_MYBOT_LVGL_UI_ANIMATIONS` enables short
face transitions, expression motion, and a 500 ms cursor blink; there is no light-theme selector.
Rendering uses the existing owner task, a 20480-byte RGB565 draw strip, two
614400-byte scanout buffers, and an 8192-byte task stack. The configured refresh
interval is 50 ms. Internal LVGL allocations and stack peaks still need target
measurement while audio/video run.

Terminal decorations use read-only BK7259 Terminal subsets derived from
DejaVu Sans Mono Bold: 15-pixel ASCII and a 16-pixel `</>` signature, totaling
4634 bitmap bytes. These replace Unscii 16; their actual LVGL text advances
determine label widths.
The cursor follows the actual text width. Bilingual operation text uses the
existing MyBot UI Sans 20 subset. The old emoji sources and
licenses remain for provenance, but the emoji image source is no longer linked.
See [display/SOURCES.md](../../components/mybot/mybot_sdk/platforms/bk7259/display/SOURCES.md)
for mask geometry, byte verification, and resource origins.

## 7. Multimodal Video Uplink

The default AP configuration enables video (`CONFIG_MYBOT_VIDEO=y`). To disable
it, use `# CONFIG_MYBOT_VIDEO is not set` in
`projects/mybot/ap/config/bk7259_ap/defconfig` and run a clean rebuild. With video
enabled, the BK7259 video path is:

```text
MIPI CSI GC2053/CV2005 sensor (1280x720 at 5 fps)
  -> ISP MP (640x480 NV12)
  -> hardware FLEXA H.264 encoder
  -> encoded-frame pool
  -> agent-studio-device-firmware video handler
  -> Agora RTSA high video stream
```

Settings and behavior when video is enabled:

| Item | Value |
| --- | --- |
| Encoded output | H.264 Annex-B access units, 640x480 |
| Sensor input | 1280x720 at 5 fps |
| Bitrate range | 256000-512000 bit/s; initial midpoint 384000 bit/s |
| GOP | 30 frames, approximately 6 seconds at 5 fps; RTSA key-frame requests can force an IDR |
| Lifecycle | Capture and encoding start only after RTC reaches `CONNECTED`; they stop before leaving RTC |
| Direction | Device uplink only; remote video subscription is disabled |
| RTSA timing | `frame_rate=0`, so RTSA uses real frame timestamps instead of synthesizing a fixed cadence |

RTSA bandwidth estimates are clamped to the configured bitrate range and
applied to the hardware encoder. The audio path remains bidirectional while
video is uplink-only.

Video configuration is defined in:

- `components/mybot/mybot_sdk/Kconfig`
- `projects/mybot/ap/config/bk7259_ap/defconfig`

After changing video configuration, run a clean rebuild so the generated
configuration cannot retain stale values.

## 8. Real-Device Validation

The user confirmed R2 LCD bring-up, conversation start/stop without blanking,
the previous dual-analog-microphone/ADC2-reference audio/AEC, and factory reset
work on hardware. The current three-input audio configuration awaits device testing.
The user confirmed basic process-gray pixel-glasses rendering. The restored
four-line layout and complete 23-expression set await hardware validation.
The checklist below describes regression coverage; it does not imply every
fault or power-interruption scenario has already been tested on the device.
Video-specific checks require a build with `CONFIG_MYBOT_VIDEO=y`.

- [ ] Cold boot the combined `all-app.bin` and confirm both AP and CP start.
- [ ] Confirm the boot log reports `camera 1.2V power enabled: gpio=27 level=1` before sensor detection; verify GPIO27 and the external 1.2 V rail on the new camera hardware.
- [ ] Confirm the installed GC2053 or CV2005 camera is detected and the source reports 1280x720 at 5 fps with 640x480 H.264 output.
- [ ] Start a conversation and verify audio in both directions plus H.264 video at the remote endpoint.
- [ ] Verify camera capture and encoding start only after RTC is connected and emit no frames after conversation teardown begins.
- [ ] Measure approximately 5 fps, a 256-512 kbit/s encoder rate, and the initial 384 kbit/s target.
- [ ] Verify a periodic IDR about every 6 seconds and that an RTSA key-frame request forces a new IDR.
- [ ] Exercise constrained Wi-Fi and confirm RTSA bitrate updates stay within 256-512 kbit/s without breaking audio.
- [ ] End, restart, and interrupt conversations repeatedly; confirm the encoder, frame pool, camera, and RTC resources stop and restart cleanly.
- [ ] Confirm conversation/video stop holds GPIO30 in reset while GPIO27/1.2 V and GPIO29/2.8 V remain enabled; only full platform shutdown releases the board rails.
- [ ] Short press/release starts and stops a conversation without blanking the LCD.
- [ ] Verify NEUTRAL after actual `READY`, a 300 ms SURPRISED opener plus the entering transition, then the latest phase face; hangup returns to neutral.
- [ ] Verify LISTENING, THINKING → PROCESSING after three continuous seconds, and NEUTRAL during SPEAKING or no active phase; repeated updates must not reset the thinking clock.
- [ ] Verify four terminal rows, lowercase expression names, actual SDK state, and session-scoped phase/voiceprint text; verify all 23 faces and their motion against the [expression mappings](../../components/mybot/mybot_sdk/platforms/bk7259/display/SOURCES.md#expression-mappings).
- [ ] Exercise persistent local set/clear, invalid names/IDs, phase updates, workflow changes, and reset priority; clear must restore the latest automatic face.
- [ ] Test MCP `audio.set_volume` at 0, 50 and 100 through the `message.user` envelope; bare commands, mute and emotion commands must have no effect.
- [ ] Test wrapped `audio.adjust_volume` with positive, negative and zero deltas, including -100 and 100; results must clamp to 0–100, and repeating a command ID must not adjust the volume again.
- [ ] Verify phase/voiceprint text appears only during a real conversation, voiceprint-only updates preserve the face, and hangup removes session fields and cancels pending expression deadlines.
- [ ] Verify both languages, web-console pairing instructions, round-bezel clearance, and factory-reset display priority.
- [ ] A hold below 5 seconds performs only the state-appropriate short action on release; a 5-second hold resets once and suppresses that action.
- [ ] Hold the key across reboot, then release; confirm a new gesture is required before another reset.
- [ ] Request reset during conversation, saved-STA connection, and APSTA provisioning; confirm shutdown precedes record deletion and the reset page remains visible.
- [ ] Confirm Wi-Fi credentials, local authentication, and saved volume are cleared, while device ID and server-side binding are preserved.
- [ ] Interrupt power after the reset marker is persisted; confirm boot resumes cleanup before network/SDK startup.
- [ ] Inject shutdown/storage failures; confirm the failure page waits for a fresh 5-second hold rather than rebooting or erasing while writers remain active.

See [UI_VALIDATION.md](./UI_VALIDATION.md) for build and host-test evidence.
