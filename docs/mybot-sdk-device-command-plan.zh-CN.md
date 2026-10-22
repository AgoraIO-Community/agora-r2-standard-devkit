# DeviceCommand v1 行为

> 本文描述当前锁定 agent-studio-device-firmware SDK 已经实现的行为。文件名为兼容旧链接而保留，
> 原文中的扩展注册、UTC 和云台命令方案没有采用，已由 Git 历史保存。

## 接收路径

DeviceCommand 通过 RTM P2P 或当前 RTC 同名频道接收。外层必须是单层
`message.user` 信封：

```json
{
  "object": "message.user",
  "data_type": "message",
  "content": "<DeviceCommand v1 JSON 字符串>"
}
```

`object`、`data_type` 和 `content` 都必须出现，`content` 必须是字符串，不能递归查找嵌套命令。
设备在执行前核对当前 RTM 发送者、
频道、会话和服务端会话代次；旧会话或来源不匹配的消息会被丢弃。

## 支持的设备命令

当前只支持两个音量 action：

| Command | Params | 行为 |
| --- | --- | --- |
| `audio.set_volume` | 整数 `level`，范围 0-100 | 设置音量 |
| `audio.adjust_volume` | 整数 `delta`，范围 -100-100 | 在当前音量上增减，结果钳制到 0-100 |

DeviceCommand 对象必须包含当前协议要求的 `type`、`schema`、`version`、
`message_id`、`command_id`、`conversation_id` 和 `command` 字段。
`issued_at` 和 `expires_at` 可以省略或提供，当前忽略时间字段，不要求 UTC 或时钟同步。

不支持的命令、错误参数类型、重复字段、错误 envelope 或无效 JSON不会改变设备状态。
`audio.set_mute`、情绪 scene 和裸命令当前不通过 RTM 执行。

## 去重

同一会话保留最近 16 个不同的 `command_id`，按 FIFO 替换：

- 重复 ID 不会再次调节音量，也不会刷新其位置。
- 新 ID 会淘汰最旧记录。
- 被淘汰的旧 ID 如果再次到达，仍可能执行。
- 会话结束时清空缓存。

## 音量执行

音量命令复用 agent-studio-device-firmware 的设备音量路径。BK7259 音量变化映射到扬声器数字增益并写入
`mybot.volume.v1`。硬件更新失败时返回失败，不记录为执行成功。

## 相关实现

- `components/mybot/mybot_sdk/src/core/mybot_device_command.c`
- `components/mybot/mybot_sdk/src/core/mybot_app.c`
- `components/mybot/mybot_sdk/src/rtc/mybot_agora_rtc.c`
- `components/mybot/mybot_sdk/platforms/bk7259/bk7259_volume.c`

## 验证边界

编码、来源校验和去重由 SDK 主机测试覆盖。真实 RTM 消息格式、云端下发生效和音量
硬件效果仍需端到端设备验证。
