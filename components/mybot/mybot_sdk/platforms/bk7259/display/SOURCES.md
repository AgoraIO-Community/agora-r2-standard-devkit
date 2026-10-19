# agora-r2-standard-devkit LVGL pixel-glasses view

## Current renderer

The pixel-glasses view is implemented in [lvgl_view.cc](lvgl_view.cc), with
expression geometry, colors, and motion in
[pixel_expression_shapes.h](pixel_expression_shapes.h). The expression IDs and
local control API are declared in
[bk7259_expression.h](../include/bk7259_expression.h); automatic face selection
and timing are defined in [expression_policy.h](expression_policy.h).

The LCD uses a fixed `#050708` black glass background and gray decorations,
avoiding visible dark-gradient banding in RGB565. Neutral idle uses the
`#f1f1ee` accent, glasses, two curved eyes, and a smile. Greeting
uses `#f1ca79`, with a curved left eye, round right eye, and a smile. The
1024x896 artwork coordinate space is scaled uniformly by 9/16 into a virtual
576x504 area at `(32,-12)` on the R2 panel's logical 640x480 display (native
GC9503CV 480x640).
Only empty canvas space is cropped above and below the display; visible face
geometry and text remain within its rounded-bezel safe area. The glasses span
about 345 pixels. Fixed LVGL widgets and sampled curves draw the face. Five
small immutable A8 masks render filled hearts, stars, ellipses, cheeks, and
tears without triangle-fan seams. Their geometry and rasterization are defined
in [generate-expression-masks.py](generate-expression-masks.py); no full-face
image or canvas buffer is required.

The renderer retains four terminal rows. In READY:

```text
> OIM / R2
> mood: neutral
> state: ready
> agora / realtime
```

The mood uses the selected expression's lowercase name independently of the
actual SDK state. The third row reports that state. In a conversation it uses
`> state: listening | vp:pending`, with SDK listening/thinking/speaking phase
indicators, priority speaking > thinking > listening, and an `in_conversation`
fallback when no phase is active.
`vp:ok` means the current session has received a successful voiceprint
notification; `vp:pending` means it has not yet been confirmed. These session
fields appear only when both runtime state and screen are IN_CONVERSATION.
SLEEP artwork does not change the actual state to `standby`.
The four rows use source position `x=104` and
`y=108 + 37*i`, preserving the 37-pixel row grid. The `</>` signature at
`(795,112)` remains left-aligned. The 500 ms cursor follows the last row using
the actual fixed-width font advance.

`READY` selects NEUTRAL. Entering `IN_CONVERSATION` shows SURPRISED for 300 ms
after its entering transition, then selects LISTENING or THINKING for those
server phases, PROCESSING after three continuous seconds of THINKING, and
NEUTRAL during SPEAKING or when no phase is active.
The independent `include/bk7259_expression.h` module provides the full 23-entry
expression enum, name conversion, and copied local set/clear requests to the
existing display owner. Overrides are limited to READY/conversation face pages
and are cleared by workflow screen changes. Phase updates select the latest
automatic face while preserving local overrides; repeated unchanged updates do
not restart its transition. Voiceprint and listening/thinking/speaking
indicators update the third terminal row rather than showing an overlay. Chinese and English
provisioning, web-console pairing, error, and factory-reset guidance remain
available. Expressions are not automatically cycled and have
no MCP/RTM command mapping or UART/motor integration.

## Expression mappings

These mappings are defined by the expression configuration table in
`pixel_expression_shapes.h`. NEUTRAL uses HAPPY's double-arc geometry with an
off-white accent. Colors below are RGB values converted to the target RGB565
format.

