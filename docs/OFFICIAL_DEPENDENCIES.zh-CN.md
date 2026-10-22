# BK 依赖与厂商补丁

**[English](OFFICIAL_DEPENDENCIES.md) | 简体中文**

## 源码角色

仓库包含两个源码子模块：

| 依赖 | 仓库 | 固定提交 |
| --- | --- | --- |
| AVDK | `https://github.com/bekencorp/bk_avdk_smp.git` | `584b947920c414230f9909375f008e7d6a2c6c6b` |
| Solution | `https://github.com/bekencorp/bk_solution_ai.git` | `03aadb49756a5109bfede04002b9b6820f407c3f` |

父仓库维护：

- `projects/mybot/`：AP/CP 工程和板级集成。
- `components/mybot/mybot_sdk/`：固定 MyBot SDK 快照和 BK7259 适配层。
- `components/mybot/mybot_aosl/`：带 BK7259 修改的 AOSL 运行时。
- `components/mybot/mybot_rtsa/`：目标 Agora RTSA 输入。
- `patches/` 与 `vendor.lock.json`：厂商修改及其摘要。

MyBot SDK 快照为 `58f0ef9c14be9f159b1dc167bf0f035983b25117`，版本 1.3.0。
源码和许可证摘要记录在 `components/mybot/mybot_sdk/SDK_REVISION`。
AOSL 基线为 `84e086084ebcd0ae2455a0ce5721950c5fe2e656`，内容摘要和 BK7259 HAL
修改记录在 `components/mybot/mybot_aosl/AOSL_REVISION`。

当前 RTSA 为 v1.10.1 build `1278380`。包信息位于
`components/mybot/mybot_rtsa/PACKAGE_INFO`，公开头文件和静态库摘要由
`mybot_sdk/CMakeLists.txt` 固定。

## 厂商补丁

补丁只应用到生成的厂商工作区：

| 补丁 | 用途 |
| --- | --- |
| `patches/bk_avdk_smp/0001-captive-portal-dns.patch` | AP/CP 强制门户 DNS 行为 |
| `patches/bk_avdk_smp/0002-r2-gc9503cv-panel.patch` | GC9503CV MIPI 屏支持 |
| `patches/bk_avdk_smp/0003-r2-trimic-aec.patch` | 三路输入 AEC 集成和配套库 |
| `patches/bk_avdk_smp/0004-r2-cst7xx-touch.patch` | CST7XX 触摸及有界生命周期 |
| `patches/bk_solution_ai/0001-mybot-camera-helper.patch` | 摄像头辅助组件、XCLK 和低帧率适配 |

补丁 SHA256 保存在 `vendor.lock.json`。AEC 算法库大小为 591224 字节，SHA256：

```text
754ad74651b43cbe2cee29a0a882ee572fa70cf2b16dc4e6dc7006e695e65f90
```

不要直接修改源码子模块。升级后确认上游已吸收某补丁时，才能从版本锁和构建元数据中移除。

## 隔离构建

统一构建会：

1. 校验两个子模块的父仓库 gitlink 和已跟踪文件状态。
2. 校验所有补丁摘要。
3. 将固定提交克隆到 `build/_vendor/<fingerprint>/`。
4. 应用已记录补丁，并复制父仓库维护的 MyBot 组件。
5. 为每种语言在独立生成工程中编译 AP/CP。
6. 校验生成配置、服务地址、镜像和 ELF。

执行：

```bash
python3 scripts/check_sources.py
python3 scripts/build_all.py
```

需要直接调用厂商构建时，先生成对应工程：

```bash
python3 scripts/prepare_vendor.py --language zh-CN
```

随后执行工具打印的完整 `make` 命令。显式 `SDK_DIR` 指向已应用补丁的生成 AVDK，
不是干净的源码子模块。

生成工作区和固件不是源码输入，不应提交。

## 更新依赖

更新 AVDK、Solution、MyBot 或 AOSL 时：

1. 选择并审查准确的源码提交。
2. 重新核对 MyBot 端口契约和目标 RTSA ABI。
3. 逐一重做或删除厂商补丁，并更新摘要。
4. 同时更新 gitlink、源码版本记录和构建元数据。
5. 从干净的生成工作区重新编译中英文 AP/CP。
6. 检查组合镜像、OTA 包、map、架构和内存使用。
7. 完成真机回归后再接受升级。

普通开发不要使用 `git submodule update --remote`。

## 许可证与验证

子模块、MyBot 快照、AOSL、Agora RTSA、LVGL、字体和音频资源分别受自身许可证或商业条款约束。
AOSL 在 Apache-2.0 之外还有附加条件；RTSA 为专有二进制，当前仓库中没有独立再分发授权。

构建成功不代表目标行为通过。Wi-Fi、音频/AEC、显示、触摸、视频、恢复出厂和掉电行为
仍需在设备上单独验证。
