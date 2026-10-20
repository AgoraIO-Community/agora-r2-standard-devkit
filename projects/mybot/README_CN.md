# MyBot 工程（BK7259）

- [English](./README.md)

## 1. 工程简介

`mybot` 是 MyBot SDK 的 BK7259 AP/CP 双核固件集成工程，在同一次会话中提供设备服务控制、
RTC 双向音频、本地配网与界面，以及可选的单向编码视频上行。视频默认开启。
当前 `release/v4.0.1-mybot-r2` 分支适配耘趣 V1.0 / Robot R2 新板，包含 GPIO47 单键
短按控制会话、硬件长按开关机、触屏配网和确认恢复出厂、GC9503CV 屏；当前音频保留配套参考的三路模拟输入与追加数字AEC参考，
正常界面采用模拟器的工艺灰皮肤与像素眼镜。产品源码维护在父仓库，官方 BK 依赖的必要改动
在隔离构建副本中以受版本锁约束的补丁应用。

用户操作见 [R2 操作手册（含 UI 截图，PDF）](../../docs/R2_操作手册.pdf)。

内嵌 SDK 同步上游 `main` 最新已提交快照 `58f0ef9c14be9f159b1dc167bf0f035983b25117`
（标签 `v1.3.0`，版本 1.3.0），保留 HTTPS 请求与响应 body 日志补丁。
本次同步仅更新发布版本及源码注释，平台接口、RTC 和媒体行为与上一快照一致。
通过 RTM 进行 MCP 控制时，
仅接受单层 `message.user` 信封，`data_type` 为 `message`，字符串 `content` 内包含 DeviceCommand v1。
支持 `audio.set_volume`（`params.level` 为 0–100 的整数）和 `audio.adjust_volume`
（`params.delta` 为 -100–100 的整数）。正数增加音量，负数减少音量，0 保持不变，结果钳制到
0–100。裸命令、`audio.set_mute` 及情绪命令不再通过 RTM 执行。发送者、频道、会话及会话代次
校验保持生效，`issued_at`/`expires_at` 可省略或
提供但均忽略，不检查时钟或有效期。本会话最近 16 个不同的 `command_id` 按 FIFO 去重：
重复不刷新位置或重复增减音量，新 ID 淘汰最老记录，已被淘汰的 ID 可能再次执行。
MCP 音量设置/增减复用已有音量适配，命令通路尚需真机验证。本地表情及已有声纹/阶段
indicator 保持支持，当前快照关闭云端 MCP 情绪入口。本地 LCD 表情不触发 EB521 UART/云台动作。

当前音频对比固件在AP defconfig设置 `# CONFIG_MYBOT_GIMBAL is not set`，不启用舵机模块、
对应UART2映射及CLI注册。以下命令仅在设为 `CONFIG_MYBOT_GIMBAL=y` 并重新编译后可用。

在日志控制台输入 `ap_cmd gimbal_cmd`，每次只发送一条UART2命令。
受理后按水平左转30°、俯仰向下10°、俯仰向上20°、水平相对右转10°依次循环，
正式日志打印完整TX/RX与CRC校验后的应答。忙时拒绝调用且不推进列表；
超时后可再次输入命令发送下一条，不自动重传。
首次手动调用后，自动回中暂停至平台重启；本地表情和阶段仍独立更新LCD。

输入 `ap_cmd gimbal_scene happy`、`sad`、`angry` 或 `surprised`，每次执行对应的一个
完整表情轨迹并在成功后回中。不带名称的 `ap_cmd gimbal_scene` 按上述顺序循环，
只有受理成功才推进。每个动作开始前读取云台的真实当前位置，并检查归零完成、两轴停止；
状态反馈缺失或无效时不开始运动。表情动作与单帧命令共用一个UART工作线程，
已有操作或对话进行中会返回忙。正式日志报告开始、完成、取消或失败；没有定时发送。

绑定状态轮询和停止通知移到独立的 `service_mpq`，慢后台请求不会延迟本地挂断或下一次开聊。
开始会话、配对码和 token 续期请求仍同步执行；SDK 关闭时排空后台线程。
HTTPS 适配支持独立连接并按连接查询 socket 错误状态。

