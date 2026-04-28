# 连麦阶段实施文档（libdatachannel 方案）

当前选定路线：

- 当前仓库后续连麦实现，优先按本文档推进
- 当前默认架构是 `主主播中心汇聚 RTC + libdatachannel + 部署在 VM 的 Go SDP/ICE 信令 + coturn + SRS`
- [guest_link_mic_plan.md](D:/itffmpeg/av_media/online/duanshipin-live-studio/docs/guest_link_mic_plan.md:1) 保留为 `LiveKit + SRS` 备选方案，不作为当前实施主线

本文档是 [guest_link_mic_plan.md](D:/itffmpeg/av_media/online/duanshipin-live-studio/docs/guest_link_mic_plan.md:1) 之外的并行方案文档，但当前已被选定为主实施路线。

它保留“主播端本地混流，观众侧继续走 SRS 分发”的总体思路，但把主播端与连麦端之间的低时延互动层改为：

- 主主播端：`libdatachannel`
- 其他上麦端：原生客户端，优先也使用 `libdatachannel`
- 信令：部署在虚拟机上的自建 Go `HTTP + WebSocket`
- NAT 穿透：`coturn`
- 观众分发：`SRS`

现有 `LiveKit + SRS` 方案继续保留，用于：

- 后续对比评估
- 当 `libdatachannel` 路线在 RTP、编解码或维护成本上超出预期时回退参考

## 1. 方案定位

这条路线适合下面这种目标：

- 主主播端必须是 Windows 原生 C++ 程序
- 其他上麦端也同样是原生客户端
- 希望 RTC 层尽量轻量，不引入完整 `libwebrtc native C++`
- 愿意自己掌握信令、SDP、ICE 和媒体轨道边界
- 观众分发仍然继续沿用 `SRS`
- 第一版只做最多 `3` 个上麦参与者

它不适合下面这种目标：

- 想要完整会议 SDK 体验
- 想要尽量少碰 RTP/SDP/编解码细节
- 想要尽量少自建业务信令与媒体接入逻辑

## 2. 当前仓库基线

当前仓库已经具备的能力：

- Windows 摄像头采集，走 `Media Foundation`
- Windows 麦克风采集，走 `WASAPI/MMDevice`
- H.264 视频编码
- AAC 音频编码
- FLV 封装
- RTMP 推流到 `SRS`

现有主链路是：

`LiveDeviceCaptureSource -> VideoEncoder(H.264) + AudioEncoder(AAC) -> FlvMuxer -> RtmpPublisher`

当前还没有的能力：

- `libdatachannel` RTC 接入
- 自建信令层
- `coturn` 穿透链路
- 多路远端上麦流 RTP 接收、解包、解码
- 节目级混流编排层

## 3. 第一版目标

第一版只做最小可用的“小规模上麦节目流”：

- 主主播端仍然是当前 Windows 导播程序
- 其他上麦端优先也是原生客户端
- 主主播界面显示房间在线人数和在线成员列表
- 主主播可从列表中选择某个用户发起邀请
- 其他用户也可以向主主播发起连麦申请
- RTC 拓扑采用“主主播中心汇聚”，而不是纯对等 mesh
- 观众继续只看 `SRS` 上的一路节目流
- 节目流混合放在主主播本地
- 1 人时输出单画面
- 2 人时输出双画面
- 3 人时输出三画面

第一版成功标准：

- 主主播正常单人开播
- 主主播界面能看到房间在线成员列表
- 主主播可从列表中点选某个用户发起邀请
- 被邀请用户可以接受或拒绝
- 其他用户也可以主动申请上麦
- 上麦后，主主播与上麦端能互相看见、听见
- 主主播端能把当前所有上麦参与者合成一路节目流
- 1 人、2 人、3 人时节目画面布局正确
- 主主播端能把最终节目流继续推给 `SRS`
- 连麦结束后，直播不中断并回到单人节目流

## 4. 总体架构

推荐的整体拓扑如下：

