# 嘉宾连麦阶段实施文档

状态说明：

- 本文档当前保留为 `LiveKit + SRS` 备选方案
- 当前主实施路线请以 [guest_link_mic_plan_libdatachannel.md](D:/itffmpeg/av_media/online/duanshipin-live-studio/docs/guest_link_mic_plan_libdatachannel.md:1) 为准

本文档面向当前 `duanshipin-live-studio` 仓库，目标是把“1 主播 + 1 嘉宾”的连麦能力拆成可分阶段落地的工程方案。第一版不追求多人房、云端导播或复杂运营能力，只先把双向可发起的嘉宾连麦链路跑通，并尽量复用当前已经稳定的本地采集、编码、封装和 RTMP 推流能力。

本方案现已固定为：

- 主播与嘉宾之间的低时延互动使用 `LiveKit`
- 主播端使用 `LiveKit C++ SDK`
- 嘉宾端优先使用网页端 `LiveKit` SDK
- 观众侧继续使用 `SRS` 做 RTMP / HTTP-FLV / HLS 分发
- 主播端本地负责节目混流，再把最终节目流推给 `SRS`

## 1. 当前基线

当前仓库已经具备的主链路是：

`LiveDeviceCaptureSource -> VideoEncoder(H.264) + AudioEncoder(AAC) -> FlvMuxer -> RtmpPublisher`

当前已经稳定的能力包括：

- Windows 摄像头采集，走 `Media Foundation`
- Windows 麦克风采集，走 `WASAPI/MMDevice`
- H.264 视频编码
- AAC 音频编码
- FLV 音视频封装
- RTMP 推流到 SRS
- 基础预览、状态和性能日志

当前还没有的能力包括：

- `LiveKit` RTC 接入
- 连麦业务状态机
- 房间与申请信令
- 远端嘉宾音视频接入
- 混流前的节目编排层

## 2. 第一版目标

第一版只做最小可用的“嘉宾连麦”：

- 只有 `1` 个主播
- 只有 `1` 个嘉宾
- 主播使用当前 Windows 导播程序
- 嘉宾优先使用网页端加入房间，并通过 `LiveKit` 参与 RTC
- 主播界面显示当前房间在线人数与在线成员列表
- 主播可以从在线成员列表里选择一个人发起连麦邀请
- 主播和嘉宾之间的互动链路使用 `LiveKit RTC`
- 观众继续观看最终 `RTMP` 节目流
- 节目流合成先放在主播本地完成

第一版成功标准：

- 主播能正常单人开播
- 主播界面能看到当前房间在线人数和在线成员列表
- 嘉宾能向主播发起连麦申请
- 主播也能向嘉宾主动发起连麦邀请
- 主播主动邀请时，必须是从房间在线成员列表中选择目标用户
- 任一方发起后，对端都能接受或拒绝
- 接受后，主播与嘉宾能互相听见、看见
- 主播端能把“主播画面 + 嘉宾画面”合成一路节目流
- 观众端持续只看一路 RTMP 输出，不感知底层 RTC 房间细节
- 连麦结束后，节目流能平滑回到单人直播

## 3. 第一版明确不做

为了控制复杂度，下面这些内容不进入第一版：

- 多嘉宾同时连麦
- 服务端混流
- 云端导播台
- 复杂布局编辑器
- 弱网自适应策略中心
- 自动降级到其他协议
- 录制、回放、剪辑
- 美颜、滤镜、虚拟背景
- 观众端直接进入 RTC 房间观看
- 跨房 PK、多房对战

## 4. 推荐总体方案

建议把第一版拆成三层：

- 业务层：房间、在线成员列表、连麦请求、邀请、接受、拒绝、结束、踢出、状态同步
- 实时互动层：主播与嘉宾之间的 `LiveKit RTC` 音视频传输
- 节目输出层：主播本地混流后继续走现有 RTMP 推流链路，经 `SRS` 分发给观众

推荐的整体拓扑如下：

