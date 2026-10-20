# R2 操作手册 UI 截图

本目录 PNG 是当前 R2 中文 UI 的原生渲染结果，分辨率均为 640 × 480。
截图使用正式 `lvgl_view.cc`、像素字体、中文字体、表情策略和官方 AVDK 的 LVGL，
没有手绘或修改屏幕内容。仅显示设备、触控输入和异步请求接收端使用主机测试桩。
动画截图是其中一个时刻，实机动态效果会随时间变化。

热点名称 `mybot-a1b2`、配对码 `001028` 是格式正确的说明示例；实际值以设备显示为准。

| 文件 | 实际内容 |
| --- | --- |
| `ready.png` | READY，中性待机，`state: ready` |
| `conversation-listening.png` | 倾听，`state: listening \| vp:pending` |
| `conversation-thinking.png` | 思考，`state: thinking \| vp:ok` |
| `conversation-speaking.png` | 说话，中性表情，`state: speaking \| vp:ok` |
| `settings.png` | 设置：配网、恢复出厂、返回 |
| `provision-confirmation.png` | 开始配网确认 |
| `provision-active.png` | 配网热点、网页地址、取消配网 |
| `provision-failed.png` | 配网失败、重新配网、设置 |
| `pair-code.png` | 固定六位配对码示例 `001028` |
| `factory-confirmation.png` | 恢复出厂确认及清除内容说明 |
| `restoring.png` | 正在恢复出厂设置 |
| `reset-failed.png` | 恢复出厂失败及重试按钮 |
| `rebooting.png` | 恢复完成，正在重启 |
| `wifi-portal.png` | 从生产代码提取的配网页；SSID 示例 `Home-WiFi`，不包含密码或真实网络数据 |

从仓库根目录重新生成：

```sh
bash docs/images/r2-user-guide/render-ui.sh
```

需要已初始化的 `bk_avdk_smp` 子模块、C/C++ 编译器、CMake、Ninja、Python 3 和 Pillow。
脚本将构建目录放在 `/tmp`，不会修改固件源码、测试或设备配置；PNG 输出到本目录。
可设置 `R2_MANUAL_UI_BUILD_DIR` 复用主机构建目录。

## 生成操作手册 PDF

手册为 [R2_操作手册.pdf](../../R2_操作手册.pdf)，排版源文件为
[r2-user-guide.html](../../r2-user-guide.html)。已按用户确认的新硬件行为说明“长按按钮开关机”，
不沿用旧板的长按忽略说明，也不指定尚未确认的长按时长。

```sh
python3 docs/render-r2-user-guide.py
```

需要本机 Chrome/Chromium 与中文字体（当前使用 Noto Sans CJK SC）。该脚本先从
`bk7259_wifi_portal.c` 提取实际网页并生成 `wifi-portal.png`，再将 HTML 导出为 A4 PDF。
数据全部为演示值，不提交网络连接请求；其余设备图像由上述原生 UI 脚本生成。
界面截图与正文分开维护，修改操作文案只需重新导出 PDF，无需编译固件。

本版以正式 R2 合并提交 `2a773a8` 的 UI 和配网流程为基础，结合用户确认的开关机硬件行为编写。
生成结果已检查7页分页、中文字体嵌入、13幅图片嵌入和可搜索文本；所有源码改动均限于 `docs`。