| ID/name | Label | Eyes; mouth | Accent |
| --- | --- | --- | --- |
| NEUTRAL | 中性待机 | arc/arc; smile | `#f1f1ee` |
| GREETING | 打招呼 | arc/dot; smile | `#f1ca79` |
| HAPPY | 开心 | arc/arc; smile | `#f1ca79` |
| EXCITED | 兴奋庆祝 | star/star; open | `#ffd276` |
| CURIOUS | 好奇 | dot/capsule; dot | `#f1f1ee` |
| CONFUSED | 疑惑没听清 | slant/dot; wave | `#f1f1ee` |
| THINKING | 专注思考 | dash/dot; side | `#8fcbdc` |
| LISTENING | 认真聆听 | dot/dot; soft | `#f1f1ee` |
| SURPRISED | 惊讶 | capsule/capsule; open | `#f1f1ee` |
| SHY | 害羞 | arc/arc; tinySmile, cheeks | `#e3a6b8` |
| PROUD | 得意 | slantUp/slantUp; smirk | `#f1f1ee` |
| LOVE | 喜欢被摸头 | heart/heart; smile | `#ef9fb5` |
| SLEEPY | 困倦 | dash/dash; yawn | `#9caccc` |
| BORED | 无聊 | dash/dash; flat | `#f1f1ee` |
| SAD | 失落 | down/down; frown | `#9caccc` |
| PLEADING | 委屈撒娇 | capsule/capsule; tinyFrown, tear | `#9ccce6` |
| APOLOGY | 抱歉 | down/down; tinyFrown | `#f1f1ee` |
| WORRIED | 担心 | dot/dot; wave | `#e6c17e` |
| ANNOYED | 轻微不满 | slant/slant; flat | `#c8b198` |
| REFUSE | 拒绝做不到 | x/x; flat | `#c0b6a3` |
| PROCESSING | 处理中 | orbit/orbit; dots | `#8fcbdc` |
| ERROR | 错误 | vertical/vertical; flat | `#e57876` |
| SLEEP | 休眠 | dash/dash; none | `#7588a5` |

Enabled expression effects are implemented in `pixel_expression_shapes.h`:
EXCITED alternates eye bounce, THINKING/BORED drift, PROCESSING moves its orbit eyes, SLEEPY animates
the yawn, and SLEEP dims and breathes. Brightness multipliers
apply to SLEEPY, SAD, APOLOGY, and SLEEP; SURPRISED/ERROR retain their brief
entry flash. ERROR and SLEEP are display artwork only, with no change to SDK
error, connectivity, or hardware power state.

LISTENING shifts the whole face and terminal actor down by 18 source pixels, and
SAD/APOLOGY/SLEEPY shift it by 28. All expressions keep the display upright.
Expression motion is limited to the LCD and does not drive hardware movement.

Two fixed shape banks, each with 16 leaves, support a 250 ms crossfade while
sharing the glasses frame and terminal decorations. Their geometry lives in
the private `pixel_expression_shapes.h` renderer helper. A single timer drives
expression motion at no more than 20 fps and static cursor blinking at 500 ms;
disabling `CONFIG_MYBOT_LVGL_UI_ANIMATIONS` retains static expression artwork.
The mask data is read-only. There is no runtime canvas or full-frame rotation
layer; recoloring and opacity are applied through public LVGL image drawing.

The application's existing 100 ms observation loop copies the current SDK
state through the native `include/bk7259_lcd.h` snapshot interface. Existing
SDK LCD callbacks provide phase/voiceprint indicators; their publication
shares the display snapshot lock and owner mailbox. State updates do not
change the workflow screen, clear expression overrides, or restart animations.
SDK STOPPED does not hide provisioning guidance. Synchronous SDK stop retains
the existing STOPPING guidance page, and the application republishes the
observed stopped state after stop completes. Reset guidance retains priority;
its third row is `> state: factory_reset` without session phase/VP fields.
This is a product-flow label, not an additional SDK state, and ordinary status
updates cannot replace it.

All view and timer operations run on the existing display owner task. The view
never owns the panel or framebuffer. Reset restoring, failed, and rebooting
presentations retain priority over SDK updates and detach. The existing RGB565
draw strip and two scanout buffers are reused; there is no new UI task or
full-frame canvas allocation.

## Geometry masks

[pixel_expression_masks.h](pixel_expression_masks.h) contains five immutable,
uncompressed A8 masks generated by the target-owned MIT-licensed
`generate-expression-masks.py`. The generator defines heart and star contours
and ellipse dimensions in its `shapes()` function, then samples at four times
the R2 9/16 scale and downsamples with Lanczos. The recorded generation tool is
Pillow 10.2.0. The generated output retains its MIT notice and copyright.

| Mask | Target dimensions | A8 bytes |
| --- | --- | --- |
| Heart | 51x48 | 2448 |
| Star | 77x74 | 5698 |
| Open-mouth ellipse | 49x59 | 2891 |
| Cheek | 67x29 | 1943 |
| Tilted tear | 21x31 | 651 |
| Total | | 13631 |

