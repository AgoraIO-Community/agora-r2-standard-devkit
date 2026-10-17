# BK 官方依赖与产品源码 / Official BK dependencies

## 范围与版本

`release/v4.0.1-mybot-r2` 继 `main` 之后使用 BK 官方仓库作为两个 Git 子模块的来源，
不再依赖 `junlon2006` 下的 BK fork。这次迁移只改变源码管理和构建准备方式，保留原 R2
的 LCD、三路模拟输入/AEC、按键、电源和运行逻辑，不同时升级厂商 SDK 或 MyBot SDK。

| 依赖 | 官方来源 | 固定提交 |
| --- | --- | --- |
| AVDK | <https://github.com/bekencorp/bk_avdk_smp.git> | `584b947920c414230f9909375f008e7d6a2c6c6b` |
| Solution | <https://github.com/bekencorp/bk_solution_ai.git> | `03aadb49756a5109bfede04002b9b6820f407c3f` |

以上提交是现有产品所基于的官方版本，不代表厂商最新分支头。迁移期间不同时升级厂商 SDK。
子模块 gitlink 与版本清单必须一致；构建入口负责校验，不允许静默选用其他提交。

## 源码归属

```text
mybot-bk7259/
├── bk_avdk_smp/             # 固定提交的官方子模块
├── bk_solution_ai/          # 固定提交的官方子模块
├── projects/mybot/          # 父仓库维护的产品工程
├── components/mybot/        # 父仓库维护的组件和内嵌依赖
├── vendor.lock.json         # 官方 URL、提交及补丁 SHA256
├── patches/
│   ├── bk_avdk_smp/0001-captive-portal-dns.patch
│   ├── bk_avdk_smp/0002-r2-gc9503cv-panel.patch
│   ├── bk_avdk_smp/0003-r2-trimic-aec.patch
│   └── bk_solution_ai/0001-mybot-camera-helper.patch
└── scripts/                 # 准备、校验、隔离编译与双语固件发布入口
```

`projects/mybot/` 保存 AP/CP 入口、板级配置、内存/Flash 分区、资源和工程构建文件。
`components/mybot/` 保存 MyBot SDK、BK7259 平台适配、AOSL 和 Agora RTSA；这些文件由原
Solution 产品版本 `28f9590` 的目录迁入。MyBot `include/`、`src/`、AOSL 和 RTSA 快照保持原字节内容及锁定摘要，
并非从 BK 官方重新获取或切换到上游最新版本。

迁移保留 R2 的海内外地址和 `CONFIG_MYBOT_HTTPS` 开关。TLS 仍使用原来的 ISRG Root X1 与
DigiCert Global Root G2 两张 PEM 根证书，不合入 `main` 的按地区单根证书优化；该集合未覆盖
需要 GoDaddy G2 的服务端证书链，海外 HTTPS 需要单独验证。证书与主机名校验保持开启。

以下硬件与产品设置也保持，不通过直接构建官方 `beken_robot_r2` 代替产品工程：

- GC9503CV LCD 物理 480x640、逻辑 640x480；GPIO20 使能 3.3V、GPIO29 使能 2.8V、
  GPIO27 使能摄像头外部 1.2V、GPIO42 复位、GPIO40 背光低有效。
- GPIO47 短按松手切换会话，长按 5 秒恢复出厂；默认音量、恢复流程和 SDK 生命周期不变。
- 三路差分模拟 ADC0/1/2 均为 MIC，`aec_en=3` 追加数字播放 REF，形成三路 MIC 加一路 REF；
  DMIC 关闭、每路模拟增益 20 dB 加数字增益 16 dB、`dual_ch=2`、传统降噪、VAD 关闭、
  `ec_only_output=1`，扬声器 PA 为 GPIO50。
- UART2 GPIO2/3 的舵机实现和 40 ms 最小控制周期保留，`CONFIG_MYBOT_GIMBAL` 当前关闭；
  MCP 表情只渲染 LCD。视频默认开启，RTSA、AOSL 和 MyBot 快照版本不变。

## 必要厂商补丁