SDK 日志不再使用 RTM 的 512 字节预览或 RTC 错误的 64 字节固定存储；RTC 错误字符串在借用
有效期间复制，完整内容传给日志层。目标 AOSL HAL 的 8 KiB 整行缓冲仍可能截断更长日志，
常规 RTM 日志仍为 DEBUG，默认 NOTICE 等级不显示。

## 2. AP 与 CP 职责

| 核 | 职责 |
| --- | --- |
| AP（`bk7259_ap`） | 负责产品控制循环、APSTA 配网、MyBot SDK 与 AOSL 生命周期、恢复出厂、HTTP(S)/RTC/RTM、音频、LCD、按键、摄像头采集、H.264 编码和视频上行。 |
| CP（`bk7259`） | 初始化 BK 运行环境并启动 AP 系统。 |

`bk7259` 工程目标会同时编译并打包两个核。

## 3. 编译

使用父仓库固定的官方 BK 子模块，在 `mybot-bk7259` 仓库根目录执行统一构建入口：

```bash
python3 scripts/check_sources.py
python3 scripts/build_all.py
```

默认先 clean 再编译中英文 AP/CP；可用 `--language zh-CN` 或 `--language en-US` 选择一种语言，
仅增量编译时使用 `--no-clean`。音频打包时长仅支持 60 ms。需要直接调用厂商 `make` 时先准备工程：

```bash
python3 scripts/prepare_vendor.py --language zh-CN
```

然后执行工具打印的完整命令，其中显式 `SDK_DIR` 指向补丁后的生成 AVDK，不是干净的官方目录。
不要直接编辑生成厂商副本；固定版本与补丁来源见 [官方依赖说明](../../docs/OFFICIAL_DEPENDENCIES.md)。

设备服务 REST 请求默认使用 HTTPS（`CONFIG_MYBOT_HTTPS=y`）。在
`projects/mybot/ap/config/bk7259_ap/defconfig` 中改为
`# CONFIG_MYBOT_HTTPS is not set`，重新运行统一构建入口即可切换为 HTTP；
恢复 `CONFIG_MYBOT_HTTPS=y` 后重新运行即可切回 HTTPS。
两种协议保持相同的语言域名与 `/api` 路径：中文为 `botstation.shengwang.cn`，
英文为 `botstation.agora.io`。该选项映射为 SDK 的 `MYBOT_ENABLE_HTTPS` 和取反的
`MYBOT_ALLOW_INSECURE_HTTP` 编译定义，不改变 RTC/RTM 与本地 HTTP 配网门户。
原有 ISRG Root X1 + DigiCert Global Root G2 证书集保持；不覆盖需要 GoDaddy G2 的证书链。

主要产物为：

```text
<生成工程>/build/bk7259/mybot/package/all-app.bin
<生成工程>/build/bk7259/mybot/package/app_pack.rbl
```

默认生成工程位于 `build/build-bk7259-zh-CN-video/solution/projects/mybot/` 及对应英文目录。
合并固件发布到 `releases/bk7259-<language>-video.bin`；HTTP 版本增加 `-http` 后缀，关闭视频时
省略 `-video`。以生成工程目录为基准，各核 ELF 分别位于 `build/bk7259/mybot/bk7259_ap/` 和
`build/bk7259/mybot/bk7259/`。

## 4. 上电初始化

CP 初始化 BK 系统并启动 AP。AP 的 `main()` 执行 `bk_init()`、打印版本、初始化媒体
服务，再创建产品控制任务；媒体服务先准备好显示后端需要的 OSI wrapper。

产品任务依次执行：

1. 根据芯片 UID 生成 `BK7259-<24位大写十六进制>` 设备 ID，填充语言、服务地址和版本配置。
2. 初始化 AOSL，准备 LCD/触摸屏、Wi-Fi 管理器和 GPIO47 扫描定时器。
3. 检查 `mybot.factory_reset.v1`；有效标记先继续恢复出厂，可检测的异常标记等待触屏重试。
4. 无恢复任务时注册平台描述符，尝试已保存的 Wi-Fi，或进入 APSTA 配网门户。
5. STA 获得 IPv4 并关闭门户、SoftAP 后启动 MyBot SDK，按服务器状态完成注册和绑定检查。
6. 在产品循环中处理触屏配网、恢复出厂、会话按键请求和 SDK 状态日志；启用视频时，摄像头等 RTC `CONNECTED` 后才启动。

