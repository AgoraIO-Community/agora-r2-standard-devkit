# BK7259 LVGL UI 验证记录

文中 SDK 历史提交可在 [agent-studio-device-firmware](https://github.com/AgoraIO-Community/agent-studio-device-firmware)
的提交历史中核对。历史测试结果对应各节当时的代码与配置；当前 SDK 版本、目标补丁和摘要以
[`SDK_REVISION`](../../components/mybot/mybot_sdk/SDK_REVISION) 为准。

## 2026-10-17 R2 官方依赖迁移

当前产品源码位于父仓库 `projects/mybot/` 与 `components/mybot/`，两套 BK 依赖改为官方固定
提交：AVDK `584b947920c414230f9909375f008e7d6a2c6c6b`、Solution
`03aadb49756a5109bfede04002b9b6820f407c3f`。DNS、GC9503CV LCD、三麦 AEC 及摄像头辅助
适配通过父仓库补丁在隔离生成副本中应用，原始官方子模块保持干净。

未改变本节之后记录的 R2 UI、按键、电源和三路音频设置。AEC 保留 `20261014` 配套库，
591224字节、SHA256 `754ad74651b43cbe2cee29a0a882ee572fa70cf2b16dc4e6dc7006e695e65f90`；
VAD与舵机仍关闭，MCP只更新LCD。原ISRG+DigiCert双根证书保持，未导入main的区域单根优化。

中英文完整 AP/CP 构建和合包通过，约34秒/33秒，总计约1分13秒；当时的测试固件输出为
`releases/official-r2/bk7259-zh-CN-video.bin` 和 `bk7259-en-US-video.bin`。
`releases/` 是构建生成目录；可运行
`python3 scripts/build_all.py --output-root releases/official-r2` 生成当前版本的固件。
源码边界检查70个产品文件通过；20项构建准备测试、表情策略13组462条断言、HTTPS 5案例、
云台14组加CLI主机测试通过。agent-studio-device-firmware/AOSL/RTSA快照与迁移前一致；生成AVDK AP/CP/tools
及摄像头辅助组件与调整构建布局前的产品输入一致；对应输入、补丁和版本锁保存在
[已发布 R2 快照](https://github.com/AgoraIO-Community/agora-r2-standard-devkit/tree/5aa441e0b193974254fd7072392368183b258b7e)。

本次仅验证源码管理迁移和编译，仍需重新烧录验证LCD、配网、HTTPS、音频/AEC、视频与按键。
以下历史条目的提交、固件摘要及实机结论按当时上下文保留；当前统一入口与准备方法见
[根 README](../../README.zh-CN.md)及
[官方依赖说明](../../docs/OFFICIAL_DEPENDENCIES.md)。
在仓库根目录可运行 `python3 scripts/check_sources.py` 检查源码边界，
运行 `python3 scripts/build_all.py` 构建固件。随仓提供的主机回归入口为
`projects/mybot/tests/expression_policy/run.sh`、`projects/mybot/tests/gimbal/run.sh` 和
`projects/mybot/tests/https/run.sh`；它们的覆盖范围见各测试目录 README。

本文先记录当前像素眼镜界面的验证，再保留原始 LVGL 状态视图及 R2 功能的历史记录。
当前界面采用工艺灰/像素眼镜样式，默认中文、启用表情过渡与光标动画。
实现位于 `components/mybot/mybot_sdk/platforms/bk7259/bk7259_lcd.c` 和该平台的 `display/`；
项目配置为 `projects/mybot/ap/lv_conf_custom.h` 和 `projects/mybot/ap/config/bk7259_ap/defconfig`。
下文 `display/`、`include/` 下的板级文件路径均相对
`components/mybot/mybot_sdk/platforms/bk7259/`。
SSID 由配网模块复制投递。SDK 的 `include/src` 快照已单独同步到锁定版本；AOSL 和 AVDK 源码
未因原始 UI 移植修改；后续 R2 屏驱动版本见下节。
旧直接渲染字体及其独立许可证已删除，可从 Git 历史恢复；新资源许可见
`components/mybot/mybot_sdk/platforms/bk7259/display/SOURCES.md`。

## 2026-10-13 SDK main 同步与 RTC 日志修复（PR #112）

远端 `main` 最新提交为 `7a10696ff27dd947da0558104da6c5414929ea09`，SDK 版本仍为 1.2.0。
完整同步 58 个 `include/src` 文件，保留原有 HTTPS body 日志目标补丁，更新 revision lock、
第三方声明和中英文文档。最终聚合摘要为
`5e953e9aae9d359a9962d9acc9a5ea6ddd02bf4e147045ee72d2090410e3dd1f`。
相对 PR #111，公开接口和命令语义未变，源码仅修改 RTC 适配器。

SDK 的 RTM 日志不再截取前 512 字节；RTC 错误文本从固定 64 字节缓冲改为在借用回调
期间复制完整字符串（含 NUL），随事件投递到 RTC worker 后释放。复制或队列投递失败时
释放已申请资源；错误文本复制失败会丢弃该错误事件，连接保持原状态，上游测试明确覆盖。
没有修改 BK7259 平台适配、AOSL、AVDK、音频增益或 UI。时间字段忽略和 16 条 FIFO 去重
沿用 PR #111，命令执行仍不依赖 UTC。

日志范围需区分：SDK 将完整内容传给日志接口，普通 RTM 日志仍为 DEBUG，默认 NOTICE
配置不打印。当前目标 AOSL HAL 使用既有 8 KiB 整行 `vsnprintf` 缓冲（有别于上游 BK725x
参考的 512 字节缓冲），超出整行容量仍会截断。主机的 31 KiB 捕获测试不等于板上最长
消息完整输出，本次不改变 HAL 缓冲或日志等级。

验证结果：

- 核对新 EMBEDDED 全文；PORTING、平台头文件和 RTSA callback ABI 与 PR #111 相同。
  原有 RTSA 1.10.1 build 1278380 的头文件、库哈希通过组件校验；无新 GCC 原子操作。
- 文件集合与所选提交一致，除已记录 HTTPS 日志补丁外均逐字匹配上游。AOSL 聚合摘要
  仍为 `2e9fc3fa5571972fd31c1ab64546477a98550b6b210e38fec181c0f4daeae64e`；
  AVDK 保持当时固定的厂商基线，跟踪文件干净。
- 原工程 clean/build 和 AP/CP 合包通过；沿用当前中文、HTTP、视频与动画配置，
  任务前 defconfig 哈希一致。ELF 为 ARM32、小端、EABI5 hard-float，端点校验通过。
- SDK 主机完整测试 23/23 通过，用时 7.72 秒。新增用例验证 2 KiB RTC 错误文本在借用
  缓冲被改写后仍完整保存、第二次分配失败的清理及连接状态，以及 31 KiB P2P/频道消息
  与日志捕获内容逐字匹配。
- Clang 18.1.3、ASan/UBSan，video=ON、cloud AEC=OFF、60 ms、HTTPS=ON。两个 CMake/安装
  fixture 按上游设计关闭 sanitizer，AOSL/Agora 外部库未插桩，泄漏检测关闭；主机日志
  捕获 hook 绕过目标 8 KiB HAL，以上不代替实板日志和 RTM 联调。
- 两仓库 `git diff --check` 通过。旧边界脚本仍因历史版本锁和旧板卡假设失败，
  未计为通过。

| 区域 | 当前中文固件 |
| --- | ---: |
| AP Flash | 1,898,948 字节 |
| AP 静态 RAM | 157,196 字节 |
| AP DTCM | 35,744 字节 |
| CP Flash | 1,295,948 字节 |

原工程 `package/all-app.bin` SHA256：
`9edfd6da76a24699526a8ee8fc30dd4885bb6cb9c31b618a5f74ca091c3ae35d`。
历史验证基于当时的 R2 产品工作树。

## 2026-10-12 SDK main 同步与移除 UTC 依赖（PR #111）

远端 `main` 最新提交为 `19a40777768b7384549b46949ef6bb6310923463`，SDK 版本仍为 1.2.0。
完整更新 58 个 `include/src` 文件，保留既有 HTTPS body 日志补丁、命令解析源的构建注册，
同步 `SDK_REVISION`、第三方声明和中英文 README。最终聚合摘要为
`50e365e98307707e4f81ddbd6404ef086bd7e5e77abbc82b92bb805798b327c9`。
本节是当前基线；后面的 PR #110 记录保留该版本当时的行为和验证结果。

DeviceCommand 的 `issued_at` 与 `expires_at` 现在可省略，提供时也忽略，不再校验 UTC 或
命令有效期。发送方、频道、conversation ID、停止请求和 AOSL 会话代次校验继续生效。
去重缓存改为本会话最近接受的 16 个 command ID 的 FIFO：重复 ID 不刷新位置，新 ID
淘汰最早条目，已被淘汰的旧 ID 再次到达可以重新执行。会话开始和结束清空缓存。
音量与 PCM 静音沿用现有适配，表情 bit 4..7 到本地图案及 UART/云台的映射仍未接入。

按用户最新指示，撤回此前暂停的 UTC 尝试：AOSL `aosl_hal_time.c` 和 `AOSL_REVISION`、
AP 的 UTC 日志均恢复任务前状态，移除新增 NTP/时区配置。AOSL 摘要恢复为
`2e9fc3fa5571972fd31c1ab64546477a98550b6b210e38fec181c0f4daeae64e`，保留五项已有 HAL 补丁。
AP defconfig 与 SDK 同步前的用户配置哈希一致，仍为中文、HTTP、视频与动画开启。
AVDK 仍使用当时固定的厂商基线，跟踪文件干净。

验证结果：

- 已完整核对新提交的 PORTING/EMBEDDED；LCD/音频契约相对 PR #110 未变。完整文件集合
  与上游一致，源码仅含已记录的 HTTPS 日志目标补丁，未新增 GCC 原子操作。
- 原工程 clean/build 和 AP/CP 合包通过。生成配置确认 NTP 同步未启用；固件端点仍为
  中文 HTTP。命令解析和执行源码已不再引用 `aosl_time_ms()`。
- 相同 SDK 源在隔离主机工程的完整测试 23/23 通过，用时 7.71 秒。新增测试覆盖时间字段
  可省略、提供过期/未来/非法类型时忽略、重复字段拒绝、FIFO 满后淘汰、重复不刷新次序、
  会话清理及现有音量/静音功能。
- Clang 18.1.3、ASan/UBSan，video=ON、cloud AEC=OFF、60 ms、HTTPS=ON。两个 CMake/安装
  fixture 按上游设计关闭 sanitizer，AOSL/Agora 外部库未插桩，泄漏检测关闭；这些主机
  测试不代替实板 RTM 联调。当前 UI/平台源码与此前已验证版本一致。
- 两仓库 `git diff --check` 通过；旧边界脚本仍因历史 SDK/AVDK 锁和旧板卡假设失败，
  不计通过。最终远端查询仍为 `19a4077`。

| 区域 | 当前中文固件 |
| --- | ---: |
| AP Flash | 1,898,956 字节 |
| AP 静态 RAM | 157,196 字节 |
| AP DTCM | 35,744 字节 |
| CP Flash | 1,295,948 字节 |

原工程 `package/all-app.bin` SHA256：
`953dcbb122819b57639e807c2a3605ff19fe0d634a0d5f9c0fcce7d44cdaca5f`。
历史验证基于当时的 R2 产品工作树。

## 2026-10-12 SDK main 同步（PR #110）

远端 `main` 更新至 `6aaee82d19b3bc51b0bc153dd65f5bd8d48f2563`，SDK 版本仍为 1.2.0。
完整导入 58 个 `include/src` 文件，包含新增 DeviceCommand 解析源及内部头文件；本地组件
CMake 显式加入 `src/core/mybot_device_command.c`。保留原有、逐字一致的 HTTPS body 日志
目标补丁，更新 `SDK_REVISION` 和第三方声明。完整路径集合与所选提交一致，其余文件与
上游逐字一致；最终聚合摘要为
`e60483f268539d0ce25ec100029af2592a1c7bedf5078f00bc1e9a8782e428be`。

SDK 新增通过 RTM P2P/频道接收 DeviceCommand v1、音量设置/增减、PCM 静音及四种表情请求；
执行前核对发送方、频道、会话、代次与 UTC 有效期，使用 16 项 command ID 记录去重。
静音保持麦克风采集与上行，云端 AEC 使用实际播放的 PCM；本工程的本地 ADC2 回采 AEC
配置沿用原实现。VP 与 listening/thinking/speaking 的格式、原有 bit 和显示路径兼容。
新增原子字段及操作使用 AOSL 接口；上游 GCC 原子 coverage 选项不进入本地固件构建。

当前 MCP 实板使用限制：

- BK725x `aosl_hal_get_time_ms()` 仍返回 `rtos_get_time()` 开机时间，而新 SDK 通过
  `aosl_time_ms()` 校验 UTC `issued_at/expires_at`。使用真实当前 UTC 的命令会被拒绝，
  完整 MCP 使用还需要 UTC 时钟适配；本次同步不改变 AOSL HAL。
- LCD content/ops 结构未变，新增表情 bit 4..7。当前 BK7259 view 继续只消费 VP/阶段
  bit 0..3，并按公开契约忽略未识别位；还没有 DeviceCommand 到本地表情或 UART/云台的
  执行映射。后续表情接入需要按一次性命令处理，不能把它并入长期 indicator 去重。

验证结果：

- 完整核对所选提交的 PORTING/EMBEDDED、涉及的 LCD/音频公共契约，以及 BK7259 RTM
  和项目参考。AVDK 基线不变且跟踪文件干净；
  AOSL 摘要仍为 `2e9fc3fa5571972fd31c1ab64546477a98550b6b210e38fec181c0f4daeae64e`。
  BK7259 RTSA 1.10.1 build 1278380 的头文件、库摘要与组件锁一致。
- 原工程 clean/build 与 AP/CP 合包通过。当前配置为中文、HTTP、视频与动画开启，任务前
  AP defconfig 哈希不变。ELF 为 ARM32、小端、EABI5 hard-float；确认链接新命令解析、
  静音、音量及 BK7259 AOSL 原子 HAL 符号，构建未混入主机 SDK 路径。
- 带原有 HTTPS 日志补丁的相同 SDK 源在隔离主机工程中完整测试 23/23 通过，用时 8.53 秒。
  Clang 18.1.3，video=ON、cloud AEC=OFF、60 ms、HTTPS=ON；SDK 和 21 个单元/公开头文件
  可执行测试启用 ASan/UBSan，两个 CMake/安装集成测试按上游设计不启用 sanitizer。
  主机 AOSL 使用相同上游 base `84e0860` 的临时 Linux HAL；AOSL/Agora 外部库未插桩，
  LeakSanitizer 因环境限制关闭。以上不代表 BK HAL 和实板联调已验收。
- HTTP-only 主机首轮为 22/23：唯一失败为 HTTP client 测试无条件要求 HTTPS URL 解析成功，
  与 HTTP-only 配置不兼容；未修改 SDK 或测试以隐藏此问题。HTTP 中文固件构建及地址校验通过。
- 当前实际 LVGL view 在新公共头文件下，中英文与关闭动画三配置的 ASan/UBSan 回归通过，
  包括新增四表情位被忽略时保持阶段/VP、表情、过渡与恢复优先级，以及 25 次创建销毁。
- 两仓库 `git diff --check` 通过。旧边界检查器仍使用过期版本锁与旧板卡规则，
  执行失败，不计通过。

| 区域 | 当前中文固件 |
| --- | ---: |
| AP Flash | 1,899,900 字节 |
| AP 静态 RAM | 157,324 字节 |
| AP DTCM | 35,744 字节 |
| CP Flash | 1,295,948 字节 |

原工程 `package/all-app.bin` SHA256：
`aea9a3213f676c656ef06f7819b6de8bf44b289d9a711dbc124931e1c3bdfa83`。
历史验证基于当时的 R2 产品工作树。

## 2026-10-12 真实生命周期与会话状态

第三行由真实 SDK 生命周期驱动，READY 显示 `> state: ready`；会话中结合 SDK LCD
indicator 显示 `listening/thinking/speaking | vp:pending/ok`，没有有效阶段时回到
`in_conversation`。`vp:ok` 表示本会话已收到声纹成功通知，`pending` 表示尚未确认。
会话字段仅在运行状态和 LCD 页面同时处于 IN_CONVERSATION 时出现，挂断或断网后隐藏。
恢复页明确显示 `> state: factory_reset`，避免残留上一会话的阶段和 VP；该文字为产品流程，
不表示新增 SDK 枚举。四行坐标、字体、表情、光标和显示缓冲沿用现有实现。

应用在现有状态观测中读取 `mybot_get_state()`，补齐 PAIRING 名称映射，经独立
`include/bk7259_lcd.h` 接口复制到 LCD 快照。LCD 内部去重，首次准备完成及停止返回后
也发布观测值；仅 UI owner 操作 LVGL。SDK LCD 回调继续提供语义页面与 indicator。
启动前产品配网时 SDK 可以仍为 STOPPED，配网指导页继续可见。同步停止期间使用现有
STOPPING 指导页，应用轮询不保证捕获每个瞬态；停止完成后立即发布实际 stopped 值。
恢复提示仍拥有最高优先级。状态更新不清除表情 override，不重启过渡或光标时钟，
SLEEP 图案不再决定生命周期文字。

实现基于当时的 R2 产品工作树。合同已核对 SDK `cfd178a` 的完整 PORTING/EMBEDDED、公开
`mybot.h`/`platform/mybot_lcd.h` 和 BK7259 RTM/声纹参考。SDK revision lock 聚合摘要为
`bd59fb9a4f7aced74d5b8e770da869897229316c284e4ccded4d2aa8e667fa7a`，AOSL 摘要为
`2e9fc3fa5571972fd31c1ab64546477a98550b6b210e38fec181c0f4daeae64e`。
AVDK 保持当时的固定基线，跟踪文件干净。SDK、AOSL、RTSA
和板级引脚不涉及本次修改，任务前 defconfig 与 RTM 文件哈希复核一致。

验证结果：

- 原工程执行 clean/build，随后包含恢复页文字修复的最终增量构建成功，AP/CP 合包通过。
  固件沿用当前用户配置：中文、HTTP、视频与 UI 动画开启。生成配置、ELF 端点及新增状态
  文字已核对，固定 `online/standby` 文字不再链接。
- 使用真实目标 view、字体及锁定 AVDK LVGL 9.5 主机库，中文、英文、关闭动画三种配置
  均通过 `-Wall -Wextra -Werror` 与 ASan/UBSan。覆盖真实状态、VP 与阶段变化、阶段清空、
  未知位、运行状态/页面不一致、借用内容修改、250 ms 过渡期间连续更新、SLEEP override、
  恢复页去除旧会话字段及每种配置 25 次创建销毁。LeakSanitizer 因环境限制关闭，不计通过。
- 实际 LVGL 渲染输出已检查，最长 `> state: in_conversation | vp:pending` 为 333 px，
  可放入现有 366 px 文字区域。字体覆盖检查通过，仍为 152 个操作引导字符。
- 两仓库 `git diff --check` 通过。旧边界检查器仍固定过期 SDK/AVDK 规则与旧板卡 LCD
  引脚；执行失败，未计为通过。

| 区域 | 当前中文固件 |
| --- | ---: |
| AP Flash | 1,895,676 字节 |
| AP 静态 RAM | 155,140 字节 |
| AP DTCM | 35,744 字节 |
| CP Flash | 1,295,948 字节 |

原工程 `package/all-app.bin` SHA256：
`d647dd49da2699644594ea50db51140211cc41673c4c6e9cc6789eca6bb45822`。
本次真机 RTM 状态、挂断、断网重连和长按恢复效果仍待用户烧录验证。

## 2026-10-10 四行终端与完整 23 表情

在 `display/lvgl_view.cc` 中恢复四行文字，沿用前一版 9/16 比例、Terminal 15/16 字体和
37 像素源坐标行距；光标移到第四行末尾：

```text
> OIM / R2
> mood: neutral
> state: online
> agora / realtime
```

`mood` 随表情更新为小写名称；当时仅 SLEEP 的 `state` 为 `standby`，这些字段为界面装饰。
新增独立 `include/bk7259_expression.h`、名称转换模块和显示所有者的 set/clear 请求，补齐
全部 23 个表情。默认 READY 为 NEUTRAL，会话为 GREETING；其余 21 个供后续调用，不自动
轮播，没有本次 MCP/RTM 命令或 UART/舵机集成。流程页面切换清除表情覆盖，仅 indicator
更新保留覆盖；配网、配对和恢复出厂提示优先。

渲染使用 66 个固定对象、两组形状和一个定时器，保留 250 ms 过渡。EXCITED 的眼睛跳动、
THINKING/BORED 的横移、PROCESSING 的眼睛运动、SLEEPY 的哈欠、SLEEP 的呼吸以最高
20 fps 更新；静态表情只更新 500 ms 光标。颜色、低亮度和短暂入场闪亮由视图配置。
LISTENING 下移 18 个源坐标像素，
SAD/SLEEPY/APOLOGY 下移 28；CURIOUS/CONFUSED/PLEADING 的整体 8° 倾斜暂未实现，避免
整屏旋转图层。

最初的三角扇填充在实际 LVGL 中出现内部抗锯齿接缝，已改成五块只读 A8 几何蒙版，
合计 13,631 像素字节；生成脚本和几何定义见 `display/SOURCES.md`。低亮度曲线先与
背景预混颜色再绘制，避免半透明圆头/线段重复叠加导致亮度不均。最终星形、心形和椭圆
实绘填充平整，困倦/休眠线条亮度均匀。没有新增 UI 任务、整帧 canvas、扫描缓冲或整组
透明度/旋转图层。哈欠只缩放小型椭圆蒙版，仍可能使用 LVGL 的临时条带内存；运行时总堆
峰值与栈高水位尚需实板测量。ARM ELF 中 `View` 为 2408 字节。

验证与产物：

- 实际锁定 LVGL 的中文、英文、关闭动画配置通过 ASan/UBSan；覆盖全部 23 表情的六个
  时间采样、完整四行标签、名称转换与非法参数、重复请求不重播、indicator 不覆盖表情、
  流程默认映射、配网/配对/恢复页面、借用文本复制和 25 轮创建/销毁。关闭动画同时通过
  `-Wall -Wextra -Werror`，所有表情保持静态。LeakSanitizer 因环境限制关闭。
- 从当前 LCD 源提取实际请求函数，用 HAL stand-in 验证 23 个编号、锁失败、生命周期拒绝、
  最后请求合并、重复请求去重、内容复制、流程/indicator 区别和恢复出厂优先级，ASan/UBSan
  通过。该项验证软件快照语义，不代替实际 RTOS/LCD 并发或硬件验证。
- 原工程中文 AP/CP 编译与合包、独立英文 `scripts/build_all.py --language en-US` 通过。
  最终原工程仍为中文，动画与视频启用。字体覆盖、蒙版复现和 `git diff --check` 通过。
  旧边界检查器仍因过期 AVDK gitlink 规则失败，未计为通过；本次没有修改该检查器。

| 区域 | 中文 | 英文 |
| --- | ---: | ---: |
| AP Flash | 1,894,132 字节 | 1,894,100 字节 |
| AP 静态 RAM | 155,020 字节 | 155,020 字节 |
| AP DTCM | 35,744 字节 | 35,744 字节 |
| CP Flash | 1,295,948 字节 | 1,295,948 字节 |

中文原工程 `package/all-app.bin` SHA256：
`749783e211b8ea6e25f37c7af1dadaa9777a6d605f586685eb8aec8dc539116d`；
独立英文固件 SHA256：
`a1bf3eaaf3481dbc40debaf8e41d56c8302c5b1766d40b22ea4463b7178cd106`。
AVDK、SDK/AOSL 和用户任务前的
RTM 日志修改保持不变，合同与依赖基线沿用下方记录。上述本机构建保留了该 RTM 日志
调整，本次 UI 提交仅包含显示源码、资源、配置和文档，该用户调整仍留在工作树。
基础像素 UI 已由用户确认实板渲染正常，四行文字和新增 21 表情仍待此次烧录验证。

## 2026-10-10 实板反馈后的比例与字体校准

用户确认首版可正常渲染，但图案大小和文字位置不协调。进一步测量发现：首版
512x448 安全区把镜框压到屏宽的 48%，Unscii 16 的字符 advance 则是 16 像素，终端行
宽达 288 像素；`</>` 还被容器居中偏移。

校准后，虚拟画布为 576x504，原点 `(32,-12)`，按 9/16 统一缩放，裁掉上下空白。
镜框实际宽从 307 增至 345 像素；眼睛、嘴和线宽使用同一比例。品牌文字恢复源坐标
`(104,108)` 和 `(104,145)` 的行网格，`</>` 在 `(795,112)` 左对齐，光标跟随实际字宽。

终端使用由固定 DejaVu Sans Mono Bold 输入生成并重命名的 BK7259 Terminal 15/16：
15 像素 ASCII 子集含 95 字符、4479 字节 bitmap；16 像素符号子集含 3 字符、155 字节
bitmap，合计 4634 字节。LVGL 会把 fractional advance 四舍五入；
15 像素字体实测字宽为 9 像素。
字体输入 SHA 和 Bitstream Vera 许可记录在 `display/SOURCES.md` 及
`display/assets/licenses/TERMINAL_FONT_LICENSE.txt`，生成脚本为 `display/generate-pixel-font.py`。
中文操作字库仍为 152 字符、18,458 bitmap 字节，没有新动态字体解码或显示缓冲。

| 元素 | 校准后 LVGL 宽度 |
| --- | ---: |
| `> OIM / R2` | 90 像素 |
| `> agora / realtime` | 162 像素 |
| `</>` | 30 像素 |

字形起点、光标、行高和所有可见标签边界均验证完整。
正常待机/会话状态、indicator 忽略、过渡、光标、双语操作引导和恢复出厂优先级保持原语义。
中文、英文、关闭动画三种实际 LVGL 配置再次通过 ASan/UBSan、最大借用内容复制和
25 轮创建销毁；关闭动画还通过 `-Wall -Wextra -Werror`。LeakSanitizer 保持关闭。

最终中英文 AP/CP 编译与合包通过，原工程最后保留中文固件。ELF 确认链接 Terminal 15/16，
不再链接 Unscii 16；AVDK、AOSL、SDK 和任务前用户 RTM 日志改动均未修改。

| 区域 | 中文 | 英文 |
| --- | ---: | ---: |
| AP Flash | 1,872,884 字节 | 1,872,868 字节 |
| AP 静态 RAM | 153,244 字节 | 153,244 字节 |
| AP DTCM | 35,744 字节 | 35,744 字节 |
| CP Flash | 1,295,948 字节 | 1,295,948 字节 |

中文 `package/all-app.bin` SHA256：
`19eff45485a5c1efcc29afbfe528f8a73274af9ec7be3d70efe7df8264a9ccfa`。
校准版在该阶段尚未完成用户真机验证，也尚未单独提交。

## 2026-10-10 像素眼镜 UI 重写（首版）

本次在当时的 R2 产品工作树实现。重写 `display/lvgl_view.cc/.h`，更新字体、组件源列表、
动画配置和双语文档。AVDK 保持当时的固定基线，跟踪文件干净；
LCD 驱动、显示任务、AOSL、AEC、按键和恢复出厂执行逻辑没有修改。

`display/lvgl_view.cc` 实现以下像素眼镜界面：

| 场景 | 显示 |
| --- | --- |
| SDK READY | 米白 `#f1f1ee` 镜框、双弧眼、微笑 |
| SDK IN_CONVERSATION | 暖金 `#f1ca79` 镜框、左弧眼、右圆点眼、微笑，保持到会话结束 |
| 配网、配对、错误、恢复出厂 | 同风格中英文操作引导，保留热点、门户入口与网页控制台配对码 |

正常界面保留两行静态终端装饰、`</>`、500 ms 光标，取消 mood/state、声纹、聆听/思考/
回复指示及通知。仅 indicator 更新不会改变表情或重启 250 ms 过渡。恢复出厂提示优先级
保持最高。1024x896 坐标等比缩为 512x448，居中于逻辑屏幕 `(64,16)`，保留圆角安全区。
初版深色渐变在 RGB565 实绘中出现色带，最终使用 `#050708` 固定黑底和灰色装饰。

渲染使用 33 个固定 LVGL 对象、一个定时器和 408 字节曲线点数据；32 位目标的 View 为
636 字节。过渡时每 50 ms 更新，静态表情只有光标每 500 ms 翻转。没有新任务、canvas
整帧缓存或组透明度图层；已有双扫描缓冲 1,228,800 字节、条带 20,480 字节和 UI 栈
8,192 字节继续复用。旧 emoji 源文件和许可记录保留，但 `lvgl_assets.c` 不再参与链接。
终端使用内置 Unscii 16；操作引导的 MyBot UI Sans 子集为 152 字符、18,458 bitmap 字节，
来源字体 SHA256 和生成参数保持固定。

验证了锁定 `main=cfd178a` 的 PORTING/EMBEDDED 和 LCD 平台合同、项目构建规则及 LVGL 9.5
line-point 借用、定时器与字体接口。用户要求隐藏指示，因此本次只调整平台显示消费方式，
不修改 SDK 的声纹/服务端状态协议或生命周期。SDK 保留任务前已有的两处 RTM 日志 NOTICE
改动：当前 include/src 摘要为 `fa13a414fb688c92738c4643363cea77a72b723b7e53da4a78fc23dc1354e5a4`。
在临时审计副本中去除该用户补丁后，摘要为 revision lock 中的
`bd59fb9a4f7aced74d5b8e770da869897229316c284e4ccded4d2aa8e667fa7a`；本任务未修改 SDK 源或锁。
AOSL 摘要与其锁一致，RTM 文件哈希与任务前一致。

真实 LVGL 主机验证使用 640x480 RGB565 和 16 行条带，直接编译本次 view、字体及锁定 AVDK
LVGL，适配仅限临时主机内存/断言后端：

- 中文、英文动画配置和英文关闭动画配置均通过 ASan/UBSan；关闭动画还通过
  `-Wall -Wextra -Werror`。LeakSanitizer 因 ptrace 环境关闭，不作其通过结论。
- READY → IN_CONVERSATION → READY、过渡期间及之后的 indicator 更新、500 ms 光标边界、
  所有操作与恢复页面、固定文本高度/安全区域、最大 SSID 和配对码的借用内容复制均通过。
- 每种配置 25 次创建/销毁后对象与 timer 回到初始显示基线；恢复页面不被 SDK 更新覆盖。

中英文 AP/CP 构建和合包通过。中文在原工程 clean/build，最后再次构建最终纯黑背景版本；
英文通过独立项目 `scripts/build_all.py --language en-US`。最终中文开启 GC9503CV、视频和
动画，ELF 已确认链接 Unscii 16、不再链接旧 emoji；未改屏幕方向或音视频参数。

| 区域 | 中文 | 英文 |
| --- | ---: | ---: |
| AP Flash | 1,869,660 字节 | 1,869,596 字节 |
| AP 静态 RAM | 153,244 字节 | 153,244 字节 |
| AP DTCM | 35,744 字节 | 35,744 字节 |
| CP Flash | 1,295,948 字节 | 1,295,948 字节 |

中文 `package/all-app.bin` SHA256：
`84413db6531549892a4877ebe0f0733e23d2431dd9af09673d6543c96a71a51b`。
相对前次 SDK 同步中文构建，AP Flash 减少 71,440 字节，静态 RAM 增加 456 字节。
运行时堆峰值、线程栈高水位、实际刷屏负载仍需上板测量。

`git diff --check` 和字体覆盖检查通过；旧边界检查器
仍使用过期版本、旧 LCD/配网规则，未计为通过。首版在该验证阶段尚未提交，
也未完成真机验证。烧录后检查待机/打招呼、挂断回待机、圆角可见区域、配网/配对、长按恢复出厂，以及
同时音视频运行时的光标和过渡效果。

## 2026-10-10 SDK main 同步与挂断顺序复核

远端 `main` 已确认并同步到 `cfd178a02fa94775cf01dc77e344f4ce5930225c`（PR #109），
SDK 版本仍为 1.2.0。整套 56 个 `include/src` 文件从该提交导入；相对上一版 `0643978`
仅生命周期实现与内部结构变化，公开平台头文件没有变化。
`SDK_REVISION` 和第三方声明已更新，最终聚合 SHA256 为
`bd59fb9a4f7aced74d5b8e770da869897229316c284e4ccded4d2aa8e667fa7a`。

唯一保留的目标补丁是既有 HTTPS 请求/响应 body 日志；RTM 日志等级调整已由用户回退，
`src/rtc/mybot_agora_rtc.c` 与上游逐字一致。AVDK、AOSL、RTSA、R2 板级配置和平台适配
没有修改。本次修改基于当时的 R2 产品工作树。

SDK 现在先停止视频、退出 RTC/RTM、清理会话 PCM 并恢复 READY，再用原会话 ID 和停止
原因发送一次 HTTPS 停止通知。通知失败只记录 warning，不重试，也不会重新开启媒体。
断网/网络代次变化的响应仍会过滤，设备鉴权被拒绝时仍重新配对。
HTTP 仍在控制线程同步执行：本地音视频和 LCD 不再等待远端响应，但紧接着开始新会话或
调用 `mybot_stop()` 仍可能等待正在执行的通知请求。远端自动结束依赖设备服务支持设备
离开 RTC channel 后终止会话。

验证结果：

- 已核对该提交的完整 PORTING/EMBEDDED 文档、涉及的平台契约及 BK7259 RTSA/RTM 参考。
  单键状态查询、LCD 挂接、双麦 ADC2 AEC、视频停止顺序和恢复出厂均无需新增平台改动。
- 最终带 HTTPS 日志补丁的 vendored SDK 使用 video=ON、cloud AEC=OFF、60 ms ptime，
  上游既有 22/22 主机测试通过，覆盖本地停止先于 HTTP、单次通知、重复请求、断网、
  重新配对及 app/RTC/media。19 个单元测试和公共头文件测试使用 ASan/UBSan；两个 CMake
  集成测试按上游配置关闭 sanitizer。LeakSanitizer 因 ptrace 环境限制关闭，不计为通过。
- 中文原工程执行 clean/build，再核准回退 RTM 日志后的最终增量构建；英文使用独立工程
  `scripts/build_all.py --language en-US`，两者 AP/CP 编译与合包通过。
- 最终中文配置为 `CONFIG_MYBOT_LANGUAGE_ZH_CN=1`、`CONFIG_MYBOT_VIDEO=1`；ELF 为 ARM32、
  小端、EABI5 hard-float。`git diff --check`、快照路径集合、实际摘要和声明补丁复核通过。
- 旧边界检查器仍使用过期 SDK/AVDK 锁及旧 LCD/配网假设，未计为通过。

| 区域 | 中文使用量 | 英文使用量 |
| --- | ---: | ---: |
| AP Flash | 1,941,100 字节 | 1,941,060 字节 |
| AP 静态 RAM | 152,788 字节 | 152,788 字节 |
| AP DTCM | 35,744 字节 | 35,744 字节 |
| CP Flash | 1,295,948 字节 | 1,295,948 字节 |

中文版 `package/all-app.bin` SHA256 为
`9a7d22fe63d7ed6c064c30e1dfaa58ae316279ad474e74a53e97faa0c3fe42db`。
尚未对这版固件完成真机验证；烧录后应检查松手至本地音视频停止的耗时、屏幕保持
点亮、连续开关会话、离线挂断和长按恢复出厂。

## 2026-10-10 R2 与恢复出厂复核

功能验证使用当时固定的 R2 产品及厂商基线，包含 GC9503CV R2 屏驱动，
厂商依赖工作树干净。
SDK 固定为 `064397882187a242be25a525a91df83d353b78bb` 加已声明的 HTTP 日志补丁；
SDK/AOSL 摘要与各自 revision lock 一致，恢复出厂功能未修改这两个快照。

当前显示和生命周期与下方历史配置的差异：

- GC9503CV MIPI 原生 480x640，逻辑 RGB565 640x480；GPIO20 控制 3.3V，GPIO29 控制
  LCD/摄像头共用 2.8V，GPIO42 复位，GPIO40 背光低有效。
- 两块扫描缓冲共 1,228,800 字节，一块 640x16 绘制条带为 20,480 字节，UI 栈仍为
  8,192 字节；对象与绘制任务的动态分配峰值仍需在目标机测量。
- 显示持有共用 2.8V 电源到完整平台关闭，视频停止只复位摄像头，避免结束会话后黑屏。
- GPIO47 短按在松手时切换会话，长按 5 秒请求恢复出厂；按键准备时已按住须先松手再按。
  产品任务在联网前检查持久恢复意图，并在 SDK 与网络停止后清理本地鉴权、Wi-Fi 和音量。

恢复页面由既有显示任务和视图渲染。LCD 快照将产品恢复状态设为优先覆盖，SDK 的
STOPPING、render 或解挂不会替换恢复提示；没有新增任务、UI 定时器或帧缓冲。

| 阶段 | 中文标题 / 提示 | 英文标题 / 提示 |
| --- | --- | --- |
| 恢复中 | 正在恢复出厂设置 / 请勿断电 | Restoring factory settings / Keep power connected |
| 失败 | 恢复失败，请长按重试 / 长按5秒重试 | Reset failed. Hold 5s to retry / Hold 5s to retry |
| 重启中 | 恢复完成，正在重启 / 请勿断电 | Reset complete. Restarting / Keep power connected |

20 像素字体含 170 个字符，覆盖固定中文恢复提示，bitmap 共 22,018 字节。
`display/generate-lvgl-font.py` 固定源字体 SHA256，许可和来源记录在 `display/SOURCES.md`。
根目录可执行以下命令检查 UI 文案覆盖，无需源字体或 Pillow：

```sh
python3 components/mybot/mybot_sdk/platforms/bk7259/display/generate-lvgl-font.py --check
```

本次中英文 AP/CP 构建和合包通过。中文版使用原工程 `make`，英文使用
`scripts/build_all.py --language en-US`；该脚本的中文地址校验仍是旧服务地址，未将
默认双语脚本的当前结果记作通过。中文原工程产物为 `package/all-app.bin` 与
`package/app_pack.rbl`，命令和路径见 [README_CN.md](./README_CN.md)。

| 区域 | 中文使用量 | 英文使用量 |
| --- | ---: | ---: |
| AP Flash | 1,941,652 字节 | 1,941,612 字节 |
| AP 静态 RAM | 152,796 字节 | 152,796 字节 |
| AP DTCM | 35,744 字节 | 35,744 字节 |
| CP Flash | 1,295,948 字节 | 1,295,948 字节 |

恢复功能的主机验证包括：

- 9 组真实按键源码手势用例，覆盖松手短按、消抖、5 秒边界、单次长按、延迟扫描、
  开机已按住、时钟回绕、未挂接 SDK 和恢复优先级，ASan/UBSan 通过。
- 产品恢复流程用例覆盖顺序、持久化/SDK/网络/删除失败、新请求和开机续做。
- 31 组持久化故障注入，以及未修改 EasyFlash 源码配合 NOR 模型的 6 组验证。
- 正常配网提交、9 个取消时机及 AP/扫描/STA 停止失败后重试。
- 字体覆盖、最终 `bk7259_*` 接口命名、`git diff --check`、SDK/AOSL 摘要和 AVDK 状态复核。

用户已反馈 R2 屏幕、双模拟麦上行与 AEC、结束会话后屏幕保持点亮、长按恢复出厂真机测试
通过。断电续做、Flash/驱动故障和长时间稳定性仍需专项真机验证；初始 UI 的截图与内存
测试是下方保留的历史结果，本次没有把它们当作已在 R2 全量重跑的结果。

持久恢复的边界：意图由产品任务开始处理请求时才写入，提前断电可能丢失尚未消费的
内存请求。EasyFlash 公共查询不能区分所有物理读取失败或冷扫描 CRC 损坏与不存在；
最终标记删除报错时尝试重建，若重建也失败则不能保证意图保留。真实 EasyFlash 主机
验证观察到既有 `ef_env.c:473` 有符号移位 UBSan 报告，未修改 vendor 源码。
旧边界检查器仍因过期版本与LCD/配网假设而失败，未计为通过。

## UI 移植时的基线与契约

本节保留当时的验证版本。后续 SDK 更新以 `components/mybot/mybot_sdk/SDK_REVISION` 为准。

- AVDK：使用当时固定基线中的 LVGL 9.5.0，工作树保持干净。
- 产品工程：改动留在当时的工作树，未提交或更新根 gitlink。
- 保留的视图与资产集成可在[已发布 R2 显示快照](https://github.com/AgoraIO-Community/agora-r2-standard-devkit/tree/5aa441e0b193974254fd7072392368183b258b7e/components/mybot/mybot_sdk/platforms/bk7259/display)
  核对；原始布局、字体与 emoji 的作者及许可证见该目录的 `SOURCES.md`。
- agent-studio-device-firmware SDK：`4ae239c804257f8b5c557e5879b54d9a88d80847`，RTC 日志级别和 AOSL 恢复已包含在
  上游快照，HTTPS body 日志仍由 `SDK_REVISION` 记录。`include/src` SHA256 复核为
  `84a5a0a015de7be6a91658ebc3de1f6958a8e595750d859426dd3a7dae1aa76c`。
- AOSL 基线：`84e086084ebcd0ae2455a0ce5721950c5fe2e656` 加 `AOSL_REVISION` 已记录的 BK7259 补丁。
  内容 SHA256 复核为 `2e9fc3fa5571972fd31c1ab64546477a98550b6b210e38fec181c0f4daeae64e`。
- 既有 RTSA 包：BK7259 v1.10.1 build 1278380，未更换；完整构建保留其头文件和库的哈希校验。
- 已核对锁定 SDK 的完整 `docs/PORTING.zh-CN.md`、`docs/EMBEDDED.zh-CN.md` 和
  `include/mybot/platform/mybot_lcd.h`，以及 BK 的显示、DPU、帧分配和 RTOS 接口。
  SDK 输入只按平台头文件消费；SDK 销毁只解挂，产品显示持续到平台关闭。

## 2026-09-23 SDK 同步复核

- 通过远端 `main` 确认最新提交为 `83fbcb0969da4c73a5912326d699f90ec634b28e`，版本号仍为 1.2.0。
  全量同步 56 个 `include/src` 文件；唯一目标补丁仍为原有 HTTPS body 日志，补丁内容不变。
  新聚合 SHA256：`3f89476f880518b0f0d68e45d709023a0cb0d6146865d8e32b8a95ceb4e46b24`。
- 此次修改基于当时的产品基线，未自动提交。
  AVDK、AOSL 和 RTSA 仍使用本记录中的固定版本；AOSL 内容与 RTSA 头文件/静态库摘要复核一致。
  BK7259 平台、圆角 UI 和 UID 设备 ID 源码未改变，无新增 GCC 原子接口依赖。
- 核对最新 `PORTING.zh-CN.md`、`EMBEDDED.zh-CN.md` 和应用生命周期、音频、视频、Wi-Fi、
  LCD、提示音公开头文件。视频控制改由 SDK 控制线程串行执行，提示音资源保留到 RTC 回调
  排空后销毁；现有平台 ops 和目标 CMake 源文件列表无需调整。
- 在隔离宿主目录以 `MYBOT_ENABLE_VIDEO=ON` 构建并运行现有 `ctest`，21/21 通过。
  测试提交 `655bf9c5aa53ea90561b0ad61af0ddd97a1d9c32` 与最终远端合并提交的完整 Git tree
  均为 `7173ad69ca9348319a8131a57053bd9bdb2381f9`。覆盖视频启停/回调、提示音 stop/destroy、
  RTM 三状态/声纹以及设备生命周期和 JSON；这些宿主测试使用上游源，不包含目标 HTTPS 日志补丁。
- 原工程执行 clean/build，带目标补丁的 AP/CP 完整编译与合包通过。
  AP Flash 1,926,024 字节（41.00%），AP 静态 RAM 152,900 字节（38.88%）；
  CP Flash 1,296,900 字节（89.95%）。ELF 为 ARM32 小端 hard-float，无未解析符号。
  `all-app.bin` 为 3,433,376 字节，`app_pack.rbl` 为 2,027,888 字节。
- 逐文件验证确认除声明补丁外与上游完全一致，`git diff --check` 通过。
  旧边界检查器仍因固定旧版本和“不使用 LVGL”的旧规则失败，未将其记作通过。
  Kconfig 和链接器既有警告仍存在。此次未重新执行 UI 宿主截图测试或实板音视频验证。

## 初始 UI 移植构建（历史记录）

当时在启用 LVGL 后清理旧生成配置，AP、CP 完整构建和合包通过，最终增量构建也通过。
产物为完整烧录镜像 `all-app.bin` 和 OTA 包 `app_pack.rbl`；
当前构建命令与生成路径见[根 README](../../README.zh-CN.md#构建固件)。
ELF 为 ARM、32 位、小端、hard-float ABI。

| 区域 | 使用量 | 分区占用 |
| --- | ---: | ---: |
| AP Flash | 1,926,128 字节 | 41.00% |
| AP 静态 RAM | 152,908 字节 | 38.89% |
| CP Flash | 1,296,900 字节 | 89.95% |

相对移植前记录，AP Flash 增加 324,144 字节，静态 RAM 增加 792 字节。
运行时另需双扫描缓冲 492,800 字节、绘制条带 12,320 字节和 UI 栈 8,192 字节。
最终链接未引入 vendor LVGL 显示线程、FreeRTOS 绘图线程或 GPU 绘制后端。
工具链仍报告既有 `_gettimeofday` stub、GNU-stack 和 RWX 段警告，不能视为零警告构建。

## 初始 UI 宿主验证（历史记录）

- 真实 LVGL 9 渲染：全部 10 个页面、中英文、深浅主题、三种会话状态、长配对码与
  SSID 滚动、借用输入立即改写、100 次创建/销毁；已检查截图布局。
- 直接包含实际 `bk7259_lcd.c`，替换 HAL/RTOS/LVGL 调用的后端测试：11 组通过。
  覆盖分条/局部旋转和未修改像素保留、双帧占用超时、补绘、提交失败的不同释放语义、
  退出与 deinit 失败后的资源保留和重试、回调退出屏障、快照复制与解挂、首帧失败、
  重新 prepare，以及分配溢出与 OOM。
- 两组测试启用 ASan/UBSan；后端还启用 `-Wall -Wextra -Werror`。
  LSan 因沙箱 ptrace 限制未启用，不作其检测通过的结论。
- 64 位宿主的 LVGL 分配跟踪：创建后 7,286 字节，首次渲染后 7,054 字节，
  真实 UI 工作负载峰值 9,499 字节；三种动画各运行 60 秒，未观察到保留分配增长。
  100 次创建/销毁后基线稳定，最终 `lv_deinit` 后跟踪分配归零。
  这些数值不含帧缓冲、条带、线程栈和分配器开销；宿主使用 24 行条带，目标为 16 行，
  且指针宽度不同，不能替代目标机内存峰值测量。
- LVGL 内部仍会为每次绘制分配/释放任务，动画测得约 240–370 次/秒，单次最大请求
  304 字节；自有动态标签使用固定缓冲。64 KiB 配置只限制绘制层像素，不是 UI 总堆上限。

## 适配边界与上板检查

显示原子变量全部走 BK7259 AOSL HAL。两帧仅由释放回调归还；释放无法确认时保留资源。
若驱动在接收帧前失败又不回调，保守保留可能要求重启，不能擅自假定该帧已释放。
UI 工作线程退出、显示关闭和回调等待均有超时；通知信号量按进程生命周期复用。

锁定 LVGL 的默认绘制分配在 HSRAM 耗尽时回退 PSRAM，但配对释放仍为 HSRAM。
适配层在初始化后只替换默认、字体、图片三组 handler 的分配函数，禁止该回退。
为保留原有像素格式复制、对齐和 stride 回调，使用固定 LVGL 9.5 的
`src/draw/lv_draw_buf_private.h` 访问 handler 字段；升级 LVGL 时需复核此处。

`git diff --check` 通过；SDK/AOSL 摘要和 vendor 干净状态单独复核通过。
旧边界检查器已运行但未通过：
它仍锁定旧版本/摘要和“不使用 LVGL”的旧方案，亦拒绝既有集成依赖。
不将它的结果记作通过。

初始 UI 移植时尚未连接实板；当前 R2 功能通过情况见 2026-10-10 复核。
后续回归应覆盖屏幕方向与颜色、中英文恢复提示、配网/配对、listening/thinking/speaking
与声纹通知、反复停止/启动 SDK、恢复时断电和失败重试，以及同时音视频时的动画流畅度、
HSRAM 余量、线程栈高水位和长时间运行情况。

## 2026-10-13 HTTP 请求/响应日志

扩展已声明的 `src/support/mybot_http_client.c` 诊断日志补丁，HTTP 与 HTTPS 均在
NOTICE 级别打印请求方法、URL、请求正文、响应状态码和响应正文，重定向额外打印
`Location`。GET 的空正文也明确输出。连接、发送、接收和解析失败记录各自阶段。
正文按最多320字节分段并保留 UTF-8 字符边界，包含偏移与总长度；响应头不输出认证或
Cookie 信息。当前 AOSL 日志缓冲为8 KiB，分段也使更长响应不会超过单条日志容量。
HTTP 开关、TLS 校验和重定向处理行为保持原有配置。

SDK 上游基线仍为 `7a10696ff27dd947da0558104da6c5414929ea09`；补丁及第三方声明同步记录。
排除用户原有 RTC NOTICE 日志两行本地修改后，锁定 `include/src` 摘要更新为
`86b9cb0a8b2ed1b8b1c92d638f92c3193a62f42fe43abb8076f0a56501ad7aa8`。
包含该用户本地修改的工作树摘要为
`4462218b7f9eb8fee9bbffa0c54692a7818642f5b9b72dbc2508ccbd989f7f4c`，不把这两行
未声明的本地调试调整算入可提交快照。现有云台实现及本地 HTTP 测试配置均保留。

新增日志代码的边界/生命周期复核及 `git diff --check` 通过；在原工程完成 AP/CP 编译和
打包，产物为 `build/bk7259/mybot/package/all-app.bin`。AP Flash 使用1,910,924字节，
CP Flash 使用1,295,948字节。AVDK 工作树保持干净；服务端的实际重定向目标以烧录后
输出的 `response Location` 为准。

## 2026-10-13 SDK 同步到 e7793eb

已获取远端 `main` 的 `e7793eba88f721304dc00d421706d4d788b81a43`（PR #113，SDK仍为
1.2.0），全量替换58个跟踪的 `include/src` 文件。源路径集合与该提交完全一致，
除既有 `src/support/mybot_http_client.c` HTTPS 正文日志补丁外，其余文件逐字节一致。
`SDK_REVISION` 和第三方声明同步记录，最终聚合摘要为
`fb7bd88a4d191e3b341c54d8a993b70b4a7b7ea3bd4632411f7899156015fa67`。

同步前详细 HTTP/HTTPS 分段日志及本地 RTC NOTICE 日志已经在工作区回退，实际源码摘要
为 `5e953e9aae9d359a9962d9acc9a5ea6ddd02bf4e147045ee72d2090410e3dd1f`，与残留的
`86b9cb...` 锁记录不一致。本次按实际生效的原有 HTTPS 正文日志保留补丁并修正记录，
不恢复已回退的详细日志。设备ID继续采用产品既定的 `bk_uid_get_data()` 和BK7259前缀，
以保持已部署设备身份；SDK本次新增ID文档不改变产品派生实现。

本次主要更新：绑定状态轮询和单次挂断通知移到16 KiB栈的 `service_mpq`，由控制线程
接收带网络/绑定代次的结果；最多一个轮询在途。配对码、开聊和token续期仍同步执行。
关闭时禁投并排空后台队列，AOSL引用保留至工作线程完成。现有SDK源文件列表和版本生成
规则无需修改；新增原子变量仍使用AOSL接口。

新合同要求不同HTTPS连接支持并发。BK7259每次连接独立分配TLS、CA、熵源、DRBG和DNS
请求，但原默认BIO及connect依赖AVDK的全局 `errno`。本次仅在 `bk7259_https.c` 使用
socket自身的 `SO_ERROR` 分类读写结果，直接进行有界 `select`，连接完成还校验peer。
没有串行化整个请求；CA链、主机名、SNI和TLS最低版本校验保持原约束。

验证结果：

- 新版上游GCC宿主构建及23项ctest通过，配置为video开启、cloud AEC关闭、HTTPS开启、
  Linux平台实现关闭。覆盖新后台应用/生命周期、HTTP及公共头/构建集成。
- 五组真实BK HTTPS适配源码的窄测试及ASAN/UBSAN通过，覆盖跨连接错误隔离、短I/O、
  EOF/失败、TCP peer确认、select范围、deadline及计时回绕。LeakSanitizer因ptrace环境
  不可用，ASAN运行关闭全局插桩以排除不参与窄测试的TLS初始化段；堆和栈检查保持开启。
  真实TLS握手、证书拒绝及实机双连接峰值不属于该窄测试。
- 在原工程clean后重新编译AP/CP并打包通过，产物为
  `build/bk7259/mybot/package/all-app.bin`；AP Flash使用1,911,012字节（40.68%），
  CP Flash使用1,295,948字节（89.88%）。额外16 KiB后台栈和双连接握手堆峰值需实物测量。
- 已阅读所选提交的完整 `PORTING.zh-CN.md`、`EMBEDDED.zh-CN.md` 及新版HTTPS公共合同，
  核对当前MbedTLS/lwIP/RTOS实现。AOSL摘要仍为 `2e9fc3fa...`，RTSA头文件和库哈希校验
  在构建中通过；AVDK基线不变且工作树干净。
- 最终ARM对象不再引用全局 `errno` 或默认MbedTLS send/recv/poll，仅使用lwIP的
  per-socket错误/peer查询和select。`git diff --check` 通过。旧boundary checker已重跑，
  仍因旧版本/LCD/配网规则以及把主机测试包含平台源码判为SDK依赖而失败，
  未计为通过。

本次基于当时的产品工作区完成，已有云台等
硬件修改保留，未自动提交或更新父仓库gitlink，目标AOSL未修改。

## 2026-10-14 EB521 UART2 接线适配

AP 的 `projects/mybot/ap/config/bk7259_ap/usr_gpio_cfg.h` 配置舵机 UART2，GPIO2 为 TX、
GPIO3 为 RX，两脚均关闭内部上下拉。串口为115200、8N1、无流控、无DMA。
Kconfig选择UART2，初始化、收发、IRQ回调和销毁均使用UART2及其GPIO功能枚举；
正式启动日志为 `UART2 ready: GPIO2 TX/GPIO3 RX, 115200 8N1`。

四个MCP动作、完成确认、动作结束及挂断回中继续使用现有逻辑。原工程clean后完成AP/CP
编译和打包，固件为 `build/bk7259/mybot/package/all-app.bin`。生产主机测试普通及
ASAN/UBSAN均通过6组，生成配置确认 `CONFIG_UART2=1`；实际云台动作待烧录验证。
AVDK及SDK/AOSL快照未修改。

## 2026-10-14 UART2 手动命令控制

AP命令 `ap_cmd gimbal_cmd` 每次只投递一条固定命令，按水平左转30°、俯仰下转10°、
俯仰上转20°、水平相对右转10°依次循环。每次受理推进一次，忙时不推进；
TX/RX由原UART2工作线程处理，正式日志打印完整发送帧、原始接收字节和CRC校验结果。
应答等待最多1秒，失败不重传、不退出工作线程，下一次显式调用仍可发送下一条。

首次手动调用进入独占手动控制，暂停自动查询、回中和MCP动作至平台重启或重新准备。
已有普通运动在途、会话中或已有命令待处理时拒绝调用。启动中的状态查询有界结束后
切换，不要求先完成状态查询初始化。关闭会取消未发送命令，并复用原资源释放与失败保留规则。
自动定时发送逻辑、测试配置和相应用例已删除。

原工程clean后完成AP/CP编译和打包，产物为 `build/bk7259/mybot/package/all-app.bin`；
最终ELF确认CLI注册和单条投递接口已链接，旧定时测试符号及配置无残留。
原6组生产回归及4组手动命令回归，在普通与ASAN/UBSAN环境均通过。AVDK及SDK/AOSL
快照未修改；新的CLI交互待设备烧录验证。

## 2026-10-15 CLI 执行完整表情动作

新增 `ap_cmd gimbal_scene [happy|sad|angry|surprised]`，每次受理一个完整表情动作。
不带参数时依次循环，有参数时指定表情；忙或不可用不推进游标。原单帧 `gimbal_cmd` 保留。
两个入口共用UART2工作线程及手动会话，不增加线程或定时发送，也不改变四套既有轨迹。

完整动作开始前读取新E3，检查归零完成、两轴停止及角度范围（允许0.4°量化误差），
再执行原 `play_scene()`，包含初始回中、表情轨迹及结束回中。状态缺失、无效或未就绪
会打印 `CLI scene not started`，不发送运动帧；00应答仍只表示接受，完成继续由原状态
确认逻辑验证。与原raw CLI一样，手动会话中MCP动作保持暂停至平台重启/重新准备。

已有运动、单帧或完整动作在排队/运行时，另一CLI请求返回忙。关闭从停止接受新工作
开始禁止新61/64，只允许63确认当前在途段，随后取消余下轨迹并按原规则释放资源；
关闭等待失败保留资源供重试。正式日志记录排队、姿态同步、完成、取消或失败。

普通及ASAN/UBSAN均通过14组native动作/生命周期测试与CLI注册、参数、游标测试；
四套完整动作、raw后真实姿态同步、异常状态不发61、互斥及关闭窗口均已覆盖。
原工程AP/CP编译打包通过，产物为 `build/bk7259/mybot/package/all-app.bin`；
AP Flash为1,915,964字节，RAM为157,516字节。AVDK和产品工程基线不变，修改留在工作区。
本次核对选定SDK的PORTING/EMBEDDED契约及BK UART/GPIO接口，SDK/AOSL内容摘要仍匹配
锁定记录，未修改依赖。现有旧边界脚本锁定旧版本/旧板配置，不能作为本R2工作树的通过证明。
设备尚未反馈有效E3数据，完整动作效果需用户烧录验证；未将原单帧E1/00成功视为动作完成。

## 2026-10-15 GPIO48 电源保持配置

AP的GPIO默认初始化表首项增加GPIO48/HOLD，采用R2相同的GPIO输出、上拉、初始化启用配置。
在 `bk_init` 的GPIO驱动初始化阶段实际输出高电平，早于产品任务及LCD初始化。
最终AP ELF确认启动表由18条增至19条，首项GPIO48字段与R2一致；原工程AP/CP编译及
打包通过，固件为 `build/bk7259/mybot/package/all-app.bin`，AVDK未修改。

用户已确认R2参考固件也不能电池冷启动。本次仅补齐上电后的保持配置，待验证USB启动后
拔掉USB是否仍能由电池维持运行；没有将这一配置视为解决无电源按键硬件的冷启动问题。

## 2026-10-15 云台应答等待与CLI动作日志

用户反馈happy仅移动少量后停止；两次有效E3查询从同步日志至动作开始约242/225 ms，
接近原250 ms查询上限。旧运动等待也会在第一个250 ms空等待后判ACK超时。
现将E3查询与运动ACK的独立应答期限设为1000 ms，250 ms仅为运动接收等待分片。
运动总期限包含预计运动时间、ACK和状态应答预算，内部查询受剩余预算限制。
仍只保留一个运动在途，未完成不推进，不重发可能已执行的运动。

CLI完整动作逐事务打印TX/RX、丢弃的旧输入、目标/反馈角度、速度、耗时与失败采样点；
原始RX每事务最多128字节。故障恢复的64/E4与63/E3也保留收发，便于区分最初失败和
后续停止未确认。正常MCP动作不启用逐帧日志。

普通与ASAN/UBSAN均通过14组native回归及CLI回调测试；新增300 ms ACK/E3延迟成功，
以及ACK丢失但控制器执行时仍有界失败、无重发的覆盖。原工程AP/CP编译与打包通过。
已复核选定SDK的完整PORTING/EMBEDDED文档、平台LCD合同和BK UART收发/ISR接口。
AVDK仍使用当时的固定基线且干净，产品基线不变；SDK/AOSL实际摘要仍分别为
fb7bd88a4d191e3b341c54d8a993b70b4a7b7ea3bd4632411f7899156015fa67与
2e9fc3fa5571972fd31c1ab64546477a98550b6b210e38fec181c0f4daeae64e，与锁一致。
旧boundary脚本因旧版本/旧LCD规则和native测试扫描而失败，未计为通过。
协议没有事务号，超时后才到达的同类型旧帧仍有歧义；新日志用于继续定位实机失败，
未宣称已通过真实完整动作或消除了全部UART丢包原因。未提交。

## 2026-10-15 云台最小控制周期40 ms

happy/angry/surprised及回中的规划周期从20 ms改为最小40 ms，sad保留原50 ms。
关键转折点前不足40 ms的尾段合并到前一段，保留原转折时间和动作总规划时长。
happy主体由100段降为50段，两次回中各由14段降为7段，总规划时长仍为2.56秒。
启动动作日志增加 `min_period=40ms`，收发日志及1000 ms应答期限继续保留。

14组native回归及CLI测试通过，新增遍历检查最小段长、关键点和名义时长；
原工程AP/CP编译打包通过，AP Flash为1,917,628字节，RAM为157,516字节。
已读PORTING/EMBEDDED、LCD平台合同及UART/GPIO接口仍适用；SDK/AOSL摘要与锁一致，
AVDK基线不变且干净，产品工程改动未提交。实机节奏待本版烧录验证。

## 2026-10-15 MCP表情仅更新LCD

移除LCD的MCP一次性表情到 `bk7259_gimbal_request()` 的唯一调用，表情解析仅输出LCD
枚举，继续保留四表情映射、同名表情重播及普通状态刷新不重播的语义。
手动gimbal_cmd/gimbal_scene及既有会话互斥、挂断/关闭回中生命周期保留。
Kconfig及中英文README同步说明MCP表情只渲染LCD。

原工程AP/CP编译与打包通过，14组native及CLI回归通过。AP LCD对象的未解析云台引用
仅剩 `bk7259_gimbal_set_conversation`，无表情动作请求；目标源码中该请求仅剩定义/声明。
AP Flash为1,917,292字节，RAM为157,516字节。SDK/AOSL摘要仍与锁一致；AVDK基线不变且干净，产品修改留在工作区。已读PORTING/EMBEDDED和LCD
合同继续适用，未修改SDK或依赖。实机MCP四表情与重复表情显示待烧录确认，未提交。

## 2026-10-15 SDK流程驱动LCD场景表情

`display/expression_policy.h` 中的场景映射使READY显示NEUTRAL；首次进入IN_CONVERSATION显示
SURPRISED保持300 ms，再按最新phase显示LISTENING、THINKING或NEUTRAL（SPEAKING及
暂无phase）。同次THINKING连续3秒后显示PROCESSING，普通phase/VP/运行文本刷新不重置。
仅使用现有公开LCD screen/indicator，未添加打断、任务结果、人体感应、电量或UTC推断。

MCP前景HAPPY/SAD/ANNOYED/SURPRISED保持2000/3000/1200/800 ms，新请求替换旧请求，
同名新请求重播；到期按最新基础phase恢复。保持时间从成功提交显示起，进入过渡250 ms
另计；关闭动画时额外过渡为0。LOCAL覆盖保持至清除、页面切换或更新的显式请求，
重复同LOCAL不重播。离开会话/SDK detach/恢复出厂/关闭清除前景与会话计时，旧表情不会复活。

私有expression_policy由snapshot锁保护，publish逐次记录状态边沿，UI线程在现有最多50 ms
等待循环中检查到期，即使无新SDK render也会推进。所有LVGL仍只由原UI owner调用，
不新增线程、定时器、运行期分配或GCC原子操作。view不再插入硬编码GREETING，直接绘制
owner选定表情；正式日志仅在表情/来源变化或重播时输出。MCP到舵机动作保持断开。

13组、462断言native策略回归普通及ASAN/UBSAN通过，覆盖边界时刻、未渲染phase往返、
MCP替换/重播/恢复、LOCAL保持、提交失败、动画关闭、工作流程切换与uint32时钟回绕。
真实LVGL主机验证的中文、英文、动画关闭三配置均通过严格编译及ASAN/UBSAN，验证
guide→指定脸、无中间GREETING、同ID不重启时钟、replay重启和factory优先级。
当前测试入口为 `projects/mybot/tests/expression_policy/run.sh`；
目标构建命令见[根 README](../../README.zh-CN.md#构建固件)。

原工程AP/CP编译及打包通过，产物为 `build/bk7259/mybot/package/all-app.bin`；
AP Flash 1,917,788字节、RAM 157,548字节，比本次前分别增加496字节和32字节。
已核对选定e7793eba88f721304dc00d421706d4d788b81a43的完整PORTING/EMBEDDED及LCD合同，
SDK摘要fb7bd88a4d191e3b341c54d8a993b70b4a7b7ea3bd4632411f7899156015fa67、AOSL摘要
2e9fc3fa5571972fd31c1ab64546477a98550b6b210e38fec181c0f4daeae64e仍与锁一致；
AVDK基线不变且干净，产品工作区修改未提交。
旧boundary脚本仍锁定旧版本/旧硬件规则，结果不作为本R2工作树的通过证明。
尚未进行本次固件的实机状态表情验收。

## 2026-10-15 新板ADC0直通诊断

以下ADC0/ADC1/ADC2直通及恢复原硬件条目为历史记录；当前配置见本文末尾的
三路模拟输入适配记录。诊断宏、每秒电平统计及其测试步骤不再适用。

新板反馈启动上行类似REF、随后人声能量低。本次先只验证ADC0原始采集，不作根因结论。
audio.c临时开启MYBOT_CAPTURE_ADC0_PASSTHROUGH，bitmap=0x1、chl_num=1、AEC追加参考关闭、
DMIC关闭，hw_ref_ch=ONBOARD_MIC_HW_REF_CH_NONE。ADC0保持差分、16kHz/S16、20+16 dB增益。
不初始化/注册AEC元素，链路仅mic→raw；链接后校验raw输入caps为单ADC0、ref=AUTO(0xFF)。
原三通道AEC分支保留，宏设0可恢复。播放、UI与CLI逻辑保留。

每16000个有效样本打印mean_abs/peak/削顶计数，元数据统计在io_end前执行，继续持有在途
I/O引用，退出不会访问已释放context；无新增运行期分配、无PCM日志或异步缓存。
已核对当前公开mybot_audio.h、V2 mic配置及caps传播合同；SDK/AOSL摘要与锁一致，AVDK未修改。
原工程AP/CP编译打包通过，AP Flash 1,878,460字节、RAM 157,524字节，DTCM 2,976字节；
AP音频对象无aec_v3_algorithm_init引用。固件为build/bk7259/mybot/package/all-app.bin，
实机ADC0能量及RTC声音待用户烧录验证。SDK仍为e7793eba，AVDK与产品基线不变，
本次工作区修改未提交。

## 2026-10-15 新板ADC1直通对照

诊断分支切为MYBOT_CAPTURE_ADC1_PASSTHROUGH，仅启用物理ADC1（bitmap=0x2、chl_num=1），
配置chl_cfg[1]差分、模拟20 dB、数字16 dB，未作极性补偿。仍为16kHz/S16单声道、
mic→raw→RTC，hw_ref=NONE、DMIC/AEC/双麦/NS关闭。唯一输出lane为物理ADC1，无需软件抽取。
电平统计与ADC0版一致，日志标记ADC1。

原工程AP/CP编译打包通过，AP Flash 1,878,468字节、RAM 157,524字节、DTCM 2,976字节。
音频对象无AEC初始化引用，SDK/AOSL摘要仍匹配锁，AVDK保持干净。实机在相同音量、
30cm距离和相同语句下的采集对照待验证；单独先后录音不用于判断两颗MIC的相对极性。

## 2026-10-15 新板ADC2回采直通验证

诊断分支切为MYBOT_CAPTURE_ADC2_PASSTHROUGH，只启用物理ADC2（bitmap=0x4、chl_num=1），
差分、16kHz/S16单声道，模拟及数字增益均0 dB。链路为mic→raw→RTC，AEC/双麦/NS/DMIC
继续关闭；无AEC消费者，hw_ref_ch保持NONE255，raw输入caps应为AUTO255，不产生零MIC布局。
每秒电平日志标识ADC2；原三通道AEC分支保留。

目标AP/CP编译打包通过，AP Flash 1,878,460字节、RAM 157,524字节、DTCM 2,976字节。
最终音频对象无AEC初始化引用，SDK/AOSL摘要匹配锁、AVDK保持干净，工作区未提交。
实验预期为欢迎语播放时有明显回采、人声及无播放期间电平较低；实际接线、电平/削顶及
499/1498Hz干扰是否仍存在，待用户烧录后的WAV和电平日志确认。

## 2026-10-15 恢复原硬件双麦及ADC2参考AEC

结束新板ADC0/ADC1/ADC2直通诊断，删除诊断宏分支及mean_abs/peak/clipped电平统计，
恢复此前已验证硬件的三通道差分采集：bitmap=0x7，ADC0/ADC1为模拟MIC，各使用20 dB
模拟增益及16 dB数字增益；ADC2为HT6872功放回采参考，模拟及数字增益均0 dB。
DMIC及ADC追加数字参考继续关闭（dmic_en=0、adc_cfg.aec_en=0）。

采集链恢复为onboard_mic_stream_v2→aec_v3_algorithm_v2→raw_stream，AEC使用硬件模式、
通道内REF2及双麦处理（dual_ch=1），启用传统降噪、关闭VAD，输出16 kHz单声道S16 PCM。
该配置对应原硬件完整的功放回采电路；R33/R34开路的新板不属于本次恢复目标。
仅撤销音频诊断改动，已有LCD场景、手动舵机CLI及其他工作区修改保留。

恢复后的bk7259_audio.c与当时产品基线中的版本逐字节一致
（Git blob df74bc5f9f63b4d5f66acbd019fd53a823f76712）。已核对固定SDK提交的
PORTING.zh-CN.md、EMBEDDED.zh-CN.md、公开mybot_audio.h，以及AVDK V2 MIC/AEC头文件。
SDK固定为e7793eba，include/src摘要fb7bd88a4d191e3b341c54d8a993b70b4a7b7ea3bd4632411f7899156015fa67；
AOSL基线84e08608及其已声明BK补丁摘要2e9fc3fa5571972fd31c1ab64546477a98550b6b210e38fec181c0f4daeae64e，
均与版本锁一致。AVDK基线不变且工作区干净，RTSA仍使用原BK7259 1.10.1包。

当时使用原工程make入口，AP/CP及组合打包通过。
AP Flash 1,917,788字节、RAM 157,548字节、DTCM 35,744字节；
新增的算法体积对应恢复的AEC链路。音频对象重新包含aec_v3_algorithm_init引用，
仅保留三通道/AEC启动信息，不再包含ADC直通诊断及每秒电平日志。
2026-10-15 17:33生成的package/all-app.bin为3,425,120字节。
父/子仓库git diff --check通过；构建目录仍被忽略。
旧边界检查脚本仍有与本次修改无关的版本/布局假设不匹配，不能据此宣称边界检查通过。
本次仅完成源码及构建验证，原硬件上的上行、AEC和反复启停仍待烧录回归；未提交或推送。

## 2026-10-15 三路模拟输入及追加数字参考AEC

本次将 `bk7259_audio.c` 改为DMIC关闭、bitmap=0x7、chl_num=3，
ADC0/1/2均差分输入、模拟20 dB和数字16 dB增益，adc.aec_en=3追加数字播放参考，
hw_ref=NONE255，布局为四条lane：mic[0,1,2]、ref@3。

AEC为hardware/APPEND、默认AUTO参考，adc_ch_num=3、dual_ch=2、传统NS；VAD启用、
能量阈值500、ec_only_output=1、multi_output_use_ec_out=1。采集块640字节/20 ms、两块缓冲，
SDK仍接收16 kHz单声道S16 PCM，原音量与生命周期保持，不增加ASR额外端口或UI方向回调。

配套AVDK实际仍调用aec_proc，Solution中“临时禁用AEC”的注释已过时。
旧AVDK只有双麦缓冲，不能直接套用三麦配置；本次同步4个必需音频文件：
AP aec_v3_algorithm_v2.c、对应配置头文件、modules/aec_v3_1.h与AP libaec_v3.a，包含
三麦解交织、trimic标记与SSL3 sidecar支持，非音频AVDK内容保持。这些改动当前由
`patches/bk_avdk_smp/0003-r2-trimic-aec.patch` 维护，通过构建准备入口应用。

GPIO12配置为输出下拉，作用为ADC内部比较器放电；未外露pad不代表
该内部配置无作用。GPIO34/35保留SECOND_FUNC_DISABLE+GPIO_OUTPUT，开机仍HIGH-Z，
不主动拉低；GPIO2/3 UART2和GPIO47上拉会话键继续保留。此前未烧录的旧DMIC实验未记为
验收事实。三路采集、AEC与反复启停仍待用户烧录验证。

本次构建及产物核查通过：

- 当时使用原工程make入口，AP/CP链接及组合打包成功。
- 2026-10-15 20:03生成 `build/bk7259/mybot/package/all-app.bin`，3,430,368字节，SHA256 `ad2e4e513569b0915f3b36bae924faf8af9aeef7ff32d4344c0edf6f47d2e428`。
- AP Flash 1,923,020字节、RAM 157,548字节、DTCM 35,744字节；CP Flash 1,295,948字节、RAM 192,744字节。新的VAD/EC副输出/SSL3另有运行期堆开销，静态RAM数字不代表堆峰值。
- 最终ELF反汇编确认 `aec_ver=20261014`，map及符号表确认AEC和SSL3均来自当前AVDK的配套新库。库SHA256为 `754ad74651b43cbe2cee29a0a882ee572fa70cf2b16dc4e6dc7006e695e65f90`。
- 新库 `aec_size(1000)=30272`字节，低于现有32KiB DTCM缓冲；SSL3 sidecar为8968字节。输入每20ms为2560字节（三MIC 1920字节及REF 640字节），主输出640字节。第三路输入用于SSL3分析，不将其宣称为已验证的三麦联合波束成形。
- `CONFIG_AEC_RUN_ON_M52`保持关闭；该proxy尚未适配三路MIC。本次不注册SSL3方向回调，不关联LCD或舵机。
- AVDK保留当时基线，仅上述4个音频文件有工作区改动。父仓库和Solution的 `git diff --check`通过；AVDK该检查报告的新增空白字符为导入音频源码的CRLF和行尾空白。
- 产品基线不变，其他UI/舵机工作区修改保留。父仓库gitlink未更新，修改未提交或推送，构建目录仍被忽略。
- 已核对固定SDK提交的PORTING/EMBEDDED及公开 `mybot_audio.h`、新AVDK V2 MIC/AEC与算法头文件；SDK `e7793eba`及AOSL `84e08608`基线/声明补丁的摘要仍与上文版本锁一致。
- 旧边界脚本的版本、目录与硬件假设未更新，不能宣称检查通过。本次同步AVDK音频依赖，属于已报告的依赖变更。

预期启动日志应包含 `capture layout verified: bitmap=0x7 lanes=4 mic=[0,1,2] appended_ref=3`
及底层 `aec ver:20261014`。这两项确认运行配置和算法版本，实际声音、AEC、CPU负载及
反复启停仍需设备回归；没有仅凭编译结果宣称硬件音频问题已修复。

## 2026-10-16 关闭VAD及舵机控制对比

按用户要求将当前音频AEC配置的vad_enable设为0，关闭VAD。三路差分模拟输入、
20/16 dB增益、ADC追加数字参考、四lane布局、硬件APPEND AEC、传统NS、EC输出选择、
640字节/20 ms两块缓冲及16 kHz单声道S16 SDK接口均保持；不添加运行期采集电平日志。
上文2026-10-15的VAD开启配置仅代表当时构建，不代表当前对比固件。

AP defconfig设置“# CONFIG_MYBOT_GIMBAL is not set”，不编入舵机控制模块，不启用其
GPIO2/3 UART2映射，不注册gimbal_cmd/gimbal_scene CLI，也不执行舵机初始化或回中。
舵机源代码和命令说明保留，重新启用CONFIG_MYBOT_GIMBAL=y后可用；MCP LCD表情、
GPIO47会话按键及其他产品功能保持。本次仅关闭固件控制，不能据此认定电机已物理断电。

首次增量构建仍沿用旧生成配置中的CONFIG_MYBOT_GIMBAL=y，已执行clean并重新生成配置，
避免将缓存构建误认为已关闭舵机。随后完整配置流程触发AP预编译库校验失败，定位到
上一轮替换libaec_v3.a后遗漏更新库校验清单；未通过关闭或绕过校验继续构建。

使用厂商compute_files_hash.py按当前实际AP库集合重新计算，更新
bk_avdk_smp/ap/components/bk_libs/bk7259_ap/hash/libs_hash.txt为
b60a283b606775768bf928606f8bb332。库校验值必须根据完整的实际库集合计算。
当时AVDK改动为4个音频文件及1个配套库校验清单；2026-10-15条目保留当时记录，
校验清单补齐发生在本次clean重生成配置后。

完整构建与产物核查已通过，实际音频和AEC效果仍待用户烧录验证：

- 执行工程 `clean` 后使用当时的双核构建入口重新编译；AP/CP与组合打包成功，AP库校验通过。
- 生成配置确认 `# CONFIG_MYBOT_GIMBAL is not set`，sdkconfig.h不再定义该开关；静态库和最终ELF均没有舵机worker/控制实现，`bk7259_gimbal_cli_init`反汇编只有返回0，不注册命令。
- 最终音频对象中的启动信息为 `AEC geometry: R2 appended REF trimic=1 NS=traditional VAD=0 EC_out=1`，布局仍为 `bitmap=0x7 lanes=4 mic=[0,1,2] appended_ref=3`。
- 2026-10-16 10:34生成的 `build/bk7259/mybot/package/all-app.bin` 为3,413,280字节，SHA256 `7b2176b7f37c6f3b089cae246ce7456d49b3429850b7dab28190b897e01a5221`。
- AP Flash 1,905,948字节、RAM 157,244字节、DTCM 35,744字节。此前三路输入算法与库保持，AVDK本轮只补库校验清单。
- SDK/AOSL内容摘要再次与版本锁一致；父仓库和Solution的 `git diff --check`通过。旧边界脚本仍受已记录的旧版本/硬件规则影响而未通过。
- 本轮重新核对公开音频合同、V2 AEC的VAD关闭路径及舵机Kconfig/CMake/GPIO/CLI条件；SDK、AOSL、RTSA与两个仓库基线均沿用上文版本，工作区未提交或推送。
