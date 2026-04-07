# 当前架构盘点

## 1. 当前已经存在、可直接复用的资产

### 1.1 观众端 HTTP-FLV 看播链路

`duanshipin-pingtai` 已经具备一条完整的 Qt 看播主链：

`LivePlayerController -> LivePlayerSession -> HttpFlvStreamReader -> FlvDemuxer -> FlvAudioDecoder / FlvVideoDecoder -> SdlAudioOutput / VideoFrameQueue -> VideoOpenGLWidget`

可直接复用的核心点：

- `backend/liveplayer/service/LivePlayerController.*`
- `backend/liveplayer/session/LivePlayerSession.*`
- `backend/liveplayer/protocol/HttpFlvStreamReader.*`
- `backend/liveplayer/protocol/FlvDemuxer.*`
- `backend/liveplayer/decode/FlvVideoDecoder.*`
- `backend/liveplayer/decode/FlvAudioDecoder.*`
- `backend/liveplayer/audio/SdlAudioOutput.*`
- `frontend/widgets/VideoOpenGLWidget.*`

这意味着新主播端项目不需要重新写观众播放器，也不应该推翻现有看播实现。

### 1.2 现有服务端可复用基础

`duanshipin-pingtai/server` 已经有：

- Go API 工程骨架
- MySQL 连接与局部 schema 保障
- SSE broker 机制
- 认证/会话基础
- MinIO 接入
- 现成 docker-compose 环境

直接可借鉴的不是直播业务本身，而是这些工程能力：

- 服务端目录组织方式
- 配置加载方式
- SSE 事件推送模型
- MySQL / Redis / MinIO / SRS 的本地环境组织

### 1.3 现有部署环境可复用

当前 `duanshipin-pingtai/docker-compose.yml` 已经声明了：

- `mysql`
- `redis`
- `minio`
- `srs`

这说明新项目做阶段 1 到阶段 3 时，不需要从零重新准备本地媒体服务器环境。

## 2. 当前已经存在、但只能“参考”不能直接搬的资产

### 2.1 `rtmp-publish-20210903`

这个旧实验项目已经包含：

- 视频采集
- 音频采集
- H.264 编码
- AAC 编码
- 时间基与发送调度
- RTMP 推流骨架

但它的 RTMP 发送核心明显依赖 `librtmp`：

- `rtmpbase.cpp`
- `rtmppusher.cpp`
- `librtmp/*`

为什么不能直接拿来当本项目主实现：

- 这套实现能帮助理解打包与发送流程
- 但 RTMP 建连、状态机、发送细节仍大量交给 `librtmp`
- 这不符合“RTMP 学习目标优先”的新约束

结论：

- 可以借鉴它的模块拆分、采集到编码的节奏组织、旧日志命名习惯
- 不建议直接复用其 `RTMPBase / RTMPPusher / librtmp` 代码作为新项目正式主实现

### 2.2 `08-03-muxing_flv`

这个工程本质上是 FFmpeg `muxing` 路线实验，适合参考：

- `AVStream` 建立
- `time_base` 选择
- `av_packet_rescale_ts`
- `avformat_write_header`
- `av_interleaved_write_frame`

它适合作为“FLV 封装与音视频交织”的参考资料，但不适合作为：

- 摄像头直播程序
- RTMP 传输主实现
- 会话控制层

结论：

- 适合借鉴 `FlvMuxer` 和时间戳组织思路
- 不适合直接承载新主播端程序

## 3. 当前缺失的关键模块

### 3.1 主播端完全缺失

当前仓库没有一个按新目标设计的独立主播侧程序，缺失：

- `DeviceManager`
- `VideoCaptureSource`
- `AudioCaptureSource`
- `PreviewRenderer`
- `AvTimeline`
- `VideoEncoder`
- `AudioEncoder`
- `FlvMuxer`
- `RtmpPublisher`
- `LiveSessionController`

### 3.2 直播业务服务缺失

当前服务端没有真正的直播业务链路：

- 没有 `live_id / stream_key / push_url / play_url` API
- 没有直播状态模型
- 没有 SRS 回调接入
- 没有 Redis live key 设计
- 没有 live SSE 事件
- 没有 WebSocket signaling

### 3.3 RTC 与节目流模块缺失

当前没有：

- `RtcSignalingClient`
- `WebRtcSession`
- `CohostController`
- `ProgramComposer`
- `ProgramAudioMixer`
- `ProgramPublisher`

## 4. 复用决策

### 4.1 直接复用

- `duanshipin-pingtai` 观众端 HTTP-FLV 看播主链
- 现有 Go 服务端工程组织方式
- 本地 MySQL / Redis / MinIO / SRS 部署环境

### 4.2 借鉴但不直接搬

- `rtmp-publish-20210903`
- `08-03-muxing_flv`

### 4.3 必须新写

- 新主播端项目
- 新的 `RtmpPublisher`
- 新的会话总控层
- 后续直播业务 API / Redis / SSE / signaling
- 后续 RTC 与节目流模块

## 5. 当前主要风险

### 5.1 容易误用旧实验代码

最大的风险不是没有代码，而是把旧实验里“能跑但边界不清”的部分整块搬进来，导致新项目一开始就背上历史耦合。

### 5.2 容易把业务状态和媒体状态糊在一起

直播业务状态、RTMP 推流状态、SRS 发布状态、观众可看状态必须分层，不然阶段 3 会非常难收口。

### 5.3 容易提前做 WebRTC

如果阶段 1 和阶段 2 的单人直播主链都还没写清楚，就直接上 WebRTC，会同时丢掉 RTMP 学习主线和节目流分发边界。