网络设置或 SDK 启动失败后的等待状态保留产品显示和触摸输入，允许再次配网或触屏恢复出厂；媒体、
平台准备等更早的初始化错误会退出产品任务；描述符注册失败保留设置入口，下一次配网成功后重新尝试注册。

## 5. 按键、配网与恢复出厂

对话键 `KEY1` 使用 GPIO47，内部上拉、低电平有效。每 20 ms 扫描，两次一致采样消抖：

| 手势 | 行为 |
| --- | --- |
| 短按并松开 | `READY` 开启对话，`IN_CONVERSATION` 关闭对话，其余状态忽略短按 |
| 长按 | 当前硬件处理开机 / 关机；恢复出厂使用触屏设置 |
| 按键准备时已按住 | 先松开再按，避免跨重启重复触发 |

固件 `KEY_SHORT_PRESS_LIMIT_MS=5000` 只负责过滤持续按住及松手后的会话事件；
长按开关机由硬件处理，开关机时长以硬件为准。

CEN 保留硬件复位功能，双击、音量和其它按键未注册。右上角“设置”→“恢复出厂”打开
二次确认；取消只关闭弹层，确认才投递恢复请求，清理完成时间取决于 SDK 和驱动退出。

右上角“设置”→“配网”→“开始配网”可单独更换 Wi-Fi。确认前不影响当前会话；
确认后由产品任务停止 SDK，确认 `STOPPED` 且 `mybot_is_running()==false` 后启动 APSTA。
屏幕显示 `mybot-xxxx` 热点名和 `http://192.168.4.1`；手机连接热点并在网页提交新网络。
候选网络取得 IPv4 后才保存凭据，关闭门户与 SoftAP 后重新启动 SDK，无需重启设备。
设备鉴权、设备 ID、服务器绑定和音量均保留。

“取消配网”在开始保存新 Wi-Fi 凭据前有效，包括点击网页 Connect 后仍在尝试连接的阶段：
退出门户、扫描和候选 STA 后，仅尝试一次原保存网络，
成功后恢复 SDK；没有旧网络或连接失败时保留“重新配网”和“设置”入口，不自动再开热点。
保存开始后不再接受取消。恢复出厂优先于配网和取消，网络清理失败时保留资源所有权等待下一次操作。

正常挂断先停止本地视频与 RTC、清理会话 PCM，恢复 READY 后再发送一次 HTTP(S) 停止通知。
通知失败不重启媒体、不重试。停止通知和绑定轮询由独立的 `service_mpq` 执行，
慢后台请求不会阻塞下一次开聊。

`ap_main.c` 是恢复操作的所有者：显示恢复状态，保存并读回意图，停止 SDK 并确认
`STOPPED`，停止门户/SoftAP/扫描/STA，再依次删除并检查以下记录，最后删除恢复标记并重启：

| 记录 | 内容 |
| --- | --- |
| `mybot.device_auth` | 本地设备鉴权 |
| `mybot.wifi.v1` | Wi-Fi 凭据 |
| `mybot.volume.v1` | 持久化音量 |

失败时显示“重试”按钮，业务保持停止，不自动重启；新一次点击才重试。
未完成的清理保留意图，最终标记删除报错时尝试重建标记。有效意图保存后遇到掉电，
下次启动在联网前继续清理；部分删除不会回滚。触摸确认请求仅在
产品任务处理时才持久化。该功能不擦除整个 ENV、固件或射频校准数据，不改变 UID/设备 ID，
不解绑服务器。重启后重新配网、注册，并按服务端保留的绑定状态继续工作。

模块接口使用 `bk7259_*` 命名，独立于平台抽象：

- `components/mybot/mybot_sdk/platforms/bk7259/include/bk7259_key.h`：领取会话请求、投递开始/结束事件。
- `components/mybot/mybot_sdk/platforms/bk7259/include/bk7259_provision.h`：独立配网/取消请求和配网显示阶段。
- `components/mybot/mybot_sdk/platforms/bk7259/include/bk7259_factory_reset.h`：恢复请求、意图持久化、网络退出、配置清理和显示阶段。
- `mybot_bk7259_platform.h`：保持平台准备、注册、网络和关闭接口。