```text
上麦客户端 A
  -> VM 上的 Go 业务服务(HTTP + WebSocket)
  -> libdatachannel PeerConnection
  -> STUN/TURN(coturn)

上麦客户端 B
  -> VM 上的 Go 业务服务(HTTP + WebSocket)
  -> libdatachannel PeerConnection
  -> STUN/TURN(coturn)

主主播导播台
  -> VM 上的 Go 业务服务(HTTP + WebSocket)
  -> 与每个上麦端分别建立 libdatachannel PeerConnection
  -> STUN/TURN(coturn)
  -> 本地视频合成 / 音频混合
  -> 现有 H.264/AAC/FLV/RTMP 链路
  -> SRS / CDN
  -> 观众
```

这条路线的关键边界是：

- 上麦互动：主主播与每个上麦端之间分别通过 `libdatachannel` 建 RTC
- 业务控制：部署在虚拟机上的自有 Go `HTTP + WebSocket`
- NAT 穿透：`coturn`
- 观众分发：`SRS`

这意味着：

- `SRS` 不参与第一版 RTC 媒体交换
- `SRS` 只负责观众看播分发
- 业务服务需要自己转发 `SDP` 和 `ICE`
- 第一版不采用客户端之间全互连的 mesh
- 主主播端是 RTC 汇聚点，也是节目合流点

## 5. 为什么可以选 libdatachannel

这条路线的优势：

- 比直接上 `libwebrtc native C++` 更轻量
- 普通 `CMake` 方式更容易纳入当前工程体系
- 对当前“自有采集链路 + 自有推流链路”的项目更贴合
- 主主播端和其他上麦端都使用同一套 RTC 栈，互通边界更可控
- 2 人和 3 人时都不需要推翻架构
- 节目画面始终集中在主主播端控制

这条路线的代价：

- 你要自己处理信令
- 你要自己处理编解码协同
- 你要自己处理 RTP 打包和收包后的解包
- 多路远端媒体进入主主播端后，还要自行解码才能本地混流
- 主主播端会承担更多 CPU、带宽和同步压力

要特别记住的一点：

- `libdatachannel` 的 Track 默认是按 `RTP packet` 收发
- 它不是“给一帧原始图像就自动替你完成完整媒体管线”的那类 SDK

这决定了第一版设计必须非常明确地区分：

- 发送侧编码与 RTP 打包
- 接收侧 RTP 解包与解码
- 节目混流与最终 RTMP 输出

## 6. 编解码策略建议

这一条是 `libdatachannel` 方案和 `LiveKit` 方案最大的不同。

### 6.1 主主播发送给其他上麦端

建议：

- 视频：继续复用当前 `H.264` 编码器
- 音频：新增 `Opus` 编码器，不复用当前 `AAC` 作为 RTC 音频

原因：

- 两端都是原生客户端时，理论上编码选择更自由
- 但第一版继续固定走 `H.264 + Opus` 最稳，便于后续排障、扩展和与标准 WebRTC 生态保持一致
- 当前仓库里的 `AAC` 适合 `RTMP/FLV`
- WebRTC 音频第一版不建议继续走 `AAC`

因此第一版主播音视频会分成两路编码用途：

- RTC 路：`H.264 + Opus`
- 节目输出路：`H.264 + AAC`

### 6.2 其他上麦端发给主主播

其他上麦端建议固定发送：

- 视频：`H.264`
- 音频：`Opus`

主播端收到后需要：

- 视频：RTP 解包 -> H.264 NALU -> 解码为原始视频帧
- 音频：RTP 解包 -> Opus 帧 -> 解码为 PCM

只有解码成原始帧后，才能进入主主播端的：

- 上麦端预览
- 视频合成
- 音频混合

## 7. 主主播端媒体链路建议

建议的主主播端目标链路如下：

