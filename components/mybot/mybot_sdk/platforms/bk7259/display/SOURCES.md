# MyBot LVGL pixel-glasses view

## Current visual reference

The face geometry and colors follow the user-supplied simulator in
`mcp-resource/threejs-project/`, relative to the parent `mybot-bk7259` repository.
The selected appearance is `process` (工艺灰) and the face style is `geek`
(像素眼镜). These reference inputs were read without modifying or copying the
simulator into the firmware source tree:

| Reference file | SHA256 |
| --- | --- |
| `src/expressions.js` | `1d51ca444e69a39da5b2feedc38d543aea362433184e89a76a825e757b684e31` |
| `src/reference-style.js` | `2b0cf05ffceb91f079d1cd542a982622e9c44c882ff84ac98a84b9a90bfa0646` |
| `src/main.js` | `675c47ed36249a89f3791c9b12ffe4300b6eec86598c88d3e62a6048e55eec5a` |

The supplied reference has no separate license declaration establishing an
open-source redistribution grant. This record identifies its provenance; it
does not assign an MIT or other third-party license to the simulator.

The LCD uses a fixed `#050708` black glass background and gray decorations,
avoiding visible dark-gradient banding in RGB565. Neutral idle uses the
process skin's `#f1f1ee` accent, glasses, two curved eyes, and a smile. Greeting
uses `#f1ca79`, with a curved left eye, round right eye, and a smile. The
1024x896 reference is scaled uniformly by 9/16 into a virtual 576x504 area at
`(32,-12)` on the R2 panel's logical 640x480 display (native GC9503CV 480x640).
Only empty canvas space is cropped above and below the display; visible face
geometry and text remain within its rounded-bezel safe area. The glasses span
about 345 pixels. Fixed LVGL widgets and sampled curves draw the face. Five
small immutable A8 masks render filled hearts, stars, ellipses, cheeks, and
tears without triangle-fan seams. These masks are generated locally from the
reference geometry; no full-face image or canvas buffer is required.

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

`READY` selects NEUTRAL and `IN_CONVERSATION` selects GREETING by default.
The independent `include/bk7259_expression.h` module provides the full 23-entry
simulator enum, name conversion, and copied local set/clear requests to the
existing display owner. Overrides are limited to READY/conversation face pages
and are cleared by workflow screen changes. Indicator-only updates retain the
selected face and do not restart its transition. Voiceprint and
listening/thinking/speaking indicators update the third terminal row rather
than showing an overlay. Chinese and English
provisioning, web-console pairing, error, and factory-reset guidance remain
available. The additional 21 expressions are not automatically cycled and have
no MCP/RTM command mapping or UART/motor integration in this change.

## Expression mappings

These mappings use the simulator's `drawGeekFace()`/`GLYPHS` path. In particular,
NEUTRAL uses HAPPY's double-arc geometry with the process skin's off-white
accent. It does not use the simulator's separate `character` face, pupil,
blink, or tremble behavior. Colors below are source RGB values converted to
the target RGB565 format.

| ID/name | Simulator label | Eyes; mouth | Accent |
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

Enabled expression effects follow the geek reference: EXCITED alternates eye
bounce, THINKING/BORED drift, PROCESSING moves its orbit eyes, SLEEPY animates
the yawn, and SLEEP dims and breathes. The reference's brightness multipliers
apply to SLEEPY, SAD, APOLOGY, and SLEEP; SURPRISED/ERROR retain their brief
entry flash. ERROR and SLEEP are display artwork only, with no change to SDK
error, connectivity, or hardware power state.

The reference's fake screen translation is retained: LISTENING shifts the
whole face and terminal actor down by 18 source pixels, and
SAD/APOLOGY/SLEEPY shift it by 28. CURIOUS/CONFUSED/PLEADING keep the display
upright rather than applying `main.js`'s 8-degree roll. That pose difference
avoids an additional transformed draw layer; the eye/mouth geometry, colors,
and expression effects still follow the geek reference. The simulator's
physical pan/tilt poses are not translated into hardware movement.

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

`pixel_expression_masks.h` contains five immutable, uncompressed A8 masks
generated by the target-owned MIT-licensed `generate-expression-masks.py`.
The generator independently expresses the reference's `heart()`/`star()`
contours and geek ellipse dimensions, then samples at four times the R2 9/16
scale and downsamples with Lanczos. The recorded generation tool is Pillow
10.2.0. The output retains its MIT notice and the reference geometry SHA256;
the reference simulator's separate provenance remains as stated above.

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