## 6. R2 音频与显示

当前音频以此前与 `mcp-resource/new` 配套参考对照的配置为基础调优 MIC 增益；本地参考目录不是构建依赖。
启用三路差分模拟输入
（`ch_bitmap=0x7`、`chl_num=3`），`adc_cfg.aec_en=3`追加数字播放参考，形成四条lane：

| lane | 算法输入 | 模拟增益 | 数字增益 |
| --- | --- | --- | --- |
| 0 / ADC0 | MIC输入1 | 16 dB | 4 dB |
| 1 / ADC1 | MIC输入2 | 16 dB | 4 dB |
| 2 / ADC2 | MIC输入3 | 16 dB | 4 dB |
| 3 / 追加 | 数字播放参考 | 不适用 | 不适用 |

DMIC关闭，`hw_ref_ch=NONE`（255），没有通道内ADC参考。采集块为640字节/20 ms、两块缓冲。
采集链为 `onboard_mic_stream_v2 -> aec_v3_algorithm_v2 -> raw_stream`，输出16 kHz单声道S16 PCM。
AEC使用硬件APPEND模式和默认AUTO参考，`adc_ch_num=3`、`dual_ch=2`、传统降噪；
VAD关闭（`vad_enable=0`），`ec_only_output=1`、`multi_output_use_ec_out=1`。

配套AVDK提供三麦解交织、trimic标记与SSL3支持，实际处理仍调用 `aec_proc`；Solution中
“临时禁用AEC”的注释已过时。wrapper、配置/算法头文件、AP算法库及配套库校验清单
由 `patches/bk_avdk_smp/0003-r2-trimic-aec.patch` 维护；库保持原 R2 的 `20261014` 版本。
不增加ASR输出端口或UI方向回调。

GPIO12按参考配置为输出下拉，用于ADC内部比较器放电；GPIO34/35保留附件中的
`SECOND_FUNC_DISABLE + GPIO_OUTPUT`声明，开机仍为HIGH-Z。GPIO2/3 UART2映射仅随
`CONFIG_MYBOT_GIMBAL=y`启用，当前对比固件关闭舵机模块、映射及CLI；GPIO47上拉会话键保持。
不增加运行期采集电平日志。当前三路采集与AEC实际效果仍待烧录验证。
GPIO50 高电平使能扬声器功放。音量通过 SDK `audio_volume` ops 调整数字增益，变更时
同步持久化；写入失败在后续设置或模块销毁时重试。当前没有音量按键，恢复出厂后使用
扬声器默认增益。

GC9503CV MIPI 屏物理分辨率为 480x640，逻辑画面为 640x480 RGB565；GPIO20 控制 3.3V，
GPIO29 控制 LCD/摄像头共用 2.8V 电源，GPIO42 为复位，GPIO40 背光低有效。
GPIO27 在平台准备阶段输出高电平，使能摄像头外部 1.2V 电源。显示的板级电源 owner
持有 2.8V 和 1.2V 电源直到完整平台关闭；关闭会话或视频只通过 GPIO30 复位摄像头，
保留板级电源，保持屏幕点亮。
恢复中、失败和重启中提示支持中文/英文，由现有显示任务渲染，SDK 停止/解挂不会覆盖提示。

LVGL 9.5.0 界面采用用户提供模拟器的 `process`（工艺灰）皮肤与 `geek`（像素眼镜）画法，
使用 `#050708` 固定黑色玻璃底与灰色装饰，避免 RGB565 深色渐变色带。
“中性待机”使用 `#f1f1ee` 米白镜框、双弧眼和微笑；
“打招呼”使用 `#f1ca79` 暖金镜框、左弧眼、右圆点眼和微笑。

| 阶段 | 画面 |
| --- | --- |
| 启动、配网、鉴权或配对 | 中英文必要引导；配对码在网页控制台输入 |
| SDK 实际进入 `READY` | 默认中性待机 |
| 进入 `IN_CONVERSATION` | 进入过渡后保持 SURPRISED 300 ms，再显示最新服务端阶段表情 |
| 会话中 LISTENING | 认真聆听 |
| 会话中 THINKING | 专注思考；连续超过 3 秒转 PROCESSING |
| 会话中 SPEAKING 或无有效阶段 | NEUTRAL |
| 挂断返回 `READY` | 中性待机 |
| 网络或启动失败 | 必要错误提示 |
| 恢复出厂 | 恢复中、失败或重启中提示，优先于 SDK 页面更新 |

