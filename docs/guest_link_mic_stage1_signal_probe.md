# 连麦阶段 1 信令探针联调说明

本文档对应 `guest_link_mic` 路线的阶段 1，也就是“业务信令闭环”，不做真实媒体传输。

## 当前目标

- 验证房间成员列表同步
- 验证 `linkmic.apply / invite / accept / reject / cancel / hangup`
- 验证 `rtc.join-params` 或 `linkmic.connected` 下发后的状态推进
- 验证 `room_id / user_id / request_id / state` 贯通日志
- 验证请求超时和基础清理

当前探针仍然复用：

- `rtc_probe.exe`
- `rtc_guest_probe.exe`

但新增了 `--mode signal` 运行路径，专门用于阶段 1 业务信令联调。

## 已实现的探针行为

- `probe.register`
- `room.member-list / room.member-join / room.member-leave` 日志与目标成员检测
- 主播端自动发 `linkmic.invite`
- 嘉宾端自动发 `linkmic.apply`
- 对邀请或申请自动 `accept / reject`
- 接受后等待 `rtc.join-params` 或 `linkmic.connected`
- 请求级超时日志
- 主播端进入 active 后可选自动发 `linkmic.hangup`
- 阶段 0 的 RTC 建连模式仍然保留，默认仍是 `--mode rtc`

## 构建

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\configure_rtc_probes.ps1 -Build
```

## 邀请流联调

这个场景用于验证：

- 主播从在线成员列表中选择目标并发起邀请
- 嘉宾接受邀请
- 服务端下发 `rtc.join-params`
- 主播端在 active 后自动挂断并回收状态

建议先启动嘉宾端，再启动主播端。

嘉宾端：

```powershell
.\build-rtc\apps\rtc_guest_probe\rtc_guest_probe.exe --mode signal --action accept --signaling-url ws://192.168.3.28:8080/ws --room-id stage1 --timeout-ms 30000 --request-timeout-ms 10000
```

主播端：

```powershell
.\build-rtc\apps\rtc_probe\rtc_probe.exe --mode signal --action invite --signaling-url ws://192.168.3.28:8080/ws --room-id stage1 --timeout-ms 30000 --request-timeout-ms 10000 --auto-hangup-ms 2000
```

## 申请流联调

这个场景用于验证：

- 嘉宾主动发起 `linkmic.apply`
- 主播端接受申请
- 服务端下发 `rtc.join-params`

建议先启动主播端，再启动嘉宾端。

主播端：

```powershell
.\build-rtc\apps\rtc_probe\rtc_probe.exe --mode signal --action accept --signaling-url ws://192.168.3.28:8080/ws --room-id stage1 --timeout-ms 30000 --request-timeout-ms 10000 --auto-hangup-ms 2000
```

嘉宾端：

```powershell
.\build-rtc\apps\rtc_guest_probe\rtc_guest_probe.exe --mode signal --action apply --signaling-url ws://192.168.3.28:8080/ws --room-id stage1 --timeout-ms 30000 --request-timeout-ms 10000
```

## 常用参数

- `--mode signal`：进入阶段 1 业务信令模式
- `--action invite`：主播端检测到目标在线后自动发邀请
- `--action apply`：嘉宾端检测到主播在线后自动发申请
- `--action accept`：收到邀请或申请后自动接受
- `--action reject`：收到邀请或申请后自动拒绝
- `--action passive`：只打印日志，不自动推进业务动作
- `--request-timeout-ms`：等待接受、拒绝、`rtc.join-params` 的超时时间
- `--auto-hangup-ms`：主播端进入 active 后多久自动发 `linkmic.hangup`
- `--auto-hangup-ms -1`：只验证到 active，不自动挂断

## 预期日志关键字

- `event=probe.registered`
- `event=room.member-list`
- `event=send.linkmic.invite` 或 `event=send.linkmic.apply`
- `event=state`
- `event=recv.rtc.join-params` 或 `event=recv.linkmic.connected`
- `event=send.linkmic.hangup`

## 当前依赖

阶段 1 信令模式默认要求服务端至少支持下面这些消息：

- `room.member-list`
- `room.member-join`
- `room.member-leave`
- `linkmic.apply`
- `linkmic.invite`
- `linkmic.accept`
- `linkmic.reject`
- `linkmic.cancel`
- `linkmic.hangup`
- `linkmic.state-sync`
- `rtc.join-params` 或 `linkmic.connected`

如果服务端还没有实现 `rtc.join-params`，探针会在接受后按 `--request-timeout-ms` 明确报超时，这属于当前阶段 1 联调的有效反馈。
