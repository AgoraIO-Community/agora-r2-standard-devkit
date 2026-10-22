# 内嵌语音资源

**[English](README.md) | 简体中文**

本目录保存编译进 AP 固件的中英文提示音。资源更新流程将 `locales/**/*.ogg` 转换为
`components/mybot/mybot_sdk/platforms/bk7259/bk7259_assets.c`，不依赖 SD 卡或独立资源分区。

## 格式

所有文件均为 Opus-in-Ogg，由 16 kHz 单声道 s16le PCM 通过
`projects/mybot/scripts/convert_pcm_to_ogg.sh` 生成：

```text
libopus，24 kbit/s VBR，application=voip
```

设备解码器依赖 `OpusHead` 中的声道数、pre-skip 和结尾 granule position。
不要使用其他参数手工编码替换资源。

`zh-CN/` 和 `en-US/` 均需包含：

- `wificonfig.ogg`：APSTA 配网开始时播放。
- `success.ogg`：配网成功后播放。
- `prompt.ogg`、`0.ogg` 至 `9.ogg`：配对码提示和数字播报。

## 更新资源

1. 准备同名 PCM 文件，格式为 16 kHz 单声道 s16le。
2. 运行 `projects/mybot/scripts/convert_pcm_to_ogg.sh` 生成 OGG。
3. 重新生成内嵌 C 数组：

```bash
python3 projects/mybot/scripts/generate_assets_c.py \
  projects/mybot/assets \
  components/mybot/mybot_sdk/platforms/bk7259/bk7259_assets.c
```

4. 执行 `python3 scripts/build_all.py` 重新构建固件。

`CONFIG_MYBOT_LANGUAGE_ZH_CN` 或 `CONFIG_MYBOT_LANGUAGE_EN_US` 同时选择提示音目录和服务地址。
不要让固件的声音语言与服务语言不一致。

## 内存

C 数组在 AP Flash 中保存压缩后的 OGG，解码 PCM 分配到 PSRAM。替换资源后应检查
`build_summary.txt`；当前 OTA 分区余量很小。