From this display directory, regenerate with the pinned reference file or
check the committed bytes:

```bash
python3 generate-expression-masks.py --reference /path/to/threejs-project/src/expressions.js
python3 generate-expression-masks.py --check
```

`--reference` rejects a different simulator SHA256. `--check` needs the recorded
Pillow version but not the simulator file; it reconstructs all five masks and
compares the generated header. Pillow and the local simulator are development
inputs, not runtime or firmware-build dependencies.

## Retained view and asset provenance

The preceding platform-independent status view and generated resources were
adapted from `junlon2006/mybot-esp32` commit
`19af3bdfaf1dd35461af48def8f9558e3503e755`:

- `components/mybot_platform/src/drivers/display/renderers/lvgl/lvgl_view.cc`
- `components/mybot_platform/src/drivers/display/renderers/lvgl/lvgl_assets.c`
- `components/mybot_platform/src/drivers/display/renderers/lvgl/lvgl_fonts.c`
- `components/mybot_platform/src/internal/display/lvgl_view.h`
- `components/mybot_platform/src/internal/display/lvgl_assets.h`

The view's MIT license, original layout reference, and copyright notice remain
in `assets/licenses/LVGL_VIEW_LICENSE.txt`. The historical asset API header
retains its Apache-2.0 SPDX identifier. The generated font and emoji pixel data
retain SIL OFL-1.1 notices in `assets/licenses/LVGL_VIEW_FONT_LICENSE.txt` and
`assets/ui/noto_emoji/LICENSE.txt`. Emoji source hashes, upstream revision,
format, and generation details remain in `assets/ui/noto_emoji/SOURCES.json`.
The old four 64x64 ARGB8888 emoji images total 65,536 pixel bytes, but
`lvgl_assets.c` is no longer linked into the current firmware.

## Fonts

Terminal decorations use renamed BK7259 Terminal subsets derived from the
external, read-only `DejaVuSansMono-Bold.ttf` generation input, pinned by SHA256
`3a3c502eeff669a231549e80df9f7c49de109bafe303170409e905d0b31a38fe`.
The original font file is not copied into the firmware tree. The 4 bpp subsets
in `pixel_fonts.c` replace the wider Unscii 16 rendering. Their actual LVGL
text advances closely match the simulator's scaled text proportions:

| Subset | Glyphs | Bitmap bytes | Use |
| --- | --- | --- | --- |
| BK7259 Terminal 15 | 95 ASCII | 4479 | Four terminal rows |
| BK7259 Terminal 16 | 3 (`</>`) | 155 | Signature |

The total bitmap data is 4634 read-only bytes. The 15-pixel terminal size is
the nearest integer to the reference's 26 pixels scaled by 9/16; the 16-pixel
signature similarly follows its 28-pixel reference. Actual LVGL character
advances determine label and cursor width. The local
`generate-pixel-font.py` uses Pillow 10.2.0 / FreeType 2.13.2 and the same
packing as `generate-lvgl-font.py`. Regenerate with
`python3 generate-pixel-font.py --font /path/to/DejaVuSansMono-Bold.ttf`.
Bitstream copyright/trademark notices, the DejaVu public-domain modification
notice, and the full Bitstream Vera license are retained in
`assets/licenses/TERMINAL_FONT_LICENSE.txt`. The generated subsets use the
distinct BK7259 Terminal name.

Chinese and English operational text uses the 20-pixel, 4 bpp MyBot UI Sans
subset of Source Han Sans SC Normal. The current font contains 185 glyphs,
including ASCII and fixed Chinese UI vocabulary. Arbitrary Chinese SSIDs or
chat text require a separately extended font.

The local `generate-lvgl-font.py` is adapted from the preceding reference's
MIT-licensed generator and reads the fixed vocabulary in `lvgl_view.cc`. It
uses the OFL-licensed `SourceHanSansSC-Normal.otf`, with required input SHA256
`1ee89e1669362dee13851129c0a8a791a87521eb4148e5efbf5d26596738e25b`.
The current bitmap uses 24,638 bytes and was generated with Pillow 10.2.0 /
FreeType 2.13.2. The source font is an external generation input; firmware
embeds only this subset. Run
`python3 generate-lvgl-font.py --font /path/to/SourceHanSansSC-Normal.otf` to
regenerate, or `python3 generate-lvgl-font.py --check` to verify UI glyph
coverage without the source font or Pillow.