```text
主主播本地采集
  -> 本地预览
  -> H.264 encoder -> RTP packetizer -> 分别送往每个远端上麦端
  -> H.264/AAC 节目输出链路

主主播本地音频采集
  -> Opus encoder -> RTP packetizer -> 分别送往每个远端上麦端
  -> AAC 节目输出链路

远端上麦端 A / B inbound
  -> RTP depacketizer
  -> H.264 / Opus decoder
  -> 上麦端预览
  -> 节目混流输入

节目混流输出
  -> VideoEncoder / AudioEncoder
  -> FlvMuxer
  -> RtmpPublisher
  -> SRS
```

这个模型里，现有仓库最容易复用的是：

- `capture/LiveDeviceCaptureSource`
- `encode/VideoEncoder`
- `mux/FlvMuxer`
- `publish/RtmpPublisher`

需要新补的部分会明显更多：

- RTC 音频 `Opus` 编码
- H.264/Opus RTP 打包
- H.264/Opus RTP 解包
- H.264/Opus 解码
- 多路参与者布局与节目时钟同步

## 8. 业务信令边界

业务服务建议继续放在现有平台后端工程 `../duanshipin-pingtai/server` 中，由部署在虚拟机上的 Go 服务承担，当前仓库的 `server/` 目录只保留说明和协议约定。职责包括：

- 开播鉴权
- 直播间创建与状态维护
- 房间在线成员列表同步
- 上麦申请、邀请、接受、拒绝、结束
- 生成和转发 RTC 信令
- 为双方下发 `STUN/TURN` 配置

结合现有平台服务端现状，连麦应继续复用同一个 Go 后端的基础设施，而不是再单独维护一套本地 Node 信令服务。当前已经存在并值得复用的方向包括：

- Go API 入口与环境变量配置
- MySQL 连接与 schema 管理
- SSE 事件推送能力
- 现有媒体平台的鉴权、存储和业务模型
- Redis 可选接入，用于在线状态缓存、邀请超时和多实例同步

第一版最小信令事件建议包括：

- `room.member-list`
- `room.member-join`
- `room.member-leave`
- `linkmic.apply`
- `linkmic.invite`
- `linkmic.accept`
- `linkmic.reject`
- `linkmic.cancel`
- `rtc.offer`
- `rtc.answer`
- `rtc.ice-candidate`
- `linkmic.connected`
- `linkmic.hangup`
- `linkmic.kick`
- `linkmic.state-sync`

说明：

- `linkmic.*` 负责业务状态
- `rtc.*` 负责媒体建连所需的最小信令
- 第一版不做更复杂的重协商策略

## 9. 房间成员列表与交互约束

主播端需要：

- 展示房间在线人数
- 展示在线成员列表
- 显示每个成员是否可邀请

第一版成员列表建议字段：

- `user_id`
- 昵称
- 头像
- 在线状态
- 是否正在连麦
- 最近活跃时间
- 当前角色

第一版交互约束：

- 第一版同时最多允许 `3` 个上麦参与者，其中包含主主播
- 主主播端同一时刻只允许一个待响应邀请
- 已经处于上麦中的用户在列表里显示为不可重复邀请
- 被邀请用户收到邀请后，只展示 `接受 / 拒绝`
- 节目布局由主主播端统一决定

## 10. 代码边界建议

建议在现有工程基础上新增这些模块：

- `src/linkmic/`
  - `LinkMicSignalingClient`
  - `RoomMemberDirectory`
  - `LinkMicRoomState`
  - `LinkMicSession`
  - `ActiveParticipantRoster`
- `src/rtc/`
  - `RtcPeerConnection`
  - `RtcSdpBridge`
  - `RtcIceBridge`
  - `RtcTransportConfig`
- `src/rtc/packet/`
  - `H264RtpPacketizer`
  - `OpusRtpPacketizer`
  - `H264RtpDepacketizer`
  - `OpusRtpDepacketizer`
- `src/decode/`
  - `VideoDecoder`
  - `AudioDecoder`
- `src/encode/`
  - `OpusEncoder`
- `src/mix/`
  - `ProgramMixer`
  - `ProgramVideoMixer`
  - `ProgramAudioMixer`
  - `ProgramLayoutPolicy`

