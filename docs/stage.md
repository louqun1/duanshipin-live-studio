# duanshipin-live-studio 阶段总文档

这份文档是 `docs/` 下唯一的阶段总文档，用来持续记录“当前代码里已经实现的链路”。

使用规则：

- 只写已经落地到仓库里的实现，不写还没做的理想流程。
- 后续新增模块时，直接在这份文档里往后追加，不再拆新的 `stage_xx_*.md`。
- 如果代码实现和文档描述不一致，以代码为准，并同步更新这里。

## 1. 当前实现边界

当前项目真正已经跑通的入口是：

`apps/stage1_video_trace/main.cpp` 中的 `main(int argc, char** argv)`

当前已经实现的完整链路是：

`main -> initializeDefaultLogger -> LiveSessionController::runStage1VideoTrace -> VideoCaptureSource(file) -> AvTimeline -> VideoEncoder(FFmpeg H.264) -> FlvMuxer(real FLV video bytes) -> RtmpPublisher(real RTMP publish)`

这里的“推流全过程”目前指的是：

- 已经生成真实的 YUV420P 原始帧
- 已经用 FFmpeg 生成真实的 H.264 access unit
- 已经把 H.264 结果封装成带真实 `payloadBytes/tagBytes` 的 `FlvVideoTagPlan`
- 已经把 `FlvVideoTagPlan.payloadBytes` 映射成 `RtmpMessagePlan.payloadBytes`
- 已经完成真实 RTMP 握手、command message、chunk 组包和 socket 发送
- 已经调用 `RtmpPublisher::publishVideoTag()` 把 sequence header 和视频帧写到真实 TCP 连接

当前还没有完成的是 SRS 端到端验收，以及观众侧 HTTP-FLV 首帧验证。

## 2. 入口到推流的完整调用链

### 2.1 进程入口

文件：

- `apps/stage1_video_trace/main.cpp`

关键函数：

- `main(int argc, char** argv)`
- `tryParsePositiveInt(const char* text, int* outValue)`
- `printUsage(const std::filesystem::path& defaultSourceFile)`

`main()` 当前做的事情：

1. 计算 `projectRoot` 和默认源文件 `apps/stage1_video_trace/default_video_source.txt`
2. 解析命令行参数，得到：
   - `rtmpUrl`
   - `sourceConfigPath`
   - `frameCountOverride`
3. 调用 `live_studio::logging::initializeDefaultLogger()`
4. 创建：
   - `live_studio::TraceLog trace`
   - `live_studio::session::LiveSessionController controller`
5. 调用 `controller.runStage1VideoTrace(rtmpUrl, sourceConfigPath, frameCountOverride, &trace)`
6. 如果失败，输出错误并调用 `live_studio::logging::shutdown()`
7. 如果成功，输出当前阶段状态并调用 `live_studio::logging::shutdown()`

## 3. 日志初始化

文件：

- `src/logging/StudioLogger.hpp`

关键函数：

- `initializeDefaultLogger(const std::string& loggerName, const std::string& fileName, const std::filesystem::path& baseDir)`
- `currentLogFilePath()`
- `shutdown()`

当前实现行为：

- 在 `baseDir/logs` 下创建日志目录
- 同时挂控制台 sink 和文件 sink
- 设置全局默认 logger
- 把当前日志文件路径保存到 `currentLogFilePathStorage()`
- 第一条日志会由 `initializeDefaultLogger()` 自己写出

## 4. 会话总编排

文件：

- `src/session/LiveSessionController.hpp`

关键函数：

- `runStage1VideoTrace(const std::string& rtmpUrl, const std::string& sourceConfigPath, int frameCountOverride, TraceLog* trace)`
- `openSingleVideoStage(const std::string& rtmpUrl)`，当前未实现

`runStage1VideoTrace()` 是当前整个业务主链路的总编排函数。它会依次创建并使用：

- `capture::VideoCaptureSource capture`
- `timeline::AvTimeline timeline`
- `encode::VideoEncoder encoder`
- `mux::FlvMuxer muxer`
- `publish::RtmpPublisher publisher`

会话内的总流程顺序是：