| 补丁 | 作用 |
| --- | --- |
| `patches/bk_avdk_smp/0001-captive-portal-dns.patch` | 在 AP、CP 的 DHCP/DNS 实现中保留强制门户行为，手机连接配网热点后可打开配网页。 |
| `patches/bk_avdk_smp/0002-r2-gc9503cv-panel.patch` | 保留 GC9503CV MIPI 屏驱动及屏注册/构建配置，共 4 个文本文件。 |
| `patches/bk_avdk_smp/0003-r2-trimic-aec.patch` | 保留三麦 AEC wrapper、配置/算法头文件、配套算法库校验清单及 Git binary patch 形式的 `libaec_v3.a`。 |
| `patches/bk_solution_ai/0001-mybot-camera-helper.patch` | 保留 MyBot 使用的摄像头辅助组件构建裁剪、关闭保护、XCLK 和低帧率模式匹配等既有适配。 |

补丁从原 R2 依赖与上述官方基线的差异提取。它们由父仓库跟踪，应用前验证基线，
应用失败立即停止准备，不自动忽略失败 hunk，不以官方当前分支头替代固定基线。

算法库不是重新编译或换版：保留 R2 配套 `20261014` 库，591224 字节，SHA256：

```text
754ad74651b43cbe2cee29a0a882ee572fa70cf2b16dc4e6dc7006e695e65f90
```

补丁本身的 SHA256 记录在 `vendor.lock.json`。更新库时必须同步核对 wrapper、头文件与厂商
库校验清单，不能只替换 archive 或删除校验来绕过不匹配。

构建在隔离副本中应用补丁，不向官方子模块的 tracked 源码写入修改。生成目录不是新的
源码来源，不应手工编辑后提交；产品功能变更应回到 `projects/mybot/`、`components/mybot/`
或相应补丁中维护，再通过构建入口准备和验证。

## 构建准备与产物

在仓库根目录运行完整双语构建：

```bash
python3 scripts/build_all.py
```

原有 `--language zh-CN|en-US|both`、`--build-root`、`--output-root`、`--no-clean`
和 `--dry-run` 接口保留。默认重新 clean 后构建；仅在需要增量编译时使用 `--no-clean`。
仅支持 60 ms 音频打包时长；默认启用视频和 HTTPS。关闭 HTTPS 的产物增加 `-http` 后缀，
关闭视频则省略 `-video`；两个开关均由父仓库产品 AP defconfig 选择。
若依赖锁发生变化，新的厂商工作区会使用新的摘要目录，并清除对应语言的旧构建缓存，
避免缓存继续引用旧 SDK。

脚本默认生成如下目录，`<digest>` 由版本锁和补丁信息决定：

```text
build/
├── _vendor/<digest>/
│   ├── bk_avdk_smp/            # 官方基线 + DNS/LCD/三麦 AEC 补丁
│   └── bk_solution_ai/         # 官方基线 + 摄像头补丁 + 产品组件副本
├── build-bk7259-zh-CN-video/solution/projects/mybot/
└── build-bk7259-en-US-video/solution/projects/mybot/
```

产品工程和组件复制进入生成工作区，以保持厂商 CMake 的相对路径约定；每次准备都从父仓库
刷新产品输入。官方子模块有 tracked 修改、HEAD 与锁不符、补丁摘要不符或已有厂商缓存遭到
修改时，准备会报错，不直接用这些内容编译。脚本不联网，不 fetch，也不重置开发者的源码。

需要直接使用厂商 `make` 时：

```bash
python3 scripts/prepare_vendor.py --language zh-CN
```

随后执行工具打印的完整 `make` 命令；其中已经指定生成工程及对应 `SDK_DIR`。换英文时用
`--language en-US`，不手工修改官方子模块或源工程 defconfig。产品 Makefile 要求显式传入
`SDK_DIR`，不会自动回退到未经准备的官方目录。

完整双语构建全部成功后才发布 `releases/bk7259-zh-CN-video.bin` 和
`releases/bk7259-en-US-video.bin`。AP/CP、OTA 和内存报告仍在对应生成工程的
`build/bk7259/mybot/` 下；`projects/mybot/` 本身只保存产品源码。

## 获取和更新

普通开发者执行：

```bash
git pull --ff-only
git submodule sync --recursive
git submodule update --init --recursive
git submodule status
```

这里使用父仓库固定的 gitlink，不使用 `git submodule update --remote`。正常情况下官方
子模块处于 detached HEAD；这是固定提交的预期状态，不需要切回个人维护分支。
`.gitmodules` 保留 BK 官方地址，不写入本地镜像绝对路径。