配网成功不会改变 SDK 就绪状态或跳过配对。正常待机、会话画面保留四行终端文字，
READY 状态下为：

```text
> OIM / R2
> mood: neutral
> state: ready
> agora / realtime
```

`mood` 使用当前表情的小写名称，与 SDK 状态独立。第三行显示实际状态；会话中例如
`> state: listening | vp:pending`，阶段随 SDK indicator 更新为
`listening`、`thinking` 或 `speaking`；多个阶段同时有效时按
`speaking` > `thinking` > `listening` 显示，没有有效阶段时显示 `in_conversation`。
`vp:ok` 表示本次会话已收到声纹成功通知，`vp:pending` 表示尚未确认。
仅真实状态和页面均为 `IN_CONVERSATION` 时显示阶段与声纹。声纹更新只改文字，
阶段更新也决定后台自动表情；同阶段重复更新不重置动画或三秒思考计时。
显式表情覆盖保留，不弹提示；SLEEP 图案不会将真实状态改为 `standby`。
`</>` 签名和最后一行光标保留，UI 不控制云台动作。

应用通过现有 100 ms 状态观测循环，经原生 `bk7259_lcd.h` 接口发布复制后的 SDK 状态快照；
阶段与声纹 indicator 由现有 SDK LCD 回调提供，不修改 SDK、不新增 UI 任务。
SDK 为 `STOPPED` 时配网引导继续可见；同步停止期间沿用 STOPPING 引导页，
停止返回后立即发布观测到的 stopped 状态。恢复出厂引导保持最高优先级，第三行固定为
`> state: factory_reset` 并移除会话阶段/VP 字段；这是产品流程文字，不是新增 SDK 状态。

全部 23 个表情均已实现，枚举顺序与模拟器一致：

```text
NEUTRAL, GREETING, HAPPY, EXCITED, CURIOUS, CONFUSED, THINKING, LISTENING,
SURPRISED, SHY, PROUD, LOVE, SLEEPY, BORED, SAD, PLEADING, APOLOGY, WORRIED,
ANNOYED, REFUSE, PROCESSING, ERROR, SLEEP
```

独立的 `include/bk7259_expression.h` 模块提供本地显示请求：

```c
#include "bk7259_expression.h"

int show_happy(void) {
    return bk7259_lcd_set_expression(BK7259_EXPRESSION_HAPPY);
}
```

`bk7259_expression_from_name()` 将完整模拟器名称转换为枚举，忽略 ASCII 字母大小写；
`bk7259_expression_name()` 返回稳定的大写名称。
`bk7259_lcd_clear_expression()` 恢复当前流程与服务端阶段对应的最新自动表情。
设置和清除返回 `0` 表示显示所有者接受了已复制的请求，不表示已经刷屏完成；
无效表情或当前无法接受表情页面请求时返回 `-1`。接口使用现有 LCD snapshot 锁和
mailbox，不在调用线程执行 LVGL。只有准备完成且运行中的 `READY`/`IN_CONVERSATION`
页面接受请求，配网、恢复出厂与关闭过程拒绝请求。流程页面变化会清除覆盖；
本地覆盖在阶段更新时保持，直到显式清除或流程切换。重复请求同一本地表情不会重播过渡。

当前 SDK 快照关闭云端 MCP 情绪入口。开启动画时，会话开场的 300 ms 保持时间不包含
进入时的 250 ms 过渡；关闭动画时只有保持时间。计时从 UI owner 显示请求开始，普通 render
不重置计时；到期恢复最新自动表情，后台 THINKING 已持续三秒时直接恢复 PROCESSING。
离开会话会取消前景与计时，必要操作引导和恢复出厂始终优先。

所有表情仅渲染 LCD，不触发云台动作；手动 UART CLI 独立保留，仅在
`CONFIG_MYBOT_GIMBAL=y`时可用，当前对比固件关闭。不做自动演示轮播。
选择 ERROR 或 SLEEP 图案不会让 SDK 进入错误状态，也不会使硬件休眠。