现有模块继续复用：

- `capture/LiveDeviceCaptureSource`
- `encode/VideoEncoder`
- `encode/AudioEncoder`
- `mux/FlvMuxer`
- `publish/RtmpPublisher`

## 11. 分阶段实施计划

阶段 0 的具体执行清单见：

- [guest_link_mic_stage0_checklist.md](D:/itffmpeg/av_media/online/duanshipin-live-studio/docs/guest_link_mic_stage0_checklist.md:1)

### 阶段 0：方案冻结与依赖验证

目标：

- 固定第一版采用“主主播中心汇聚 RTC + libdatachannel + 自建信令 + coturn + SRS”
- 固定视频编码优先走 `H.264`
- 固定 RTC 音频优先走 `Opus`
- 固定上麦端优先也为原生客户端
- 固定节目布局先支持 `1 / 2 / 3` 人

这一阶段只做：

- 补齐本文档
- 验证 `libdatachannel` 在 Windows 本地可编译、可链接
- 验证主主播端与单个上麦端之间的基础互通
- 验证 `coturn` 的 STUN/TURN 可用

必须输出：

- 本文档
- `libdatachannel` 集成结论
- `coturn` 部署说明
- RTC 编解码策略结论

验收标准：

- 能明确回答“主播端 RTC 用什么库接”
- 能明确回答“RTC 音频是否继续用 AAC”
- 能明确回答“RTC 信令由谁转发”

### 阶段 1：业务信令闭环

目标：

- 不接媒体，先把房间成员列表、上麦申请、邀请、接受、拒绝跑通

当前仓库里的阶段 1 探针联调说明见：

- [guest_link_mic_stage1_signal_probe.md](D:/itffmpeg/av_media/online/duanshipin-live-studio/docs/guest_link_mic_stage1_signal_probe.md:1)

只做：

- 开播后创建房间
- 主主播端展示在线人数与在线成员列表
- 其他用户申请上麦
- 主主播点选成员列表中的某个用户发起邀请
- 对端接受或拒绝
- 状态同步和超时处理

不做：

- 真正的 SDP/ICE 交换
- 真正的媒体发送
- 混流

验收标准：

- 主主播端能看到在线列表
- 主主播能从列表中选人邀请
- 被邀请者能接受或拒绝
- 结束上麦后房间恢复到单人直播状态

### 阶段 2：RTC DataChannel / 建连探针

目标：

- 先验证主主播端与单个上麦端之间的最小 RTC 建连能力

只做：

- `offer / answer`
- `ICE candidate`
- `DataChannel`
- 连接状态日志

不做：

- 音视频轨道
- 混流

建议：

- 单独做一个 `rtc_probe` 小程序
- 再单独做一个 `rtc_guest_probe` 或测试客户端
- 不要一上来就直接塞进主导播程序

验收标准：

- 主主播端探针能与上麦客户端建立连接
- `coturn` 可选情况下仍能连通
- 断开和重连日志清晰

### 阶段 3：主主播到单个上麦端单向视频

目标：

- 先打通最小视频链路

只做：

- 主主播本地采集
- H.264 编码
- H.264 RTP 打包
- `libdatachannel` 发视频轨道到上麦客户端

不做：

- 远端视频回传
- 音频
- 混流

验收标准：

- 上麦客户端能看到主主播画面
- 主主播端断流和恢复行为清晰

### 阶段 4：双向视频与 2 人节目布局

目标：

- 让主主播和单个上麦端互相看到对方，并形成 2 人节目画面

只做：

- 上麦客户端向主主播发送视频
- 主主播端接收 RTP 视频包
- 主主播端做 H.264 解包和解码
- 主主播端显示上麦端预览
- 主主播端输出 2 人节目布局

不做：

- 音频
- 最终节目混流

验收标准：

- 上麦端能看到主主播
- 主主播能看到上麦端
- 2 人节目布局正确
- 一次正常挂断后能再次上麦

