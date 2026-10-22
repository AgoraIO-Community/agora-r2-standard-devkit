# R2 操作手册截图

本目录 PNG 是当前 R2 中文界面的 640x480 原生渲染结果。截图使用正式的 LVGL view、
字体、表情策略和固定 AVDK LVGL；仅设备、触摸输入和异步请求接收端使用主机测试桩。

| 文件 | 内容 |
| --- | --- |
| `ready.png` | READY，中性待机 |
| `conversation-listening.png` | 会话倾听 |
| `conversation-thinking.png` | 会话思考 |
| `conversation-speaking.png` | 会话播报 |
| `settings.png` | 设置入口 |
| `provision-confirmation.png` | 配网确认 |
| `provision-active.png` | 配网热点和网页地址 |
| `provision-failed.png` | 配网失败 |
| `pair-code.png` | 六位配对码示例 |
| `factory-confirmation.png` | 恢复出厂确认 |
| `restoring.png` | 恢复中 |
| `reset-failed.png` | 恢复失败 |
| `rebooting.png` | 重启中 |
| `wifi-portal.png` | 从生产配网代码提取的网页 |

热点名和配对码均为格式示例，实际值以设备为准。`wifi-portal.png` 不包含密码或真实网络数据。

## 重新生成截图

```bash
bash docs/images/r2-user-guide/render-ui.sh
```

需要已初始化 `bk_avdk_smp`、C/C++ 编译器、CMake、Ninja、Python 3 和 Pillow。
构建目录位于 `/tmp`，PNG 输出到本目录。

## 重新生成 PDF

手册源文件为 [r2-user-guide.html](../../r2-user-guide.html)，输出为
[R2_操作手册.pdf](../../R2_操作手册.pdf)：

```bash
python3 docs/render-r2-user-guide.py
```

需要 Chrome/Chromium 和中文字体。脚本会先更新 `wifi-portal.png`，再导出 A4 PDF；
不改写固件源码或设备配置。

使用已跟踪的全部 PNG，仅重新生成 PDF：

```bash
python3 docs/render-r2-user-guide.py --pdf-only
```