1. `trace->push("session", "stage1 trace begins")`
2. `capture.configureFileSource(sourceConfigPath)`
3. `capture.start()`
4. `timeline.configureVideoClock(capture.config().fps)`
5. `encoder.configureH264(...)`
6. `publisher.configureVideoStream(...)`
7. `publisher.connect(rtmpUrl)`
8. `encoder.buildSequenceHeader(&parameterSets)`
9. `muxer.buildVideoSequenceHeader(parameterSets, &sequenceHeaderTag)`
10. `publisher.mapVideoTagToMessage(sequenceHeaderTag, &sequenceHeaderMessage)`
11. `publisher.publishVideoTag(sequenceHeaderMessage)`
12. 进入逐帧循环
13. 循环结束后执行 `capture.stop()` 和 `publisher.disconnect()`
14. `trace->push("session", "stage1 trace finished cleanly")`

## 5. 文件源加载与原始帧生成

文件：

- `src/capture/VideoCaptureSource.hpp`

关键函数：

- `configureFileSource(const std::string& sourcePath)`
- `start()`
- `captureFrame(int frameIndex, std::int64_t ptsMs, media::VideoFrame* outFrame) const`
- `stop()`
- `config() const`

内部辅助函数：

- `loadFileSourceConfig(const std::string& sourcePath, media::VideoCaptureConfig* outConfig)`
- `parsePositiveInt(const std::string& key, const std::string& value, int* outValue)`
- `trim(const std::string& text)`
- `fillSyntheticYuv420pPayload(int frameIndex, media::VideoFrame* outFrame)`
- `configureCamera(...)`，当前未实现

当前实现行为：

### 5.1 `configureFileSource()`

- 从文本配置文件读取 `source_label`、`width`、`height`、`fps`、`frame_count`
- 配置文件解析在 `loadFileSourceConfig()` 里完成
- 解析结果写入 `media::VideoCaptureConfig config_`

### 5.2 `start()`

- 校验当前配置是否有效
- 校验宽高是否满足 YUV420P 的偶数要求
- 只把内部状态切到 `started_ = true`

### 5.3 `captureFrame()`

当前 `captureFrame()` 不是从真实摄像头或真实视频文件读帧，而是：

1. 用 `frameIndex` 和 `ptsMs` 填充 `media::VideoFrame`
2. 设置：
   - `pixelFormat = media::VideoPixelFormat::yuv420p`
   - `lineStrideY`
   - `lineStrideU`
   - `lineStrideV`
3. 调用 `fillSyntheticYuv420pPayload()` 生成一帧 synthetic YUV420P 数据

### 5.4 `fillSyntheticYuv420pPayload()`

这个函数当前就是“原始视频帧来源”的最终落点。它会：

- 分配 `pixelPayload`
- 填充 Y 平面
- 填充 U 平面
- 填充 V 平面
- 把 `hasPixelPayload` 置为 `true`

所以当前已经不是“只有元数据、没有像素”的 trace 帧了，而是“有真实原始像素数据，但来源仍然是 synthetic 生成”的帧。

## 6. 视频时间线

文件：

- `src/timeline/AvTimeline.hpp`

关键函数：

- `configureVideoClock(int fps)`
- `nextVideoTick(VideoTick* outTick)`

当前实现行为：

- `configureVideoClock()` 根据 `fps` 初始化：
  - `nextFrameIndex_`
  - `elapsedUs_`
  - `baseFrameDurationUs_`
  - `remainderUs_`
  - `remainderBucket_`
- `nextVideoTick()` 每调用一次就生成一个新的：
  - `frameIndex`
  - `ptsMs`
  - `dtsMs`

这一步提供的是“逐帧调度时钟”，当前没有真实采集时钟，也没有音视频对齐逻辑。

## 7. FFmpeg H.264 编码

文件：

- `src/encode/VideoEncoder.hpp`
- `src/encode/VideoEncoder.cpp`

对外关键函数：

- `configureH264(int width, int height, int fps, int bitrate)`
- `buildSequenceHeader(media::H264ParameterSets* outSets) const`
- `encodeFrame(const media::VideoFrame& frame, media::EncodedVideoAccessUnit* outUnit)`
- `pushRawFrame(std::int64_t ptsMs)`，当前未实现

内部辅助函数：

- `ffmpegErrorString(int errorCode)`
- `startsWithStartCode(...)`
- `splitAnnexBNalus(...)`
- `describeH264Profile(std::uint8_t profileIdc)`
- `describeParameterSet(const char* label, const std::vector<std::uint8_t>& bytes)`
- `parseAvccParameterSets(...)`
- `parseAnnexBParameterSets(...)`
- `convertPacketToAnnexB(...)`
- `summarizeAccessUnitPayload(...)`
- `copyVideoFrameToAvFrame(...)`