### 阶段 5：双向音频与 3 人扩展

目标：

- 让参与者互相听见，并支持最多 3 人节目布局

只做：

- 主主播端新增 `Opus` 编码
- 上麦客户端发送 `Opus`
- 主主播端 `Opus` RTP 解包与解码
- 双方基础静音控制
- 第 3 个上麦端接入
- 3 人节目布局

不做：

- 自动混音策略
- 复杂回声消除

建议：

- 第一版默认要求主主播佩戴耳机

验收标准：

- 上麦端能听到主主播
- 主主播能听到上麦端
- 不出现明显回声闭环
- 3 人节目布局正确

### 阶段 6：主播端本地混流与节目输出

目标：

- 让观众看到真正的 `1 / 2 / 3` 人节目流

只做：

- 所有上麦参与者远端视频进入本地节目画布
- 所有上麦参与者远端音频进入本地节目混音
- 最终节目流继续走现有 `RTMP -> SRS`

不做：

- 云端混流
- 多参与者布局
- 观众端 RTC 看播

验收标准：

- 上麦人数变化时，观众看到的节目画面正确切换
- 观众始终只看一路节目流
- 所有上麦结束后，直播不中断并回到单人节目流

### 阶段 7：稳定性与运维补强

目标：

- 让第一版具备持续联调能力

只做：

- 信令超时处理
- `ICE` 失败处理
- TURN 失败日志
- RTP 丢包与关键状态日志
- 断线清理和可重复连麦

建议补齐的指标：

- 信令往返耗时
- `ICE` 完成耗时
- RTC 首帧耗时
- 首个远端视频解码耗时
- 节目流首帧耗时

## 12. 这条路线最大的风险

和 `LiveKit` 方案相比，`libdatachannel` 方案的主要风险不是“能不能建连”，而是下面这些工程细节：

- 你要自己维护 `offer / answer / ICE`
- 你要自己决定 RTC 编码策略
- 你要自己做 `RTP packetize / depacketize`
- 你要自己接收远端媒体并解码成可混流的原始帧

这意味着它更轻，但也更“靠自己”。

还要明确一点：

- 当前架构适合最多 `3` 个上麦参与者的小规模节目场景
- 这里不是 mesh，而是“主主播中心汇聚”，所以 `2` 人和 `3` 人都可以在同一架构内扩展
- 如果后续目标超过 `3` 人，或者希望每个参与者都稳定看到所有其他人，再重新评估 `SFU`，例如回退到 `LiveKit` 备选方案

所以第一版最重要的策略是：

- 先 DataChannel 建连
- 再单向视频
- 再双向视频
- 再双向音频
- 最后再接节目混流

不要一上来就尝试完整双向音视频加本地混流。

## 13. 与 LiveKit 方案的关系

当前仓库中建议同时保留两份方案文档：

- `LiveKit + SRS` 方案
- `libdatachannel + coturn + SRS` 方案

建议的理解方式是：

- `LiveKit` 方案更重，但集成更省心
- `libdatachannel` 方案更轻，但自己要做的事更多

如果后续发现下面这些点成为主要瓶颈：

- RTP/编解码接入太重
- Windows 主播端维护成本过高
- 多人 RTC 拓展成本太高

那就优先回退到 `LiveKit` 文档对应的实施路线。

## 14. 建议的近期执行顺序

建议接下来按下面顺序推进：

1. 先完成阶段 0，验证 `libdatachannel` 本地编译与客户端互通。
2. 再完成阶段 1，先把在线成员列表和邀请闭环做出来。
3. 然后做阶段 2 的 `rtc_probe`，只验证主主播与单个上麦端的信令与建连。
4. 再做单向视频、双向视频、双向音频和 3 人扩展。
5. 最后把多路上麦流接进本地节目混流和 `SRS` 分发链路。

这条路线的成败关键，不在 `SRS`，而在“你是否愿意自己接住主主播中心汇聚、多路 RTP、编解码、布局和自建信令这些工作”。
