# 目标架构

## 1. 总体原则

- 观众端继续复用 `duanshipin-pingtai` 的 Qt 看播链路
- 主播端是一个新程序，不把媒体核心逻辑绑死在 Qt UI 上
- RTMP 阶段优先看清底层路径，不用高层黑盒 sink 抹平细节
- RTC 互动链路和观众节目流分发链路必须分开

## 2. 最终模块分层

### 2.1 主播端

#### 设备与采集层

- `DeviceManager`
- `VideoCaptureSource`
- `AudioCaptureSource`

#### 预览与时间线层

- `PreviewRenderer`
- `AvTimeline`
- `FrameBufferQueue`

#### 编码层

- `VideoEncoder`
- `AudioEncoder`

#### 封装与推流层

- `FlvMuxer`
- `RtmpPublisher`

#### RTC 层

- `RtcSignalingClient`
- `WebRtcSession`
- `CohostController`

#### 节目流层

- `ProgramComposer`
- `ProgramAudioMixer`
- `ProgramPublisher`

#### 总控层

- `LiveSessionController`

### 2.2 业务服务端

- `LiveRoomService`
- `LiveStateRepository`
- `LiveEventBroker`
- `SignalingGateway`
- `SrsCallbackHandler`

### 2.3 观众端

直接沿用现有：

`HTTP-FLV -> Demux -> Decode -> Render / Audio Play`

## 3. 分阶段主链路

### 3.1 阶段 1

`VideoCaptureSource -> VideoEncoder -> FlvMuxer(video only) -> RtmpPublisher -> SRS -> HTTP-FLV -> duanshipin-pingtai viewer`

### 3.2 阶段 2

`VideoCaptureSource + AudioCaptureSource -> VideoEncoder + AudioEncoder -> AvTimeline -> FlvMuxer(av interleave) -> RtmpPublisher -> SRS -> HTTP-FLV -> duanshipin-pingtai viewer`

### 3.3 阶段 3

`LiveSessionController -> Go live API -> live_id/stream_key/push_url/play_url -> RtmpPublisher -> SRS callback -> Redis + SSE -> viewer live list`

### 3.4 阶段 4

`Anchor A <-> signaling <-> Anchor B`

`Anchor A media <-> WebRTC <-> Anchor B media`

观众端不消费 RTC 房间流。

### 3.5 阶段 5

`Anchor A RTC stream + Anchor B RTC stream -> ProgramComposer / ProgramAudioMixer -> ProgramPublisher -> RTMP -> SRS -> HTTP-FLV -> viewer`

## 4. 新项目目录建议

```text
duanshipin-live-studio/
  apps/
    stage0_probe/
    publisher/
  src/
    common/
    capture/
    encode/
    mux/
    publish/
    session/
    rtc/
    program/
  server/
    api/
    signaling/
    callbacks/
    repository/
  docs/
```

## 5. 关键边界

### 5.1 `RtmpPublisher`

职责：

- 建连
- 握手
- command message
- chunk/message 组织
- 发送状态
- 重连

不负责：

- 摄像头采集
- H.264/AAC 编码
- UI 展示

### 5.2 `FlvMuxer`

职责：

- sequence header 组织
- video/audio tag 组织
- 音视频 tag 输出模型

不负责：

- RTMP 建连
- socket 发送

### 5.3 `AvTimeline`

职责：

- 统一 `pts/dts` 语义
- 管理采集时钟到发送时钟的映射
- 决定音视频交织顺序

### 5.4 `LiveSessionController`

职责：

- 编排阶段 1 到阶段 3 的单人直播主链
- 统一暴露高层状态给 UI

不负责：

- 具体编码细节
- 具体 RTMP 包结构

## 6. 阶段 1 之前明确不做的事

- 不做音频
- 不做 Redis/SSE 业务状态
- 不做 WebRTC
- 不做连麦
- 不做节目流合成
- 不做复杂 UI
