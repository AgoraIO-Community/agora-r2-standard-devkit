# EB521 云台与 UART

> 默认 AP 配置关闭 `CONFIG_MYBOT_GIMBAL`。本文件记录保留实现的接线、协议和验证方式，
> 不表示产品默认启用云台。历史设计过程见 Git 记录。

## 当前状态

云台模块、UART2 映射和 CLI 注册仅在 `CONFIG_MYBOT_GIMBAL=y` 时参与构建。
LCD 表情不会自动驱动云台；MCP 情绪输入在当前 SDK 配置中也没有接入云台动作。

## 接线与串口

| 信号 | BK7259 | EB521 |
| --- | --- | --- |
| TX | GPIO2 / UART2_TX | RX |
| RX | GPIO3 / UART2_RX | TX |
| GND | GND | GND |

UART 参数为 115200、8 数据位、无校验、1 停止位、无流控。云台供电和电平仍需按实物确认。

## 协议

帧格式：

```text
03 + command + BE16(payload_length) + payload + LE16(Modbus-CRC16)
```

CRC 初值为 `FFFF`，多项式为 `A001`，覆盖帧头到 payload 末尾。单帧总长不超过
200 字节。

当前实现使用：

- `0x61`：两轴绝对角度。
- `0x63`：查询归零状态、角度、速度和运动状态。
- `0x64`：仅在通信故障时尝试一次停止，不用于正常动作抢占。
- 应答命令字为请求命令字加 `0x80`。

角度单位为 0.1°，速度为 pps。控制器每次只保留一个运动事务；收到完成帧并通过状态查询
确认后才能发送下一段。

## CLI

启用云台并完成烧录后，日志控制台支持：

```text
ap_cmd gimbal_cmd
ap_cmd gimbal_scene
ap_cmd gimbal_scene happy
ap_cmd gimbal_scene sad
ap_cmd gimbal_scene angry
ap_cmd gimbal_scene surprised
```

`gimbal_cmd` 每次发送下一条预定义 UART 命令。`gimbal_scene` 执行完整表情轨迹并回中；
不带参数时按 happy、sad、angry、surprised 顺序循环。

命令行为：

- 对话进行中、已有命令在途或资源不可用时返回 busy。
- 命令不会自动重传。
- 首次手动单帧命令会暂停自动回中，直到平台重启。
- 日志记录发送帧、接收字节、CRC 结果、目标角度和完成状态。

## 生命周期

云台由一个 UART worker 串行拥有。关闭时先禁止新动作，取消未下发轨迹，等待当前事务结束，
再释放 UART/GPIO；等待失败时保留资源供后续重试。LCD 更新和 UART 操作不在同一锁内执行。

## 测试

```bash
bash projects/mybot/tests/gimbal/run.sh
GIMBAL_TEST_SANITIZERS=1 bash projects/mybot/tests/gimbal/run.sh
```

主机测试覆盖 CRC、分片和粘包、完成确认、超时、四种轨迹、回中、CLI 游标、取消和
worker 清理。UART 电平、电机方向、寻零、物理行程和长时间运动仍需实板验证。
