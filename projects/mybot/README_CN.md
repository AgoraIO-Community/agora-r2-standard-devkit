# MyBot 工程（BK7259）

- [English](./README.md)
- [仓库概览](../../README.zh-CN.md)
- [验证状态](./UI_VALIDATION.zh-CN.md)

## 范围

本目录保存 MyBot SDK 在耘趣 V1.0 R2 开发板上的 BK7259 产品集成，负责板级初始化、
APSTA/STA 网络、本地外设、产品策略和 MyBot 生命周期边界。

设备服务注册、鉴权、配对、会话状态、RTC/RTM 和声纹处理由 MyBot SDK 负责，
RTC 传输由 Agora 提供。云端设备服务和 AI Agent 属于外部依赖。

构建命令、产物路径、配置文件和第三方许可证见
[仓库概览](../../README.zh-CN.md)与[依赖说明](../../docs/OFFICIAL_DEPENDENCIES.zh-CN.md)。

## AP 与 CP

| 核 | 职责 |
| --- | --- |
| CP（`bk7259`） | 初始化 BK 运行环境并启动 AP 系统。 |
| AP（`bk7259_ap`） | 运行产品任务、网络、MyBot 生命周期、音频、显示、触摸、按键、摄像头和 H.264 编码。 |

`bk7259` 工程目标同时编译并打包两个核。

## 启动与所有权

AP 入口为 [ap_main.c](./ap/ap_main.c)：

1. 初始化 BK 运行环境和媒体服务。
2. 创建 `mybot_app` 产品任务。
3. 从芯片 UID 派生设备 ID，构建运行时配置。
4. 初始化 AOSL，并准备产品自有的显示、Wi-Fi 管理器和按键。
5. 恢复待执行的恢复出厂操作、连接已保存 Wi-Fi，或进入 APSTA 配网。
6. STA 获得 IPv4 后才启动 MyBot。
7. 轮询 SDK 状态并处理会话、Wi-Fi 设置和恢复出厂请求。

Wi-Fi 由产品层拥有。SDK 适配层只观察连接状态并转发串行化事件。进入配网前必须先停止
SDK；SDK 服务运行时不能启动配网门户。

网络或 SDK 启动失败时，产品保留已准备好的显示和操作入口，允许重试配网或恢复出厂。
设备身份或早期硬件准备失败时，产品任务可以退出，因为 MyBot 所需平台不可用。

## 按键与网络设置

| KEY1/GPIO47 操作 | 行为 |
| --- | --- |
| 短按并松开 | `READY` 开启对话；`IN_CONVERSATION` 结束对话 |
| 其他 SDK 状态 | 忽略短按 |
| 长按 | 当前 R2 硬件处理开关机 |
| 平台准备时已按住 | 先松开才能产生新手势 |

按键低电平有效，每 20 ms 扫描，并连续两次采样消抖。`KEY_SHORT_PRESS_LIMIT_MS=5000`
只过滤长按松手后的会话事件，不定义硬件开关机时间。CEN 按键仍仅执行硬件复位。

点击界面中的 **Settings** 可进入 Wi-Fi 设置或恢复出厂。

- 配网要求 SDK 为 `STOPPED` 且 `mybot_is_running() == false`。
- 设备启动开放热点 `mybot-xxxx`，在 `http://192.168.4.1/` 提供设置页面。
- 候选网络获得 IPv4 后才保存凭据。
- 保存开始前可取消；取消后只尝试重连一次原网络。
- 配网成功后重启 MyBot，无需重启设备，也不清除设备身份、绑定或音量。

恢复出厂由产品任务执行：

1. 持久化 `mybot.factory_reset.v1`。
2. 停止 MyBot 并确认进入 `STOPPED`。
3. 停止配网门户、SoftAP、扫描和 STA。
4. 删除 `mybot.device_auth`、`mybot.wifi.v1` 和 `mybot.volume.v1`。
5. 校验删除结果，最后删除恢复标记并重启。