```text
嘉宾网页端
  -> 业务服务(HTTP + WebSocket)
  -> 获取 LiveKit room/token
  -> LiveKit RTC

主播导播台
  -> 业务服务(HTTP + WebSocket)
  -> 获取 LiveKit room/token
  -> 本地采集
  -> LiveKit 上行给嘉宾
  -> LiveKit 下行接收嘉宾
  -> 本地视频合成 / 音频混合
  -> 现有 H.264/AAC/FLV/RTMP 链路
  -> SRS / CDN
  -> 观众
```

这里最核心的设计决定是：

- `LiveKit RTC` 只服务于“主播和嘉宾之间的低延迟互动”
- `RTMP` 继续服务于“观众看最终节目流”
- 第一版混流放在主播本地，不放在服务端

这样做的原因是：

- 当前仓库最成熟的是本地采集和 RTMP 输出
- 1v1 连麦时，本地混流接入成本最低
- `LiveKit` 对原生 C++ 客户端接入更友好，能降低 Windows 主播端 RTC 接入难度
- 观众分发链路可以继续沿用 `SRS`
- 后续要扩展到多人房时，再评估是否迁移到服务端混流

## 5. 推荐基础设施

第一版推荐基础设施为：

- RTC 服务：`LiveKit`
- 观众分发：`SRS`
- 业务服务：自建 `HTTP + WebSocket + LiveKit token 下发`
- 嘉宾端：网页 H5 + `LiveKit` 前端 SDK
- 主播端：当前本地导播程序 + `LiveKit C++ SDK`

选择理由：

- 当前项目已经验证过 `SRS` 的 RTMP 推流
- `LiveKit C++ SDK` 官方提供预编译 SDK 集成方式，适合先做 Windows 主播端 RTC 接入
- `LiveKit` 官方文档明确支持服务端生成 token，并由客户端使用 token 连入房间
- 嘉宾先用网页端接入，验证成本最低

官方资料可作为后续实现参考：

- LiveKit C++ quickstart: `https://docs.livekit.io/transport/sdk-platforms/cpp/`
- LiveKit authentication: `https://docs.livekit.io/frontends/build/authentication/`
- LiveKit running locally: `https://docs.livekit.io/transport/self-hosting/local/`
- LiveKit Egress: `https://docs.livekit.io/server/egress/`
- SRS RTMP: `https://ossrs.io/lts/en-us/docs/v7/doc/rtmp`
- mediasoup Overview: `https://mediasoup.org/documentation/overview/`

第一版不建议直接选 `mediasoup`，原因不是它不能做 RTC，而是它本身是 `SFU`，不直接解决节目混流输出问题。对当前仓库来说，先把 `LiveKit RTC + 本地混流 + SRS 分发` 做起来更贴近现状。

## 6. 主播端目标媒体链路

主播端第一版最终会同时维护三条链路：

- 本地预览链路
- `LiveKit RTC` 互动链路
- RTMP 节目输出链路

建议目标形态如下：

```text
本地主播采集
  -> 本地预览
  -> LiveKit publish 给嘉宾
  -> 节目混流输入

远端嘉宾 LiveKit subscribe
  -> 嘉宾预览
  -> 节目混流输入

节目混流输出
  -> VideoEncoder / AudioEncoder
  -> FlvMuxer
  -> RtmpPublisher
```

要特别注意：

- `LiveKit` 发给嘉宾的主播画面，不一定等于最终节目画面
- 第一版里，嘉宾看到主播原始机位即可，不必回看最终混流
- RTMP 只推一路最终节目流

## 7. 业务信令边界

连麦业务服务建议放在 `server/` 对应的后端工程里，职责边界如下：

- 开播鉴权
- 直播间创建与状态维护
- 记录当前主播是否在线
- 记录当前房间在线成员和在线人数
- 向主播端同步可邀请的房间成员列表
- 记录当前是否存在活跃连麦
- 处理连麦请求、邀请与超时
- 为主播和嘉宾生成并下发 `LiveKit room/token`
- 通过 WebSocket 向主播端和嘉宾端同步状态

第一版最小信令事件建议包括：

