# agora-r2-standard-devkit

[![License](https://img.shields.io/github/license/AgoraIO-Community/agora-r2-standard-devkit)](LICENSE)
[![Upstream](https://img.shields.io/badge/upstream-agent--studio--device--firmware-0969da)](https://github.com/AgoraIO-Community/agent-studio-device-firmware)
[![Platform](https://img.shields.io/badge/platform-BK7259-2f6f4e)](https://github.com/bekencorp/bk_avdk_smp)

**简体中文 | [English](README.md)**

`agora-r2-standard-devkit` 是
[agent-studio-device-firmware](https://github.com/AgoraIO-Community/agent-studio-device-firmware)
面向 Beken BK7259 AP/CP 平台的参考固件集成。本项目面向耘趣 V1.0 R2 开发板，
提供板级初始化、网络所有权、外设适配、固件构建文件和本地产品策略；设备服务注册、
鉴权、配对、会话状态、RTC/RTM 和声纹行为仍由 SDK 负责。

本项目是参考实现，不是可直接量产的产品固件。部署前仍需完成板级适配、安全审查、
目标硬件测试和第三方许可证审查。

## 功能

- BK7259 AP/CP 启动、Flash 分区以及 SRAM/PSRAM 布局。
- 产品层负责的 STA 重连和带强制门户的 APSTA Wi-Fi 配网。
- 三路模拟输入采集，并追加播放参考信号供 AEC 使用。
- 16 kHz、单声道、有符号 16 位音频播放和持久化音量控制。
- 可选 MIPI CSI 摄像头采集、硬件 H.264 编码和 Agora RTC 视频上行。
- GC9503CV MIPI 显示、CST7XX 触摸、本地状态界面、Wi-Fi 设置和恢复出厂。
- 一个 GPIO 对话键、EasyFlash 键值存储、HTTPS 传输和派生的设备身份。
- 内嵌中英文提示音资源，不依赖 SD 卡或单独的资源分区。
- 统一构建入口，可生成中英文 AP/CP 镜像、组合烧录镜像和 OTA 包。

设备服务和云端 AI Agent 是外部依赖，不在本仓库范围内。

## 架构

| 组件 | 职责 |
| --- | --- |
| CP（`bk7259`） | 初始化 BK 运行环境并启动 AP 系统。 |
| AP（`bk7259_ap`） | 运行产品控制循环、APSTA/STA 网络、SDK 生命周期、音频、视频、显示、触摸和按键。 |
| agent-studio-device-firmware | 负责平台无关的设备服务和对话状态机。 |
| BK7259 平台适配 | 使用 BK 组件和板级资源实现 SDK 平台 `ops`。 |
| AOSL | 为 SDK 和 RTSA 提供可移植运行时。 |

产品控制循环位于 [projects/mybot/ap/ap_main.c](projects/mybot/ap/ap_main.c)。
它从芯片 UID 派生设备 ID，准备产品自有硬件，恢复或执行 Wi-Fi 配网，在 STA 获得 IPv4
后启动 SDK，并串行处理 Wi-Fi 设置、恢复出厂和 SDK 生命周期。

BK 平台源码只通过 `<mybot/platform/...>` 公共平台头文件访问 SDK，公开的 SDK
生命周期接口由 AP 应用入口统一管理。

## 仓库结构

| 路径 | 内容 |
| --- | --- |
| `bk_avdk_smp/` | 固定版本的 BK7259 AP/CP SDK 子模块 |
| `bk_solution_ai/` | 固定版本的 BK Solution 子模块 |
| `components/mybot/mybot_sdk/` | agent-studio-device-firmware 快照和 BK7259 平台适配 |
| `components/mybot/mybot_aosl/` | 内嵌 AOSL 运行时 |
| `components/mybot/mybot_rtsa/` | Agora RTSA 头文件、包信息和目标库 |
| `projects/mybot/` | AP/CP 入口、板级配置、分区、资源和测试 |
| `patches/` | 仅应用于生成构建副本的审核后厂商补丁 |
| `scripts/` | 依赖准备、源码校验、构建和测试工具 |
| `docs/` | 依赖、操作和验证文档 |

`build/`、`releases/` 等生成目录不会进入版本控制。

## 固定依赖

| 依赖 | 版本 |
| --- | --- |
| `bk_avdk_smp` | `584b947920c414230f9909375f008e7d6a2c6c6b` |
| `bk_solution_ai` | `03aadb49756a5109bfede04002b9b6820f407c3f` |
| agent-studio-device-firmware | v1.3.0，提交 `46e265a960fac48d93ae6755bdf694aeb09ab366` |
| AOSL 基线 | `84e086084ebcd0ae2455a0ce5721950c5fe2e656` |
| Agora RTSA | v1.10.1 build `1278380` |

父仓库通过 `vendor.lock.json` 固定子模块 gitlink，通过 `SDK_REVISION` 和
`AOSL_REVISION` 记录目标源码修改，并通过
`components/mybot/mybot_rtsa/PACKAGE_INFO` 记录 RTSA 包。构建时在隔离副本中应用已记录
的厂商补丁，不修改官方子模块 checkout。

补丁来源、摘要、升级规则和验证边界见
[BK 官方依赖与隔离构建](docs/OFFICIAL_DEPENDENCIES.zh-CN.md)。

## 环境要求

- 支持 Git submodule 的 Linux 环境。
- Python 3，以及 `bk_avdk_smp` 所需的构建依赖。
- `bk_avdk_smp` 选择的 BK7259 Arm GNU 工具链。
- 耘趣 V1.0 R2 开发板，或板级配置等价的硬件。
- 兼容的 SDK 设备服务和已启用 Agora 服务的部署环境。

## 获取源码

```bash
git clone --branch main \
  --recurse-submodules https://github.com/AgoraIO-Community/agora-r2-standard-devkit.git
cd agora-r2-standard-devkit
```

已有仓库可通过以下命令初始化子模块：

```bash
git submodule sync --recursive
git submodule update --init --recursive
git submodule status
```

正常开发使用父仓库固定的 gitlink，不要用 `git submodule update --remote` 跟踪移动分支。

## 构建

在仓库根目录执行：

```bash
python3 scripts/check_sources.py
python3 scripts/build_all.py
```

`check_sources.py` 校验子模块版本、已跟踪文件状态、补丁摘要、SDK 和 AOSL 内容摘要、
RTSA 文件摘要以及产品源码边界。`build_all.py` 准备隔离厂商工作区，为每种语言编译 CP
和 AP，校验生成配置和服务地址，并在全部请求的变体成功后发布产物。

常用选项：

```bash
python3 scripts/build_all.py --language zh-CN
python3 scripts/build_all.py --language en-US
python3 scripts/build_all.py --dry-run
python3 scripts/build_all.py --no-clean
```

固定版本的 RTSA 包只支持 `MYBOT_AUDIO_PTIME_MS=60`。

### 构建产物

默认输出为：

```text
releases/bk7259-zh-CN-video.bin
releases/bk7259-en-US-video.bin
```

关闭视频后文件名不包含 `-video`；关闭 HTTPS 后文件名增加 `-http`。

中文默认构建的生成产物位于：

```text
build/build-bk7259-zh-CN-video/solution/projects/mybot/build/bk7259/mybot/package/
```

| 文件 | 用途 |
| --- | --- |
| `all-app.bin` | 包含 bootloader、CP 和 AP 的组合烧录镜像 |
| `app_pack.rbl` | 包含 CP 和 AP 镜像的 OTA 包 |
| `build_summary.txt` | Flash 和 RAM 使用报告 |

独立 CP 和 AP 镜像位于同一生成构建目录下的 `bk7259/app.bin` 和
`bk7259_ap/app.bin`。

直接调用厂商构建时，先生成对应语言的工程：

```bash
python3 scripts/prepare_vendor.py --language zh-CN
```

随后执行工具打印的完整 `make` 命令。不要在未经补丁准备的厂商目录中直接构建源工程。

## 烧录与使用

使用 Beken 烧录工具写入 `all-app.bin`；具体步骤见 `bk_avdk_smp` 随附的开发板和烧录文档。

R2 典型流程：

1. 没有有效 Wi-Fi 凭据时，设备启动开放热点 `mybot-xxxx`。
2. 连接热点并打开 `http://192.168.4.1/`，提交目标网络凭据。
3. STA 获得 IPv4 后，配网门户停止，SDK 启动。
4. 未绑定设备显示并播报配对码；在设备服务控制台输入完整配对码，保留前导零。
5. 设备进入 `READY` 后，短按并松开 `GPIO47` 开始对话，再次操作结束对话。
6. 点击 **Settings** 可更换 Wi-Fi 或确认恢复出厂。

当前 R2 硬件的长按开关机由硬件处理，不在固件对话按键路径中。GPIO 和外设分配见
[usr_gpio_cfg.h](projects/mybot/ap/config/bk7259_ap/usr_gpio_cfg.h)。

## 配置

主要配置文件：

| 文件 | 用途 |
| --- | --- |
| `projects/mybot/ap/config/bk7259_ap/defconfig` | AP 功能、语言、服务传输、显示、音频和视频 |
| `projects/mybot/cp/config/bk7259/defconfig` | CP 功能 |
| `projects/mybot/partitions/bk7259/auto_partitions.csv` | Flash 布局 |
| `projects/mybot/partitions/bk7259/ram_regions.csv` | SRAM/PSRAM 布局 |

AP 主要选项：

- `CONFIG_MYBOT_LANGUAGE_ZH_CN` 或 `CONFIG_MYBOT_LANGUAGE_EN_US`：选择界面文本、提示音
  资源和服务地址，必须且只能启用一个。
- `CONFIG_MYBOT_HTTPS`：选择设备服务 REST 请求使用 HTTPS 还是 HTTP。
- `CONFIG_MYBOT_VIDEO`：启用摄像头采集和 H.264 视频上行。
- `CONFIG_MYBOT_LVGL_UI_ANIMATIONS`：启用表情过渡和光标动画。
- `CONFIG_MYBOT_KEY_GPIO`、`CONFIG_MYBOT_KEY_ACTIVE_LEVEL`：配置对话键 GPIO 和有效电平。

默认设备服务地址：

```text
https://botstation.shengwang.cn/api   # zh-CN
https://botstation.agora.io/api       # en-US
```

关闭 HTTPS 只改变 URL 协议，不改变 RTC/RTM 或本地 HTTP 配网门户。量产前需验证实际服务
端证书链和服务配置。

修改项目配置后，应重新构建且不要使用 `--no-clean`，避免 CMake 保留旧配置。

## 测试与验证

源码和主机侧检查：

```bash
python3 scripts/check_sources.py
for script in projects/mybot/tests/*/run.sh; do
  bash "$script"
done
(cd scripts && python3 -m unittest test_build_all.py test_vendor_workspace.py)
```

现有测试覆盖构建准备、配网所有权和取消、触摸与恢复出厂、表情策略、HTTPS
重试与超时、云台协议逻辑以及原生 LVGL 界面。主机测试不能替代目标硬件验证。

量产前至少完成以下验证：

- R2 冷启动以及反复启动、停止 SDK。
- Wi-Fi 配网、取消、重连和掉电恢复。
- RTC 双向音频以及反复结束和开始会话。
- 摄像头采集、H.264 编码、码率自适应和重复视频会话。
- 显示、触摸、恢复出厂和存储故障行为。
- 音频、视频、界面、HTTP 和 RTC 并发运行时，测量栈、堆、PSRAM 和时序余量。
- 实际服务的证书链、主机名、有效期和超时行为。

当前限制：

- 配网 SoftAP 为开放热点，本地设置页面使用 HTTP。
- Wi-Fi 和设备凭据保存在 EasyFlash 中，没有静态加密。
- 目标头文件区分 RTC 和 RTM token，在声明端到端 RTM 互通前需确认服务契约。
- 当前三路输入 AEC 和完整视频链路仍需目标硬件验证。
- 唤醒词未启用，EB521 云台在默认 AP 配置中关闭。
- 发布固件前必须解决第三方组件的再分发条款。

详细板级行为见 [工程说明](projects/mybot/README_CN.md)，当前验证证据和剩余真机检查见
[验证状态](projects/mybot/UI_VALIDATION.zh-CN.md)。

## 文档

- [R2 操作手册（含界面截图）](docs/R2_操作手册.pdf)
- [BK7259 工程说明](projects/mybot/README_CN.md)
- [BK 官方依赖与隔离构建](docs/OFFICIAL_DEPENDENCIES.zh-CN.md)
- [agent-studio-device-firmware](https://github.com/AgoraIO-Community/agent-studio-device-firmware)
- [SDK 移植指南](https://github.com/AgoraIO-Community/agent-studio-device-firmware/blob/main/docs/PORTING.zh-CN.md)
- [SDK 嵌入式集成指南](https://github.com/AgoraIO-Community/agent-studio-device-firmware/blob/main/docs/EMBEDDED.zh-CN.md)
- [BK7259 SDK 与解决方案文档](bk_solution_ai/docs/bk7259/)

## 贡献

- 平台无关的 SDK 功能和公开 API 修改提交到 agent-studio-device-firmware。
- BK7259 板级、适配层、工程和构建修改提交到本仓库。
- 厂商修改必须保留在受版本锁约束的补丁集中。
- 每次提交需说明所用硬件、配置和实际完成的验证。
- 不要提交构建产物、凭据或 token。

## 许可证与第三方依赖

本顶层仓库的原创内容采用 [Apache License 2.0](LICENSE)。该许可证不覆盖子模块、内嵌源码
快照、字体、音频资源或预编译二进制。

组合工程包含具有独立或附加条款的组件：

- [BK AVDK 许可证](bk_avdk_smp/LICENSE)
- [BK Solution 许可证](bk_solution_ai/LICENSE)
- [AOSL 许可证及附加条款](components/mybot/mybot_aosl/aosl/LICENSE)
- [LVGL 许可证](bk_avdk_smp/ap/components/lvgl/LICENCE.txt)
- [LVGL 视图、字体和资源来源](components/mybot/mybot_sdk/platforms/bk7259/display/SOURCES.md)
- [BK7259 第三方声明](components/mybot/mybot_sdk/THIRD_PARTY_NOTICES.md)
- [SDK 第三方声明](https://github.com/AgoraIO-Community/agent-studio-device-firmware/blob/main/THIRD_PARTY_NOTICES.md)

AOSL 许可证在 Apache-2.0 之外还有附加条件，因此组合工程不能整体描述为仅采用
Apache-2.0。内嵌提示音和视觉资源也有各自的来源及许可要求。

仓库在 `components/mybot/mybot_rtsa/` 中包含 Agora RTSA v1.10.1 build `1278380`
输入。当前仓库中没有该包的独立再分发授权；发布二进制或包含它的固件前，必须取得所需的
书面授权。
