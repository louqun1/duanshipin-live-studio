# duanshipin-live-studio 阶段说明

本文记录当前仓库里已经真正落地、能够跑起来的代码链路。

## 当前边界

当前可运行入口是：

`apps/stage1_video_trace/main.cpp`

当前阶段 1 链路是：

`main -> initializeDefaultLogger -> LiveSessionController::runStage1VideoTrace -> LiveDeviceCaptureSource(Media Foundation + WASAPI) -> VideoEncoder(H.264) + AudioEncoder(AAC) -> FlvMuxer -> RtmpPublisher`

当前状态：

- `stage1_video_trace` 已经不再使用媒体文件作为输入源。
- 摄像头采集走 `Media Foundation`。
- 麦克风采集走 `WASAPI/MMDevice`。
- 视频会编码成真实的 H.264 access unit。
- 音频会编码成真实的 AAC access unit。
- FLV 音视频 tag 由真实编码后的字节构建。
- RTMP 的 `connect / createStream / publish / metadata / chunked media send` 已经是真实实现。

## 入口与参数

`apps/stage1_video_trace/main.cpp` 里的 `main(int argc, char** argv)` 当前会：

1. 解析 `projectRoot`。
2. 使用 `apps/stage1_video_trace/default_live_devices.txt` 作为默认直播配置。
3. 解析以下参数：
   - `rtmpUrl`
   - `captureConfigPath`
   - `frameCountOverride`
4. 初始化日志器。
5. 调用 `LiveSessionController::runStage1VideoTrace(...)`。

## 直播会话控制器

`src/session/LiveSessionController.hpp`

当前 `runStage1VideoTrace(...)` 负责串起下面这些步骤：

1. `capture.configureDeviceSource(captureConfigPath)`
2. `capture.start()`
3. `videoEncoder.configureH264(...)`
4. 可选 `audioEncoder.configureAac(...)`
5. `publisher.configureVideoStream(...)`
6. 可选 `publisher.configureAudioStream(...)`
7. `publisher.connect(rtmpUrl)`
8. 发送 H.264 sequence header
9. 可选发送 AAC sequence header
10. 循环读取实时采集事件，直到达到目标视频帧数
11. flush 尚未输出的 AAC 包
12. `capture.stop()` 与 `publisher.disconnect()`

采集循环当前是事件驱动的：

- 视频事件走 `VideoEncoder -> FlvMuxer::buildVideoNaluTag -> RtmpPublisher`
- 音频事件走 `AudioEncoder -> FlvMuxer::buildAudioRawTag -> RtmpPublisher`

## 实时设备采集

`src/capture/LiveDeviceCaptureSource.hpp`
`src/capture/LiveDeviceCaptureSource.cpp`

当前已实现：

- 文本配置解析，支持：
  - `source_label`
  - `video_device`
  - `audio_device`
  - `width`
  - `height`
  - `fps`
  - `sample_rate`
  - `channels`
  - `frame_count`
- 通过 Media Foundation 枚举并选择摄像头
- 通过 WASAPI/MMDevice 枚举并选择麦克风
- 将 Media Foundation sample 转成原始 YUV420P 视频帧
- 将 WASAPI packet 转成交错的 S16 PCM 音频帧
- 音视频各自采集线程，以及共享事件队列
- 事件输出类型：
  - `CaptureEventKind::video`
  - `CaptureEventKind::audio`

当前选择行为：

- `video_device=default` 选择第一个可用的 Media Foundation 摄像头
- `audio_device=default` 选择默认的 WASAPI 采集端点
- 也支持显式填写 friendly name

## 编码器

`src/encode/VideoEncoder.cpp`

- 真实的 FFmpeg H.264 编码器
- 从编码器 extradata 中提取 SPS/PPS
- 返回 `media::EncodedVideoAccessUnit`

`src/encode/AudioEncoder.cpp`

- 真实的 FFmpeg AAC 编码器
- 将交错的 S16 PCM 转成编码器需要的采样格式
- 使用 `AVAudioFifo` 做样本缓存
- 规范化 AAC 包时间戳，保证 FLV/RTMP 时间戳非负
- 返回 0 个或多个 `media::EncodedAudioAccessUnit`
- 从编码器 extradata 中暴露 AAC sequence-header 配置

## FLV 封装

`src/mux/FlvMuxer.hpp`

当前已实现：

- AVC sequence header
- AVC NALU 视频 tag
- AAC sequence header
- AAC 原始音频 tag
- 真实的 FLV tag header 字节，覆盖 tag type `9` 和 tag type `8`

## RTMP 推流器

`src/publish/RtmpPublisher.cpp`

当前已实现：

- RTMP URL 解析
- TCP 连接
- RTMP simple handshake
- `connect`
- `createStream`
- `publish`
- `Set Chunk Size`
- `@setDataFrame/onMetaData`
- 视频消息映射与发送
- 音频消息映射与发送

当配置了视频时，metadata 会带上视频字段；当配置了音频时，metadata 会带上音频字段。

## 验证状态

代码与构建层面已验证：

- `stage1_video_trace` 能在当前 MinGW 工具链下成功编译
- Media Foundation 和 WASAPI 已经可以正确链接以下库：
  - `mf`
  - `mfplat`
  - `mfreadwrite`
  - `mfuuid`
  - `mmdevapi`
  - `ole32`
  - `uuid`
  - `avrt`
  - `ksuser`
- 基于 FFmpeg 的 encode/mux/publish 仍然能正确链接以下库并成功编译：
  - `FFmpeg::avcodec`
  - `FFmpeg::avutil`
  - `FFmpeg::swscale`
  - `FFmpeg::swresample`

当前工作站上的运行验证结果：

- 使用默认摄像头 + 默认麦克风的 fake RTMP server smoke test 已成功
- 服务端观察到：
  - `connect`
  - `createStream`
  - `publish`
  - `@setDataFrame/onMetaData`
  - H.264 sequence header + 视频帧
  - AAC sequence header + 音频帧
- 使用摄像头 + 麦克风向真实 SRS 推流也已成功：
  - 进入 `rtmp://192.168.99.128:1935/live/livestream` 的 publishing 状态
  - 已发送 H.264 和 AAC sequence header
  - 已跑到 `video frame#299`
  - 最终以 `stage1 trace finished cleanly` 正常结束
- 实时设备日志现在会额外报告：
  - 启动前请求的摄像头/麦克风名称
  - 启动后实际选中的摄像头/麦克风 friendly name
  - 查找失败时可供选择的设备名列表

## 已从阶段 1 入口移除

当前从 `stage1_video_trace` 中移除的内容有：

- 媒体文件作为直播输入源
- 基于合成数据或文件的阶段 1 用法与文档
- DirectShow 作为默认摄像头/麦克风采集路径

如果后续需要确定性的离线测试，应走独立的原始 `yuv + pcm` 路径，而不是复用当前的实时设备入口。

## 待做

房间页 观众端页面 多路推流切换 连麦 UI 美颜滤镜 业务层 Redis/SSE 联动 推流过程中动态改分辨率