- `room.member-list`
- `room.member-join`
- `room.member-leave`
- `linkmic.apply`
- `linkmic.invite`
- `linkmic.accept`
- `linkmic.reject`
- `linkmic.cancel`
- `rtc.join-params`
- `linkmic.connected`
- `linkmic.hangup`
- `linkmic.kick`
- `linkmic.state-sync`

说明：

- `room.member-list` 用于主播端初始化在线成员列表和在线人数
- `room.member-join / room.member-leave` 用于增量更新房间成员变化
- `linkmic.apply` 表示嘉宾向主播发起请求
- `linkmic.invite` 表示主播从房间在线成员列表中选中一个目标用户并发起邀请
- `rtc.join-params` 表示业务服务在连麦被接受后，下发 `livekit_url / room_name / token / participant_role`
- `linkmic.accept / reject / cancel / hangup` 对两种发起方向都适用

这些事件先只解决一件事：让双方能稳定建连、进入连麦状态、正常结束。

第一版成员列表约束：

- 主播界面只展示当前房间在线成员的简化信息
- 第一版列表字段建议只包含 `user_id`、昵称、头像、在线状态、是否已在连麦中
- 当房间已经存在活跃连麦时，其余成员在主播端列表中应显示为不可邀请
- 第一版不做复杂分页、搜索、排序推荐

## 8. 第一版状态机建议

主播端最小状态机：

- `idle`
- `live-only`
- `guest-applying`
- `host-inviting`
- `linkmic-connecting`
- `linkmic-active`
- `linkmic-ending`
- `failed`

嘉宾端最小状态机：

- `idle`
- `applying`
- `guest-invited`
- `waiting-host`
- `rtc-connecting`
- `rtc-active`
- `ending`
- `failed`

业务层要保证同一直播间第一版同时最多只有一个活跃嘉宾。

状态说明：

- `guest-applying` 表示主播收到了嘉宾申请，等待主播决策
- `host-inviting` 表示主播已经向嘉宾发起邀请，等待嘉宾决策
- `waiting-host` 表示嘉宾已经发起申请，等待主播决策
- `guest-invited` 表示嘉宾收到了主播邀请，等待嘉宾决策

交互约束：

- 主播只能对房间在线成员列表中的单个用户发起邀请
- 同一时刻主播端只能存在一个待响应邀请
- 嘉宾收到主播邀请后，只展示 `接受` 或 `拒绝`
- 嘉宾接受前，不进入 RTC 建连
- 连麦被接受前，业务服务不下发 `LiveKit` 加房参数

## 9. 代码边界建议

结合当前仓库，建议后续按下面的模块边界扩展，而不是把 RTC、混流和业务状态都塞进现有 RTMP 发送逻辑里。

建议新增或扩展的模块如下：

- `src/linkmic/`
  - `LinkMicSignalingClient`
  - `RoomMemberDirectory`
  - `LinkMicRoomState`
  - `LinkMicSession`
- `src/rtc/`
  - `LiveKitRoomClient`
  - `LiveKitTrackPublisher`
  - `LiveKitTrackSubscriber`
  - `LiveKitTokenBundle`
- `src/mix/`
  - `ProgramVideoMixer`
  - `ProgramAudioMixer`
  - `ProgramLayoutPolicy`
- `src/session/`
  - 在 `LiveSessionController` 之外新增或拆出节目级会话编排层

现有模块建议继续复用：

- `capture/LiveDeviceCaptureSource`
- `encode/VideoEncoder`
- `encode/AudioEncoder`
- `mux/FlvMuxer`
- `publish/RtmpPublisher`

核心思路是：

- 采集层继续负责本地主播原始数据
- RTC 层只负责主播与嘉宾通过 `LiveKit` 进行实时互动
- 混流层负责产出统一节目帧
- 现有编码封装推流层继续负责最终节目输出到 `SRS`

## 10. 分阶段实施计划

下面的阶段按“先把双向可发起的嘉宾连麦做出来，再打磨稳定性”的顺序推进。

### 阶段 0：方案冻结与依赖验证

目标：

- 固定第一版只做 `1 主播 + 1 嘉宾`
- 固定第一版采用“本地混流 + RTMP 推给 SRS + LiveKit 做 RTC”
- 固定嘉宾端优先为网页端
- 固定主播端 RTC 方案为 `LiveKit C++ SDK`

