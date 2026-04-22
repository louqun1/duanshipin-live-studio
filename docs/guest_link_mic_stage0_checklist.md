# 连麦阶段 0 待办清单

本文档对应当前主实施路线：

- RTC：`libdatachannel`
- 拓扑：主播端为中心汇聚点
- 信令：自建 `WebSocket`，转发 `SDP / ICE`
- NAT 穿透：`coturn`
- 观众分发：`SRS`

为避免“主主播”这个词太拗口，本文档统一使用下面的称呼：

- `主播端`：节目主控端，负责收流、合流、最终推流到 `SRS`
- `连麦端`：加入 RTC 的另一端，可以是嘉宾，也可以是另一位主播
- `上麦参与者`：最终会进入节目画面的参与者统称

阶段 0 的目标不是做出连麦功能，而是把“开工前必须定死的事情”全部定下来，让阶段 1 可以直接开始实施。

## 阶段 0 完成后的状态

阶段 0 完成后，团队必须能明确回答下面这些问题：

- `libdatachannel` 怎么获取、怎么编译、怎么接进当前工程
- 主播端和连麦端分别用什么 probe 程序验证 RTC
- `coturn` 怎么启动，客户端拿到什么 `STUN/TURN` 配置
- 信令消息最小集合是什么
- 第一版编解码固定成什么
- 第一版只支持几个人同时上麦
- 第一版哪些东西明确不做

只要这些问题没有定下来，就不要开始阶段 1 和阶段 2 的实现。

## 当前阶段边界

阶段 0 只做：

- 方案冻结
- 依赖获取与本地联通验证
- 最小消息协议冻结
- probe 程序范围冻结
- 文档和目录约定冻结

阶段 0 不做：

- 真正的 UI
- 真正的音视频轨道传输
- 混流
- 业务接口的完整实现
- 房间页

## 总执行顺序

建议按下面顺序执行，不要乱序：

1. 先冻结术语和第一版边界。
2. 再确定依赖获取与构建策略。
3. 再把本地联调拓扑和端口定下来。
4. 再冻结最小信令消息。
5. 再定义两个 probe 程序的责任边界。
6. 最后做阶段 0 验收。

## 任务 0：冻结第一版边界

目标：

- 所有人对第一版范围没有歧义

必须定死的结论：

- 第一版上麦参与者最多 `3` 人，其中包含主播端
- 拓扑是“主播端中心汇聚”，不是 mesh
- 观众只看 `SRS` 上的一路节目流
- 第一版节目布局只支持：
  - `1` 人单画面
  - `2` 人双画面
  - `3` 人三画面
- 第一版 RTC 编码固定为：
  - 视频：`H.264`
  - 音频：`Opus`
- 节目输出编码固定为：
  - 视频：`H.264`
  - 音频：`AAC`
- 第一版只支持 Windows 原生客户端之间的 RTC

本任务产出：

- 当前主线文档确认使用 [guest_link_mic_plan_libdatachannel.md](D:/itffmpeg/av_media/online/duanshipin-live-studio/docs/guest_link_mic_plan_libdatachannel.md:1)
- 本文档

验收标准：

- 团队内部不再讨论“是不是先做浏览器端”
- 团队内部不再讨论“是不是先做纯 P2P mesh”

## 任务 1：冻结目录和命名

目标：

- 后续代码落地时不再临时起名

建议冻结的模块名：

- `src/linkmic/`
- `src/rtc/`
- `src/rtc/packet/`
- `src/decode/`
- `src/mix/`
- `apps/rtc_probe/`
- `apps/rtc_guest_probe/`

建议冻结的角色命名：

- `controller`：主播端
- `participant`：连麦端
- `active_participant`：当前已上麦参与者

建议冻结的核心类型名：

- `LinkMicSignalingClient`
- `RoomMemberDirectory`
- `ActiveParticipantRoster`
- `RtcPeerConnection`
- `RtcTransportConfig`
- `ProgramMixer`
- `ProgramLayoutPolicy`

本任务产出：

- 命名表
- 模块目录表

验收标准：

- 阶段 1 和阶段 2 代码不再出现 `guest_only`、`browser_only` 这种会把架构写死的命名

## 任务 2：确定 libdatachannel 获取方式

目标：

- 明确 `libdatachannel` 的来源、版本固定方式和接入方式

必须决定的事项：

- 是用源码构建，还是用预编译包
- 是直接放到 `third_party/`，还是独立下载到仓库外
- 是通过 `find_package`、子目录、还是导入库方式接入

推荐结论：

- 第一版优先用固定版本源码或稳定二进制，不跟随上游滚动
- 优先做“独立 probe 程序接入”，不要第一步就把它硬接进现有 `live_studio_core`

建议记录的信息：

- 获取来源
- 版本号或 commit
- include 路径
- 静态库或动态库路径
- 需要的编译器和架构

本任务产出：

- `libdatachannel` 接入说明
- 本地依赖路径约定

验收标准：

- 能在当前机器上重复完成一次获取和构建
- 新人按文档能复现同样结果

## 任务 3：确定构建策略

目标：

- 避免后续因为工具链混乱卡住

必须决定的事项：

- probe 程序是否与现有主工程共用同一套 CMake
- `libdatachannel` 相关目标优先用什么编译器
- 阶段 0 是否允许 probe 程序单独构建

推荐策略：

- 阶段 0 和阶段 2 的 probe 程序允许单独构建
- 不要求一开始就并入现有主工程所有 target
- 先把 RTC 探针跑通，再做集成

建议冻结的构建输出：

- `rtc_probe.exe`
- `rtc_guest_probe.exe`

本任务产出：

- 构建命令清单
- build 目录约定

验收标准：

- 能在当前机器上稳定生成两个 probe 的可执行文件

