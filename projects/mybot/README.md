# agora-r2-standard-devkit Project (BK7259)

- [简体中文](./README_CN.md)
- [Repository overview](../../README.md)
- [Validation status](./UI_VALIDATION.md)

## Scope

This directory contains the BK7259 product integration for
[agent-studio-device-firmware](../../README.md) on the Yunqu V1.0 R2 board. It
owns board initialization, APSTA/STA networking, local peripherals, product
policy, and the SDK lifecycle boundary.

agent-studio-device-firmware owns device-service registration, authentication, pairing,
conversation state, RTC/RTM, and voiceprint handling. Agora provides the RTC
transport. Cloud device services and AI agents are external dependencies.

Build commands, artifact paths, configuration files, and third-party licensing
are documented in the [repository overview](../../README.md) and
[dependency guide](../../docs/OFFICIAL_DEPENDENCIES.md).

## AP and CP

| Core | Responsibility |
| --- | --- |
| CP (`bk7259`) | Initializes the BK runtime and starts the AP system. |
| AP (`bk7259_ap`) | Runs the product task, networking, SDK lifecycle, audio, display, touch, keys, camera, and H.264 encoding. |

The `bk7259` project target builds and packages both cores.

## Startup and Ownership

The AP entry point is [ap_main.c](./ap/ap_main.c):

1. Initialize the BK runtime and media service.
2. Create the `mybot_app` product task.
3. Derive the device ID from the chip UID and build runtime configuration.
4. Initialize AOSL and prepare the product-owned display, Wi-Fi manager, and key.
5. Resume a pending factory reset, restore saved Wi-Fi, or enter APSTA provisioning.
6. Start the SDK only after STA IPv4 connectivity is available.
7. Poll SDK state and process conversation, Wi-Fi setup, and reset requests.

Wi-Fi is owned by the product layer. The SDK adapter only observes connectivity
and forwards serialized state transitions. Entering Wi-Fi setup always stops the
SDK first; the product does not start the portal while SDK services are running.

Network or SDK failures retain the prepared display and controls so the user can
retry setup or factory reset. Identity or early hardware-preparation failures can
terminate the product task because the required platform is unavailable.

## Controls and Network Setup

| KEY1/GPIO47 action | Behavior |
| --- | --- |
| Short press and release | `READY`: start conversation; `IN_CONVERSATION`: stop conversation |
| Other SDK states | Ignore the short press |
| Long press | Power on/off is handled by the current R2 hardware |
| Held during platform preparation | Release before using a new gesture |

The key is active low, scanned every 20 ms, and debounced with two matching
samples. `KEY_SHORT_PRESS_LIMIT_MS=5000` filters prolonged presses; it does not
define the hardware power-button timing. The CEN button remains hardware reset.

Touch **Settings** to open Wi-Fi setup or factory reset.

- Wi-Fi setup requires SDK `STOPPED` and `mybot_is_running() == false`.
- The device starts an open `mybot-xxxx` hotspot and serves the setup page at
  `http://192.168.4.1/`.
- Credentials are saved only after the candidate network obtains IPv4.
- Cancellation is accepted until credential saving starts and then reconnects the
  saved network once.
- Successful setup restarts the SDK without rebooting or clearing identity,
  binding, or volume.

Factory reset is owned by the product task:

1. Persist `mybot.factory_reset.v1`.
2. Stop the SDK and require the `STOPPED` state.
3. Stop the portal, SoftAP, scans, and STA.
4. Delete `mybot.device_auth`, `mybot.wifi.v1`, and `mybot.volume.v1`.
5. Verify deletion, remove the reset marker, and reboot.

The reset does not erase the whole EasyFlash environment, change the UID-derived
device ID, or unbind the device from the service. A failed cleanup keeps the
marker and waits for an explicit Retry action. Board-level details are in
[usr_gpio_cfg.h](./ap/config/bk7259_ap/usr_gpio_cfg.h).

## Audio

The SDK receives 16 kHz, mono, signed 16-bit PCM. The R2 capture path uses three
differential analog ADC inputs and appends the digital playback signal as a
fourth AEC reference lane.

```text
ADC0 + ADC1 + ADC2 + appended playback reference
  -> AEC v3
  -> 16 kHz mono S16 PCM
  -> SDK RTC uplink
```

Capture blocks are 640 bytes (20 ms) with two buffered blocks. VAD is disabled;
traditional noise suppression and EC-only output are enabled. The paired AVDK
patch supplies the three-input handling and AEC library integration.

Playback uses the onboard DAC stream with GPIO50 enabling the active-high speaker
amplifier. SDK volume control maps 0-100 to speaker digital gain and persists the
value in `mybot.volume.v1`. There are no volume keys on this board.

The current three-input configuration still requires target-board validation for
capture quality, AEC behavior, and repeated start/stop.

## Display

The product owns a GC9503CV MIPI panel from platform preparation through shutdown,
so provisioning and reset pages remain visible while the SDK is stopped. The native
480x640 panel is rendered as a 640x480 logical RGB565 surface.

The board-power owner controls:

- GPIO20: LCD 3.3 V
- GPIO29: shared LCD/camera 2.8 V
- GPIO27: camera 1.2 V
- GPIO40: active-low backlight
- GPIO42: panel reset

Conversation teardown retains these rails and resets the camera through GPIO30.

The LVGL 9.5.0 UI follows the product's process-gray pixel-glasses design. It
shows provisioning, pairing, ready, conversation phase, error, and factory-reset
pages with Chinese or English guidance. During a conversation it displays
listening, thinking, or speaking state and voiceprint status from SDK indicators.
All 23 local expressions are available without driving the optional gimbal.

The display owner applies copied snapshots on one UI task and submits completed
RGB565 frames through the DSI panel/DPU path. SDK LCD init/destroy only attach and
detach; SDK stop does not destroy the product display.

Resource provenance is recorded in
[display/SOURCES.md](../../components/mybot/mybot_sdk/platforms/bk7259/display/SOURCES.md).
Touch and display tests are under [tests/touch](./tests/touch/README.md) and
[tests/touch_ui](./tests/touch_ui/README.md).

## Video

Video is enabled in the default AP configuration. The uplink path is:

```text
GC2053/CV2005 MIPI CSI sensor, 1280x720 at 5 fps
  -> ISP MP scaling to 640x480 NV12
  -> hardware FLEXA H.264 encoder
  -> complete Annex-B access units
  -> SDK video handler
  -> Agora RTC uplink
```

The encoder bitrate range is 256000-512000 bit/s with a 384000 bit/s initial
target. RTSA bandwidth updates adjust the encoder; a key-frame request forces an
IDR. Video starts only after RTC reaches `CONNECTED` and stops before RTC leaves.
The device does not subscribe to or render remote video.

Configuration and source are in
[defconfig](./ap/config/bk7259_ap/defconfig) and
[bk7259_video.c](../../components/mybot/mybot_sdk/platforms/bk7259/bk7259_video.c).

## Optional Feature Notes

- [DeviceCommand behavior](../../docs/mybot-sdk-device-command-plan.zh-CN.md)
- [EB521 gimbal and UART](../../docs/eb521-gimbal-plan.zh-CN.md)
- [Validation status](./UI_VALIDATION.md)
- [UI screenshots](../../docs/images/r2-user-guide/README.md)

The EB521 gimbal is disabled in the default AP configuration. Host tests still
exercise the retained implementation. Device audio, video, touch, power-loss, and
long-duration behavior are not implied by a successful host build.