恢复出厂不会擦除整个 EasyFlash 环境，不改变 UID 派生的设备 ID，也不向服务器发送解绑请求。
清理失败时保留恢复标记并等待用户显式重试。板级配置见
[usr_gpio_cfg.h](./ap/config/bk7259_ap/usr_gpio_cfg.h)。

## 音频

MyBot 使用 16 kHz、单声道、有符号 16 位 PCM。R2 采集路径使用三路差分模拟 ADC，
并追加数字播放信号作为 AEC 参考：

```text
ADC0 + ADC1 + ADC2 + 追加播放参考
  -> AEC v3
  -> 16 kHz 单声道 S16 PCM
  -> MyBot RTC 上行
```

采集块为 640 字节（20 ms），两块缓冲；VAD 关闭，启用传统降噪和 EC-only 输出。
配套 AVDK 补丁提供三路输入处理和 AEC 库集成。

播放使用板载 DAC 流，GPIO50 高电平使能扬声器功放。SDK 音量 0-100 映射到扬声器数字增益，
并保存到 `mybot.volume.v1`。当前板卡没有音量按键。

当前三路输入配置仍需在目标板上验证采集质量、AEC 效果和反复启停。

## 显示

产品层从平台准备到关闭期间独占 GC9503CV MIPI 屏，因此 MyBot 停止时配网和恢复页面仍然可见。
原生 480x640 面板以 640x480 逻辑 RGB565 画面渲染。

板级电源 owner 控制：

- GPIO20：LCD 3.3 V
- GPIO29：LCD/摄像头共用 2.8 V
- GPIO27：摄像头 1.2 V
- GPIO40：低有效背光
- GPIO42：屏幕复位

结束会话时保留这些电源轨，并通过 GPIO30 复位摄像头。

LVGL 9.5.0 界面采用工艺灰和像素眼镜风格，包含配网、配对、就绪、会话阶段、错误页和
恢复出厂页面，并支持中英文提示。会话中根据 SDK indicator 显示聆听、思考或播报状态及
声纹结果。全部 23 个本地表情可用，但不会驱动可选云台。

显示 owner 在单一 UI 任务中应用复制后的状态快照，并通过 DSI、面板和 DPU 路径提交完整
RGB565 帧。SDK LCD init/destroy 只负责挂接和脱离，不会在 SDK 停止时销毁产品显示。

资源来源见
[display/SOURCES.md](../../components/mybot/mybot_sdk/platforms/bk7259/display/SOURCES.md)。
触摸和显示测试见 [tests/touch](./tests/touch/README.md) 与
[tests/touch_ui](./tests/touch_ui/README.md)。

## 视频

默认 AP 配置启用视频，链路为：

```text
GC2053/CV2005 MIPI CSI，1280x720，5 fps
  -> ISP MP 缩放为 640x480 NV12
  -> 硬件 FLEXA H.264 编码
  -> 完整 Annex-B access unit
  -> MyBot 视频处理器
  -> Agora RTC 上行
```

码率范围为 256000-512000 bit/s，初始目标 384000 bit/s。RTSA 带宽变化会调整编码器，
关键帧请求会强制生成 IDR。视频仅在 RTC `CONNECTED` 后启动，并在离开 RTC 前停止。
设备不订阅或显示远端视频。

配置和实现见 [defconfig](./ap/config/bk7259_ap/defconfig)与
[bk7259_video.c](../../components/mybot/mybot_sdk/platforms/bk7259/bk7259_video.c)。

## 可选功能说明

- [DeviceCommand 行为](../../docs/mybot-sdk-device-command-plan.zh-CN.md)
- [EB521 云台与 UART](../../docs/eb521-gimbal-plan.zh-CN.md)
- [验证状态](./UI_VALIDATION.zh-CN.md)
- [界面截图](../../docs/images/r2-user-guide/README.md)

默认 AP 配置关闭 EB521 云台；主机测试仍覆盖保留的实现。主机测试和编译成功不代表音频、
视频、触摸、掉电或长时间运行已在目标板上验证通过。