### 7.1 `configureH264()`

当前编码初始化过程如下：

1. 校验宽高、帧率、码率
2. `av_log_set_level(AV_LOG_ERROR)`
3. 优先 `avcodec_find_encoder_by_name("libx264")`
4. 如果没找到，再回退到 `avcodec_find_encoder(AV_CODEC_ID_H264)`
5. `avcodec_alloc_context3()`
6. 设置 `AVCodecContext`：
   - `width`
   - `height`
   - `bit_rate`
   - `gop_size`
   - `max_b_frames = 0`
   - `pix_fmt = AV_PIX_FMT_YUV420P`
   - `time_base`
   - `framerate`
   - `AV_CODEC_FLAG_GLOBAL_HEADER`
7. 设置 FFmpeg 选项：
   - `preset=veryfast`
   - `tune=zerolatency`
   - `profile=baseline`
8. 调用 `avcodec_open2()`
9. 分配编码输入帧 `av_frame_alloc()`
10. 调用 `av_frame_get_buffer()`
11. 分配编码输出包 `av_packet_alloc()`
12. 从 `codecContext->extradata` 里提取 SPS/PPS：
    - 先走 `parseAvccParameterSets()`
    - 如果不是 AVCC，再走 `parseAnnexBParameterSets()`
13. 生成 `media::H264ParameterSets`

### 7.2 `buildSequenceHeader()`

这个函数当前不做二次计算，只是把 `configureH264()` 阶段已经保存好的 `parameterSets` 返回给上层。

### 7.3 `encodeFrame()`

每帧编码过程如下：

1. 校验 `outUnit`
2. 校验编码器已配置
3. 校验输入帧尺寸和帧率匹配
4. 调用 `copyVideoFrameToAvFrame()`
5. 把毫秒时间戳换算成编码器 `time_base`
6. 调用 `avcodec_send_frame()`
7. 调用 `avcodec_receive_packet()`
8. 调用 `convertPacketToAnnexB()` 把输出统一整理成 Annex B
9. 调用 `summarizeAccessUnitPayload()` 生成日志摘要
10. 输出 `media::EncodedVideoAccessUnit`

当前已经得到真实的：

- `keyFrame`
- `ptsMs`
- `dtsMs`
- `annexBPayload`

## 8. FLV 封装层

文件：

- `src/mux/FlvMuxer.hpp`

关键函数：

- `buildVideoSequenceHeader(const media::H264ParameterSets& parameterSets, media::FlvVideoTagPlan* outTag) const`
- `buildVideoNaluTag(const media::EncodedVideoAccessUnit& accessUnit, media::FlvVideoTagPlan* outTag) const`

当前实现行为：

- 这里会生成带真实字节的 `media::FlvVideoTagPlan`
- `payloadBytes` 是可直接映射到 RTMP message type 9 的 FLV VideoData
- `tagBytes` 是 11 字节 FLV tag header 加 VideoData 的完整 tag 字节
- `previousTagSize` 会保存当前 tag 对应的 `11 + dataSize`

具体来说：

### 8.1 `buildVideoSequenceHeader()`

- 消费 `H264ParameterSets`
- 先生成 `AVCDecoderConfigurationRecord`
- 再生成一条 `sequenceHeader = true` 的 `FlvVideoTagPlan`
- 写出真实的：
  - `payloadBytes`
  - `tagBytes`
  - `previousTagSize`
- `payloadSummary` 类似：
  - `avc-sequence-header(sps(...),pps(...))`

### 8.2 `buildVideoNaluTag()`

- 消费 `EncodedVideoAccessUnit`
- 把 `annexBPayload` 拆成 NALU，再转成 4 字节长度前缀的 AVC payload
- 根据 `dtsMs` 和 `ptsMs - dtsMs` 生成视频 tag 时间字段
- 生成逐帧 `FlvVideoTagPlan`
- 写出真实的：
  - `payloadBytes`
  - `tagBytes`
  - `previousTagSize`
- `payloadSummary` 类似：
  - `avc-keyframe-tag(...)`
  - `avc-interframe-tag(...)`

## 9. RTMP 发布层

文件：

- `src/publish/RtmpPublisher.hpp`

关键函数：

- `configureVideoStream(int width, int height, int fps)`
- `connect(const std::string& url)`
- `mapVideoTagToMessage(const media::FlvVideoTagPlan& tag, media::RtmpMessagePlan* outMessage) const`
- `publishVideoTag(const media::RtmpMessagePlan& message)`
- `disconnect()`

