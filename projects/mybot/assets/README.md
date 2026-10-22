# Embedded Voice Assets

**English | [简体中文](README.zh-CN.md)**

This directory contains the Chinese and English prompts compiled into the AP
firmware. The asset update workflow converts `locales/**/*.ogg` into
`components/mybot/mybot_sdk/platforms/bk7259/bk7259_assets.c`; no SD card or
separate asset partition is required.

## Format

All files are Opus-in-Ogg, converted from 16 kHz mono signed 16-bit PCM with
`projects/mybot/scripts/convert_pcm_to_ogg.sh`:

```text
libopus, 24 kbit/s VBR, application=voip
```

The device decoder expects `OpusHead` with mono or stereo channels and uses the
pre-skip and final granule position. Do not hand-encode replacement assets with
different parameters.

Required files in each `zh-CN/` or `en-US/` directory:

- `wificonfig.ogg`: played when APSTA provisioning starts.
- `success.ogg`: played after successful provisioning.
- `prompt.ogg` and `0.ogg` through `9.ogg`: pairing-code prompt and digits.

## Update Assets

1. Prepare same-name PCM files as 16 kHz mono s16le.
2. Run `projects/mybot/scripts/convert_pcm_to_ogg.sh` to produce the OGG files.
3. Regenerate the embedded C array:

```bash
python3 projects/mybot/scripts/generate_assets_c.py \
  projects/mybot/assets \
  components/mybot/mybot_sdk/platforms/bk7259/bk7259_assets.c
```

4. Rebuild the firmware with `python3 scripts/build_all.py`.

The language selected by `CONFIG_MYBOT_LANGUAGE_ZH_CN` or
`CONFIG_MYBOT_LANGUAGE_EN_US` selects both the prompt directory and service
endpoint. Do not provide a firmware whose audio language and service language
differ.

## Memory

The C arrays store compressed OGG bytes in AP Flash. Decoding allocates PCM in
PSRAM. Check `build_summary.txt` after changing assets; the current OTA partition
has very limited headroom.