The masks occupy read-only Flash; descriptors and alignment add a small
amount of metadata. Public `lv_draw_image()` draws each mask with the current
expression's recolor and opacity. Yawning scales the ellipse descriptor
directly; vendor image scaling may allocate temporary scratch for the clipped
draw strip, so this does not imply zero runtime LVGL allocation. Stable dim
stroke colors are blended against the fixed `#050708` glass color before
opaque drawing, avoiding brighter overlaps between rounded line segments.
The 250 ms bank transition still applies its separate fade opacity.
This replaces independently antialiased triangles whose interior
edges produced seams, without full-expression images, a canvas, or a new
framebuffer. LVGL's existing draw strip and scanout buffers remain unchanged.

From this display directory, check the committed bytes using Pillow 10.2.0:

```bash
python3 generate-expression-masks.py --check
```

`--check` reconstructs all five masks from the geometry in the script and
compares the generated header without writing it. Pillow is a development tool;
the firmware build uses the committed header.

## Retained view and asset provenance

The preceding platform-independent status view and generated resources were
adapted from [agora-esp32-devkit commit `19af3bdfaf1dd35461af48def8f9558e3503e755`](https://github.com/AgoraIO-Community/agora-esp32-devkit/tree/19af3bdfaf1dd35461af48def8f9558e3503e755).

The view's MIT license, original layout reference, and copyright notice remain
in `assets/licenses/LVGL_VIEW_LICENSE.txt`. The historical asset API header
retains its Apache-2.0 SPDX identifier. The generated font and emoji pixel data
retain SIL OFL-1.1 notices in `assets/licenses/LVGL_VIEW_FONT_LICENSE.txt` and
`assets/ui/noto_emoji/LICENSE.txt`. Emoji source hashes, upstream revision,
format, and generation details remain in `assets/ui/noto_emoji/SOURCES.json`.
The old four 64x64 ARGB8888 emoji images total 65,536 pixel bytes, but
`lvgl_assets.c` is no longer linked into the current firmware.

## Fonts

Terminal decorations use renamed BK7259 Terminal subsets derived from DejaVu
Sans Mono Bold, with generation input SHA256
`3a3c502eeff669a231549e80df9f7c49de109bafe303170409e905d0b31a38fe`.
The committed 4 bpp subsets in [pixel_fonts.c](pixel_fonts.c)
provide fixed-width ASCII terminal text and the signature:

| Subset | Glyphs | Bitmap bytes | Use |
| --- | --- | --- | --- |
| BK7259 Terminal 15 | 95 ASCII | 4479 | Four terminal rows |
| BK7259 Terminal 16 | 3 (`</>`) | 155 | Signature |

The total bitmap data is 4634 read-only bytes. Actual LVGL character advances
determine label and cursor width. [generate-pixel-font.py](generate-pixel-font.py)
records the subset sizes, character selection, and 4 bpp packing. The committed
subsets were generated with Pillow 10.2.0 / FreeType 2.13.2.
Bitstream copyright/trademark notices, the DejaVu public-domain modification
notice, and the full Bitstream Vera license are retained in
`assets/licenses/TERMINAL_FONT_LICENSE.txt`. The generated subsets use the
distinct BK7259 Terminal name.

Chinese and English operational text uses the 20-pixel, 4 bpp MyBot UI Sans
subset of Source Han Sans SC Normal. The current font contains 152 glyphs,
including ASCII and fixed Chinese UI vocabulary. Arbitrary Chinese SSIDs or
chat text require a separately extended font.

[generate-lvgl-font.py](generate-lvgl-font.py) is adapted from the MIT-licensed
[agora-esp32-devkit generator](https://github.com/AgoraIO-Community/agora-esp32-devkit/blob/19af3bdfaf1dd35461af48def8f9558e3503e755/scripts/generate-lvgl-font.py).
The R2 tool reads the fixed vocabulary in `lvgl_view.cc` and adds a coverage-only
`--check` mode. Source Han Sans SC Normal has generation input SHA256
`1ee89e1669362dee13851129c0a8a791a87521eb4148e5efbf5d26596738e25b`.
The committed subset in
[lvgl_fonts.c](lvgl_fonts.c) uses 18,458 bitmap bytes and was generated with
Pillow 10.2.0 / FreeType 2.13.2. Its SIL OFL-1.1 notice remains in
`assets/licenses/LVGL_VIEW_FONT_LICENSE.txt`.

From this display directory, verify that the committed subset covers the UI
vocabulary:

```bash
python3 generate-lvgl-font.py --check
```

This coverage check reads only `lvgl_view.cc` and `lvgl_fonts.c` and uses the
Python standard library.