当前实现行为：

### 9.1 `configureVideoStream()`

- 保存视频宽、高、帧率
- 供 `connect()` 阶段自动发送 `@setDataFrame/onMetaData`

### 9.2 `connect()`

- 解析 `rtmp://host[:port]/app/stream`
- 建立 TCP socket
- 完成 RTMP simple handshake
- 发送并等待：
  - `connect`
  - `createStream`
  - `publish`
- 发送 `Set Chunk Size`
- 如果已经配置视频流信息，则发送 `@setDataFrame/onMetaData`
- 成功后把内部状态切到 `publishing`
- 保存 URL 和服务端返回的 `streamId`

### 9.3 `mapVideoTagToMessage()`

- 把 `FlvVideoTagPlan` 映射成 `RtmpMessagePlan`
- 固定写入：
  - `chunkStreamId = 6`
  - `messageStreamId = 当前 RTMP streamId`
  - `typeId = 9`
- 把 `FlvVideoTagPlan.payloadBytes` 原样拷贝到 `RtmpMessagePlan.payloadBytes`

### 9.4 `publishVideoTag()`

- 校验当前状态必须是 `publishing`
- 校验 `message.payloadBytes` 非空
- 按当前 chunk size 组装 RTMP chunk
- 通过真实 socket 写出 message payload
- 成功后把 `lastPublishedSummary_` 更新成最近一次消息摘要

这就是当前“推流”流程的最后一步。

### 9.5 `disconnect()`

- 清空状态
- 清空 URL
- 清空 `lastPublishedSummary_`
- 关闭 socket
- 释放 socket 子系统状态

## 10. 帧循环的真实执行顺序

在 `LiveSessionController::runStage1VideoTrace()` 里，逐帧循环的顺序是固定的：

1. `timeline.nextVideoTick(&tick)`
2. `capture.captureFrame(tick.frameIndex, tick.ptsMs, &frame)`
3. `encoder.encodeFrame(frame, &accessUnit)`
4. `muxer.buildVideoNaluTag(accessUnit, &tag)`
5. `publisher.mapVideoTagToMessage(tag, &message)`
6. `publisher.publishVideoTag(message)`

每一步之后都会调用 `TraceLog::push()` 写日志。

文件：

- `src/common/TraceLog.hpp`

关键函数：

- `TraceLog::push(std::string stage, std::string detail)`

`TraceLog::push()` 会同时：

- 把日志行保存到内存 `lines_`
- 调用 `spdlog::info()` 输出到控制台和日志文件

## 11. 当前可以用来定位代码的最短路径

如果你要从入口一路跟到“推流”收口，按下面顺序看就够了：

1. `apps/stage1_video_trace/main.cpp`
   - `main()`
2. `src/logging/StudioLogger.hpp`
   - `initializeDefaultLogger()`
3. `src/session/LiveSessionController.hpp`
   - `runStage1VideoTrace()`
4. `src/capture/VideoCaptureSource.hpp`
   - `configureFileSource()`
   - `loadFileSourceConfig()`
   - `start()`
   - `captureFrame()`
   - `fillSyntheticYuv420pPayload()`
5. `src/timeline/AvTimeline.hpp`
   - `configureVideoClock()`
   - `nextVideoTick()`
6. `src/encode/VideoEncoder.cpp`
   - `configureH264()`
   - `buildSequenceHeader()`
   - `encodeFrame()`
7. `src/mux/FlvMuxer.hpp`
   - `buildVideoSequenceHeader()`
   - `buildVideoNaluTag()`
8. `src/publish/RtmpPublisher.hpp`
   - `connect()`
   - `mapVideoTagToMessage()`
   - `publishVideoTag()`
   - `disconnect()`

## 12. 当前还没有实现的内容

以下内容先不写进“已实现全过程”，因为它们还没有真正落地：

- `VideoCaptureSource::configureCamera()`
- `LiveSessionController::openSingleVideoStage()`
- 真实摄像头采集
- 真实文件解码读帧
- SRS 端到端验收

## 13. 后续新增模块时怎么续写

后续如果新增了真实 RTMP 发送、真实摄像头采集或音频链路，直接按下面规则往这份文档继续补：

1. 先补“当前实现边界”
2. 再补“入口到推流的完整调用链”
3. 再补新增模块的文件、函数名、调用顺序和实现边界
4. 只写已经提交到仓库的真实函数，不写规划态伪流程