模拟器 1024x896 画布按 9/16 等比缩放为 576x504 虚拟区域，位于 640x480 逻辑屏的 `(32,-12)`。
上下只裁去空白，文字与表情仍位于圆角安全区内，眼镜总宽约 345 像素。
终端保留参考的 37 像素源坐标行距，`</>` 左对齐。
固定 LVGL 控件与采样曲线绘制表情；心形、星形、实心椭圆、腮红和泪滴使用五块小型
只读 A8 抗锯齿蒙版，合计 13631 字节 Flash，消除三角扇接缝。蒙版由参考几何在本地生成，不使用完整表情图片，
不增加 canvas 缓冲。`CONFIG_MYBOT_LVGL_UI_ANIMATIONS`
控制短过渡、表情动态效果和 500 ms 光标闪烁，浅色主题选项已移除。复用现有显示任务、20480 字节 RGB565
绘制条带、两块各 614400 字节扫描缓冲和 8192 字节任务栈，配置刷新间隔为 50 ms。
LVGL 内部分配与栈峰值仍需在同时音视频运行时实测。

终端装饰使用由 DejaVu Sans Mono Bold 生成的只读 BK7259 Terminal 子集：15 像素 ASCII
和 16 像素 `</>`，合计 4634 字节位图，替代 Unscii 16，按 LVGL 实际字宽贴近参考缩放文字比例，
光标按实际字宽定位。
中英文操作引导使用 MyBot UI Sans 20 字体子集。
旧 emoji 源码与许可证保留用于来源追溯，但不再链接。资源来源、蒙版生成与复现、模拟器 SHA256 见
[display/SOURCES.md](../../components/mybot/mybot_sdk/platforms/bk7259/display/SOURCES.md)。
显示构建与验证记录见 [UI_VALIDATION.md](./UI_VALIDATION.md)。

## 7. 多模态视频上行

AP 默认配置开启视频（`CONFIG_MYBOT_VIDEO=y`）。在
`projects/mybot/ap/config/bk7259_ap/defconfig` 中改为
`# CONFIG_MYBOT_VIDEO is not set` 并 clean 后重新编译即可关闭。启用时 BK7259 视频链路为：

```text
MIPI CSI GC2053/CV2005 传感器（1280x720，5 fps）
  -> ISP MP（640x480 NV12）
  -> 硬件 FLEXA H.264 编码器
  -> 编码帧池
  -> MyBot 视频处理器
  -> Agora RTSA 主视频流
```

启用视频后的参数与行为：

| 项目 | 当前值 |
| --- | --- |
| 编码输出 | H.264 Annex-B access unit，640x480 |
| 传感器输入 | 1280x720，5 fps |
| 码率范围 | 256000-512000 bit/s，初始中点为 384000 bit/s |
| GOP | 30 帧；5 fps 下约 6 秒，RTSA 关键帧请求可强制生成 IDR |
| 生命周期 | RTC 到达 `CONNECTED` 后才启动采集和编码；离开 RTC 前先停止 |
| 方向 | 仅设备视频上行，远端视频订阅已关闭 |
| RTSA 时序 | `frame_rate=0`，由 RTSA 使用真实帧时间戳，不按固定帧率合成时间戳 |

RTSA 的带宽估算结果会被限制在配置的码率范围内，再应用到硬件编码器。音频保持双向，
视频仅上行。

视频配置位于：

- `components/mybot/mybot_sdk/Kconfig`
- `projects/mybot/ap/config/bk7259_ap/defconfig`

修改视频配置后应执行完整重新编译，避免生成配置保留旧值。

## 8. 真机验收

R2 LCD、此前双模拟麦/ADC2参考的上行与AEC、结束会话后的显示以及长按恢复出厂已有用户真机通过反馈。
上述按键恢复反馈属于旧入口；触屏恢复的GPIO43修正版和设置内独立配网均已收到用户实机测试通过反馈。
当前硬件长按按钮开关机的行为已由用户确认。
长时间音视频并发、断电续做和故障注入仍需专项实机验证。
当前三路输入音频配置仍待烧录验证。
工艺灰像素眼镜基础渲染已有用户真机通过反馈；四行文字与完整 23 表情尚待真机验证。
中英文固件编译、手势、恢复流程、持久化故障注入和配网取消已有主机验证；以下列表用于
后续回归，不能据此认定断电、驱动或 Flash 故障已全部做过真机验证。
视频相关检查需要先编译启用 `CONFIG_MYBOT_VIDEO=y` 的固件。