这一阶段只做：

- 补齐本文档
- 确认 `LiveKit` 和 `SRS` 的职责边界
- 确认本地开发部署方式
- 验证 `LiveKit` 本地启动、token 下发和基础入房联通

这一阶段不做：

- 正式 UI
- 正式混流
- 正式观众端联动

必须输出：

- 本文档
- `LiveKit C++ SDK` 集成结论
- `LiveKit + SRS` 本地联调说明
- token 下发接口说明
- 基础网络拓扑图

验收标准：

- 团队对第一版边界没有分歧
- 能明确回答“谁负责 RTC、谁负责分发、token 由谁签发”

说明：

这一阶段最关键的风险不是 `SRS`，而是“Windows 原生导播程序如何稳定接入 `LiveKit C++ SDK`，并与现有本地采集链路并存”。如果这件事没有提前定下来，后续所有阶段都会被阻塞。

### 阶段 1：业务信令闭环

目标：

- 不接媒体，先把连麦业务流程跑通

只做：

- 开播后创建房间
- 主播端订阅房间在线成员列表
- 主播端展示在线人数和在线成员列表
- 嘉宾申请连麦
- 主播从在线成员列表中选择用户并主动邀请
- 对端接受或拒绝
- 连麦接受后，业务服务下发 `rtc.join-params`
- 状态同步和超时处理
- 结束连麦和清理状态

不做：

- 真实音视频轨道传输
- 混流

建议产出：

- 房间在线成员列表接口或推送消息
- 连麦请求接口
- `LiveKit room/token` 下发接口
- 主播端消息订阅
- 嘉宾端申请页或测试页
- 业务日志和错误码

验收标准：

- 主播开播后，界面能看到在线人数和在线成员列表
- 成员进入或离开房间时，主播端列表会实时更新
- 嘉宾申请时，主播端能看到请求
- 主播能从列表里选择某个用户发起邀请
- 被邀请用户端能看到邀请
- 任一方接受后双方状态进入 `linkmic-connecting`
- 被接受后，双方都能拿到正确的 `livekit_url / room_name / token`
- 任一方拒绝或超时后双方状态一致
- 结束连麦后房间能回到单人直播状态

### 阶段 2：RTC 建连与双向互动

目标：

- 主播和嘉宾之间打通真正的 `LiveKit RTC` 音视频

只做：

- 主播端使用 `LiveKit C++ SDK` 入房
- 嘉宾端使用网页 `LiveKit` SDK 入房
- 主播端发布本地音视频给嘉宾
- 主播端接收嘉宾音视频
- 双方基础断开清理

不做：

- 节目混流输出
- 多布局切换
- 自动重连策略

关键实现点：

- 主播端需要把本地采集结果同时送给 `LiveKit` 和节目输出链路
- 嘉宾端只需要网页 `LiveKit` 采集与播放
- 第一版建议要求主播使用耳机，避免先陷入复杂的回声消除问题

验收标准：

- 嘉宾能看到和听到主播
- 主播能看到和听到嘉宾
- 一次正常挂断后双方能恢复到可再次申请的状态

### 阶段 3：主播端节目视频合成

目标：

- 在主播端形成真正的双人节目画面

只做：

- 主播主画面
- 嘉宾画中画或固定双人分屏
- 固定布局
- 嘉宾离开后的布局回退

不做：

- 自由拖拽布局
- 模板编辑器
- 动态转场系统

建议实现：

- `ProgramVideoMixer` 输入一帧主播视频和一帧嘉宾视频
- 输出一路节目视频帧
- 第一版先只支持固定分辨率和固定布局

验收标准：

- 本地预览中能看到完整节目画面
- 嘉宾进入和退出时布局切换正确
- 没有嘉宾时节目画面退回单机位

### 阶段 4：主播端节目音频混合

目标：

- 在主播端形成真正的双人节目音频

只做：

- 主播麦克风 PCM
- 嘉宾远端 PCM
- 简单增益控制
- 静音开关

不做：