## 任务 4：冻结本地联调拓扑

目标：

- 把本地网络拓扑和服务依赖写清楚

建议的本地联调拓扑：

```text
rtc_probe.exe
  <-> signaling server (WebSocket)
  <-> coturn

rtc_guest_probe.exe
  <-> signaling server (WebSocket)
  <-> coturn

主播端节目输出
  -> RTMP
  -> SRS
```

必须决定的事项：

- signaling server 地址
- `coturn` 地址与端口
- `SRS` 推流地址
- 本机测试时是否允许全都跑在同一台机器

建议冻结的本地默认端口表：

- signaling：例如 `ws://127.0.0.1:18080/ws`
- `coturn`：`3478`
- `SRS RTMP`：`1935`
- `SRS HTTP-FLV/HLS`：按现有环境

本任务产出：

- 本地联调拓扑图
- 默认端口和地址表

验收标准：

- 不同同事按照文档能连到同一套本地服务

## 任务 5：冻结 coturn 使用方式

目标：

- 明确第一版到底怎么拿 TURN 配置

必须决定的事项：

- 第一版先只用 `STUN` 还是直接带 `TURN`
- 是否使用账号密码方式
- 这些配置由谁下发

推荐策略：

- 第一版 probe 允许先写死一套本地 `STUN/TURN` 配置
- 真正进入业务层时，再改成由 signaling/业务服务下发

建议冻结的字段：

- `urls`
- `username`
- `credential`

本任务产出：

- `coturn` 本地启动说明
- 客户端 `RtcTransportConfig` 字段定义

验收标准：

- probe 程序能成功读取并使用一套固定 `STUN/TURN` 配置

## 任务 6：冻结最小信令协议

目标：

- 阶段 1 和阶段 2 共享同一套最小消息协议

第一版最小消息集合建议固定为：

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

每条消息至少要冻结：

- `type`
- `room_id`
- `from_user_id`
- `to_user_id`
- `request_id`
- `payload`
- `ts_ms`

额外必须决定：

- `request_id` 是否由发起方生成
- `ice-candidate` 是逐条发送还是批量发送
- offer/answer 是否允许重发

推荐策略：

- `request_id` 由发起方生成
- `ICE` 逐条发送
- 第一版不支持复杂重协商

本任务产出：

- 消息字段表
- 示例 JSON

验收标准：

- 前后端和 probe 端都能按同一份表发消息

## 任务 7：冻结参与者模型

目标：

- 后续实现时不再纠结“主播-嘉宾”和“主播-主播”是不是两套逻辑

建议冻结的参与者抽象：

- `controller`
- `co_host`
- `guest`

建议冻结的节目画面参与规则：

- 只要进入 `active_participant` 集合，就进入节目画面
- 不区分他是嘉宾还是另一位主播
- 角色差异只体现在权限，不体现在混流逻辑

必须决定的事项：

- 谁能发邀请
- 谁能同意别人上麦
- 谁能踢人下麦
- 谁控制布局

推荐策略：

- 第一版统一由主播端控制邀请、接受、踢人、布局

本任务产出：

- 参与者角色表
- 权限矩阵

验收标准：

- 团队内部不再把“主播-主播”当成另一套 RTC 架构

## 任务 8：冻结 probe 程序职责

目标：

- 阶段 2 能直接照着做

建议的两个 probe：

### `rtc_probe`

责任：

- 模拟主播端
- 主动创建或接收 RTC 会话
- 打印连接状态
- 打印 DataChannel 消息
- 打印远端 SDP/ICE 到达情况

### `rtc_guest_probe`

责任：

- 模拟连麦端
- 接收邀请
- 回应 offer/answer
- 打印连接状态
- 打印 DataChannel 消息

阶段 0 明确不要求：

- 真正音视频采集
- 真正画面预览
- 真正混流

本任务产出：

- 两个 probe 的功能边界说明

验收标准：

- 不会在阶段 2 里一边写 probe，一边临时讨论 probe 到底要干什么

## 任务 9：冻结阶段 0 的验收命令

目标：

- 阶段 0 结束时有一套可重复执行的最小验证步骤

建议的最终验收顺序：

1. 启动 signaling server。
2. 启动 `coturn`。
3. 启动 `rtc_probe.exe`。
4. 启动 `rtc_guest_probe.exe`。
5. 手工触发一次邀请或申请。
6. 观察 `offer / answer / ICE` 是否完整流转。
7. 观察 DataChannel 是否连通。

必须能看到的结果：

- 两端都进入 `connected`
- 两端都打印远端 candidate 到达
- 两端都能收发一条测试消息

本任务产出：

- 阶段 0 验收步骤文档

验收标准：

- 按文档执行一次，能稳定完成最小建连

## 阶段 0 最终交付物

阶段 0 完成时，仓库里至少应有：

- 当前主线架构文档  
  [guest_link_mic_plan_libdatachannel.md](D:/itffmpeg/av_media/online/duanshipin-live-studio/docs/guest_link_mic_plan_libdatachannel.md:1)
- 本文档  
  [guest_link_mic_stage0_checklist.md](D:/itffmpeg/av_media/online/duanshipin-live-studio/docs/guest_link_mic_stage0_checklist.md:1)
- `libdatachannel` 接入说明
- 本地 `coturn` 使用说明
- 最小信令字段表
- probe 程序职责说明

## 完成定义

只有当下面这些条件同时成立，阶段 0 才算完成：

- 主线方案不再摇摆
- 角色命名和目录命名不再摇摆
- `libdatachannel` 获取方式确定
- probe 构建策略确定
- `coturn` 使用方式确定
- 最小信令字段表确定
- 阶段 0 验收步骤可重复执行

如果其中任意一项还没定，就不要进入阶段 1 或阶段 2。