- [ ] 冷启动组合镜像 `all-app.bin`，确认 AP 和 CP 均正常启动。
- [ ] 确认摄像头探测前打印 `camera 1.2V power enabled: gpio=27 level=1`，并在新摄像头硬件上测量 GPIO27 电平与外部 1.2V 电源。
- [ ] 短按松手开启/关闭会话；关闭后屏幕仍亮，双向音频和 AEC 正常。
- [ ] 确认实际 `READY` 时为中性待机，进入会话先显示 SURPRISED 300 ms 并另计进入过渡，再恢复最新阶段表情；挂断后回中性待机。
- [ ] 验证 LISTENING、THINKING 连续三秒转 PROCESSING、SPEAKING 或无阶段时 NEUTRAL；同阶段重复更新不重置思考计时。
- [ ] 确认四行终端文字、小写表情名称、真实 SDK 状态及本次会话的阶段/声纹文字，对照模拟器验证 23 个表情及动态效果。
- [ ] 验证持久本地设置/清除、非法名称和枚举、阶段更新、流程切换及恢复优先级；清除后应恢复最新自动表情。
- [ ] 通过 `message.user` 信封验证 MCP `audio.set_volume` 的 0、50、100 音量；裸命令、静音及情绪命令应无效果。
- [ ] 验证封装后的 `audio.adjust_volume` 正数、负数及 0，包含 -100、100 边界；结果应钳制到 0–100，重复 command ID 不应再次增减音量。
- [ ] 确认阶段/声纹文字仅在真实会话显示，单独声纹更新保留表情；挂断后移除会话字段并取消遗留表情计时。
- [ ] 验证中英文引导、网页配对文案、圆角安全区域及恢复出厂显示优先级。
- [ ] 验证硬件长按开机/关机，短按松手仅在对应状态开启/结束会话；恢复出厂仍从触屏设置确认。
- [ ] 持续按住跨过重启不产生事件，先松开后才能产生新手势。
- [ ] 验证触摸四角坐标与设置入口；拖出按钮、多指、丢帧不能触发确认。
- [ ] 打开设置或取消确认不停止当前会话，一次触摸不能穿透设置与确认两个页面。
- [ ] 待机和会话中进入配网，确认 SDK 完全停止后才开放热点；成功更换网络后无需重启，鉴权、绑定和音量保留。
- [ ] 开始保存新 Wi-Fi 前取消配网，仅重连一次原网络；错误密码、无旧网络及重连失败时保留重新配网/恢复出厂入口。
- [ ] 配网保存开始后取消无效；配网、取消、保存及失败页面均可请求恢复出厂，恢复流程始终优先。
- [ ] 分别在配网、离线和会话中触屏确认，检查恢复提示、自动重启和重新配网。
- [ ] 清理后设备 ID 和服务端绑定保持不变，音量使用默认值，非产品 ENV 记录仍存在。
- [ ] 在有效意图保存后中断供电，检查下次启动续做；注入停止/删除失败，检查失败提示和新一次触屏重试。
- [ ] 确认板上 GC2053 或 CV2005 摄像头识别成功，视频源报告 1280x720、5 fps，并输出 640x480 H.264。
- [ ] 开启会话，确认双向音频正常，远端能收到 H.264 视频。
- [ ] 确认摄像头采集和编码只在 RTC 连接后启动；会话拆除开始后不再发送视频帧。
- [ ] 实测帧率约 5 fps、编码码率位于 256-512 kbit/s，初始目标为 384 kbit/s。
- [ ] 确认周期 IDR 间隔约 6 秒，并验证 RTSA 关键帧请求会强制生成新的 IDR。
- [ ] 在受限 Wi-Fi 下确认 RTSA 码率调整始终位于 256-512 kbit/s，且不会破坏音频通路。
- [ ] 反复结束、重启和中断会话，确认编码器、帧池、摄像头及 RTC 资源均能正常停止并再次启动。
- [ ] 确认结束会话或视频后 GPIO30 保持复位，GPIO27/1.2V 与 GPIO29/2.8V 仍开启；仅完整平台关闭时释放板级电源。