- 自动混音策略
- 智能降噪
- 复杂回声消除

建议实现：

- `ProgramAudioMixer` 在 PCM 层完成混合
- 节目链路只编码混合后的最终音频
- 第一版默认主播佩戴耳机

验收标准：

- 观众端只听到一路节目音频
- 主播和嘉宾声音同时存在时不出现明显爆音
- 嘉宾挂断后节目音频能回退到主播单路

### 阶段 5：接回现有 RTMP 节目输出链路

目标：

- 让观众看到真正的双人连麦节目流

只做：

- 把 `ProgramVideoMixer` 和 `ProgramAudioMixer` 输出送入现有编码推流链路
- 保持观众侧仍然只消费一路 RTMP 或 HTTP-FLV

不做：

- 观众端 RTC 观看
- 多码率转推
- 回看录制

关键点：

- 尽量复用 `VideoEncoder`、`AudioEncoder`、`FlvMuxer`、`RtmpPublisher`
- 不要把 `LiveKit RTC` 数据直接塞进 FLV 层
- RTMP 输出的时间戳基准要改为节目时钟，而不是单一本地采集时钟

验收标准：

- 单人直播时，节目流行为与当前版本一致
- 连麦开启后，观众看到的是双人节目画面
- 连麦结束后，不需要重开直播即可回到单人节目流

### 阶段 6：稳定性与运维补强

目标：

- 把第一版做成能持续联调和演示的版本

只做：

- `LiveKit` 连接失败处理
- 嘉宾超时未连上自动清理
- 主播手动踢出嘉宾
- 断网与重进房日志
- 关键链路指标上报

建议补齐的可观测性：

- 信令往返耗时
- `LiveKit` 入房耗时
- RTC 首帧耗时
- 节目流首帧耗时
- 节目混流帧率
- RTMP 输出连续性

验收标准：

- 网络异常时不会把直播主链路拖死
- 连麦失败不会影响主播继续单人直播
- 日志足够支持排查“建连失败、无声、无画、回声、音画不同步”

## 11. 主播端实现的关键难点

第一版最值得提前单独标记的难点有四个：

- `LiveKit C++ SDK` 与当前本地采集链路的集成边界
- 本地主播采集同时供给 `LiveKit RTC` 和 RTMP 两条链路
- 嘉宾远端音视频与本地节目时钟对齐
- 本地混音时的回声与监听路径控制

建议处理方式：

- 第一版优先要求主播佩戴耳机
- 第一版只支持固定分辨率和固定布局
- 第一版只支持一个嘉宾
- 第一版尽量减少可选项，先跑通闭环

## 12. 后续扩展路径

在第一版稳定后，再考虑下面这些扩展：

- 扩展到 `3` 到 `5` 人连麦
- 服务端混流
- 云端录制
- 布局模板系统
- 观众端更低延迟播放
- 多房 PK

如果后续需要减轻主播端本地混流压力，再评估是否引入 `LiveKit Egress` 或其他云端导出能力。第一版不依赖这条能力。

如果未来要扩展到多人连麦，第一版最需要保留的抽象就是：

- 连麦房间状态
- RTC 轨道管理
- 节目混流输入输出接口

这些抽象从一开始就要独立，不要和 `RtmpPublisher` 或单路采集流程硬耦合。

## 13. 建议的近期执行顺序

建议接下来的实际工作顺序是：

1. 先完成阶段 0，把 `LiveKit C++ SDK + token + room` 方案彻底定下来。
2. 再完成阶段 1，只做连麦请求、邀请与状态流转。
3. 然后完成阶段 2，验证主播和嘉宾通过 `LiveKit` 双向 RTC。
4. 再接阶段 3 和阶段 4，把本地节目混流拼起来。
5. 最后接阶段 5，把节目流重新接回现有 RTMP 推流链路。

如果阶段 2 在 `LiveKit C++ SDK` 接入上卡住，不要立刻推翻整体方案，而是先把卡点单独拆成预研任务。第一版成败的关键，不在 RTMP 推流，而在“主播端如何稳定拥有一条真正可用的双向 RTC 链路，并把它和现有本地节目链路接好”。
