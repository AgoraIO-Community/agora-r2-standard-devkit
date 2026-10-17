# mybot 固件内嵌语音资源

本目录保存父仓库维护的 mybot 提示语音资源。构建准备时先复制产品工程和组件，随后
`projects/mybot/scripts/generate_assets_c.py` 会把
`locales/` 下的 OGG 文件转换为 C 源文件
`components/mybot/mybot_sdk/platforms/bk7259/bk7259_assets.c`；该源文件随 AP 固件一起编译，设备直接从
固件中的只读数组读取并解码语音，不需要在 SD 卡上保存资源文件。

生成器只收集 `locales/**/*.ogg`。`LICENSE.xiaozhi-esp32` 和本说明文件不会被
编译进固件。

## 音频格式

所有音频为 **Opus-in-Ogg**：由 16 kHz 单声道 s16le 源经
`scripts/convert_pcm_to_ogg.sh` 编码（libopus 24 kbps VBR、`-application voip`），
设备侧由 `bk7259_ogg_pcm.c` 解码为 16 kHz 单声道 PCM。解码器依赖 OpusHead
（mono、pre_skip 裁剪）与结尾 granule position，因此始终使用脚本转换，不要手工
指定其它编码参数。

`zh-CN` 与 `en-US` 均包含：

- `wificonfig.ogg`：APSTA 配网开始时播放一次（`bk7259_prompt.c`）。
- `success.ogg`：配网成功提示（同上）。
- `prompt.ogg` 与 `0.ogg`~`9.ogg`：播报配对码
  （`bk7259_announce.c` 的 `sound_file_name()`）。

音频内容源自 MIT 协议的 `xiaozhi-esp32` 项目对应 locale 资源；配对码文件来自
Linux mybot demo（文档标注为同一 `xiaozhi-esp32` 资源的衍生）。再分发必须保留
`LICENSE.xiaozhi-esp32`。

## 修改现有语音

1. 准备 16 kHz 单声道 s16le PCM 源文件，放到对应 locale 目录下同名
   （如 `prompt.pcm`）。
2. 运行 `scripts/convert_pcm_to_ogg.sh`，生成或覆盖同名 `.ogg`。
3. 重新生成 C 数组：

   ```bash
   python3 projects/mybot/scripts/generate_assets_c.py \
     projects/mybot/assets \
     components/mybot/mybot_sdk/platforms/bk7259/bk7259_assets.c
   ```

4. 在仓库根目录执行 `python3 scripts/build_all.py` 重新编译并烧录。构建会在生成副本中
   刷新数组；无需直接修改官方子模块或在源工程内编译。

## 新增语音

1. 按上面的步骤生成新的 `.ogg`，并放入对应的 `locales/<lang>/` 目录。
2. 重新运行 `generate_assets_c.py`。只有被设备代码引用的文件才会播放：
   - `wificonfig.ogg` / `success.ogg`：
     `components/mybot/mybot_sdk/platforms/bk7259/bk7259_prompt.c`。
   - `prompt.ogg` / `0.ogg`~`9.ogg`：
     `components/mybot/mybot_sdk/platforms/bk7259/bk7259_announce.c`
     的 `sound_file_name()`（对应 `mybot_announce_sound_t` 枚举）。
3. 在仓库根目录执行 `python3 scripts/build_all.py` 并烧录。

语言在 `MyBot BK7259 platform` 的 Kconfig choice 中选择。中文使用
`https://botstation.shengwang.cn/api`，英文使用
`https://botstation.agora.io/api`；服务器地址由该 choice 派生，不能与内嵌语音标签错配。
关闭 `CONFIG_MYBOT_HTTPS` 只将协议切换为 HTTP。

## 固件空间

数组保存的是压缩后的 OGG 字节，实际占用 AP 固件的 Flash 空间；解码后的 PCM
缓冲区仍在运行时分配到 PSRAM。新增或替换较多音频后，请检查 AP 固件的 Flash
使用率，确保仍有足够空间。