需要升级 BK 版本时：

1. 选定并核实两套官方提交的兼容性，在独立工作目录中审查源码差异。
2. 更新 gitlink 和版本清单；逐项检查现有补丁是否已被上游吸收，删除已无必要的补丁或重新
   基于新版本生成，保持补丁校验信息一致。
3. 从干净生成目录重新准备并编译中英文 AP/CP，检查组合镜像、内存报告和 MyBot/AOSL/RTSA
   摘要，确认官方子模块 tracked 工作树仍然干净。
4. 回归冷启动、配网门户、配对/HTTPS、按键、音频/AEC、LCD、摄像头及反复开启/结束会话。
5. 只有上述内容一并审核后，才提交父仓库的版本锁、补丁和 gitlink。

不要将 `mcp-resource/`、本地缓存、生成厂商副本、构建目录或固件加入源码提交。

## 许可与验证边界

源码移动不改变原许可证。BK、MyBot、AOSL、RTSA、LVGL、字体和语音资源继续适用各自的
许可证或商业条款；AOSL 有附加条件，RTSA 为专有二进制，不能将整个仓库描述为统一
Apache-2.0。相关许可和来源文件随产品目录保留，见根 README 的第三方依赖章节。

构建通过不等于迁移后实机通过；本次源码管理迁移仍需在 R2 硬件上完成配网、HTTPS、
三路模拟采集/AEC、视频、GC9503CV LCD 和按键的烧录回归。历史 UI 验证记录保留在
`projects/mybot/UI_VALIDATION.md`，其时间、路径和版本应按当时上下文理解。

## 2026-10-17 R2 迁移验证

从新布局完整准备、编译并合包中英文 AP/CP 已通过。中文构建约34秒，英文约33秒，
包含厂商副本准备的整次构建约1分13秒。使用 `--build-root build/official-r2` 和独立输出目录：

```text
releases/official-r2/bk7259-zh-CN-video.bin
releases/official-r2/bk7259-en-US-video.bin
```

这次使用 `--output-root releases/official-r2` 保留原发布目录中的固件；脚本默认输出仍为
`releases/bk7259-<language>-video.bin`。时间只代表本次开发机的编译，不是设备启动或会话延时。

`python3 scripts/check_sources.py` 的70个产品源码检查通过；MyBot SDK、AOSL 和 RTSA
不可变快照与迁移前一致。官方两个子模块保持干净；生成 AVDK 的 AP/CP/tools 与原 R2
依赖 `2a60aa47b` 对应内容一致，生成摄像头辅助组件与原 Solution `28f9590` 对应内容一致。
AEC 库大小和摘要与上文相同，保留 `20261014` 版本。

20项构建准备主机测试、表情策略13组462条断言、HTTPS 5个案例及云台14组加CLI测试通过。
本次没有改变 UI 策略或外设运行逻辑，也未将历史实机结果视为本次迁移后的实机结果。
仍需用户烧录后验证。

## English

`release/v4.0.1-mybot-r2` pins the two official `bekencorp` repositories to the commits listed above. Product files
live in the parent repository under `projects/mybot` and `components/mybot`; no personal BK fork
is a build dependency. This preserves the existing R2 hardware and firmware behavior without
upgrading the vendor SDK or MyBot snapshots.

The four recorded patches preserve captive-portal DNS, GC9503CV LCD, three-microphone AEC and
camera-helper integration. The binary AEC archive remains the previous 591224-byte `20261014`
library, with its SHA256 recorded above. Preparation verifies the pins and applies patches in isolated vendor copies,
leaving the official tracked checkouts unchanged. A patch failure is an error, not permission to
silently build unpatched or use a different upstream commit.

Use `git submodule update --init --recursive` to restore the reviewed revisions. Do not follow
remote branch tips with `--remote`. An upstream upgrade must update the gitlinks, version lock,
and patch set together, then rebuild both languages and complete the board regression checks.

MyBot, AOSL, and RTSA snapshots retain their previous bytes, revision records, and applicable
licenses. Board configuration and runtime behavior are preserved, including disabled VAD/gimbal
and the previous two-root TLS store (ISRG + DigiCert, without GoDaddy G2). Both language builds and
host checks passed; a successful build does not replace R2 hardware regression testing. See the
root English README for build commands and output paths.
