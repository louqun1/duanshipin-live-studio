# 阶段 1 数据流说明

## 目标

阶段 1 仍然围绕第一条直播主链路展开：

`摄像头 -> H.264 编码 -> FLV 视频 tag -> RTMP 推流 -> SRS -> HTTP-FLV -> Qt 观众端`

当前这次提交并不宣称整条链路已经完成。
它落下的是一个“可运行的 trace 里程碑”，目的是在写真实媒体和网络代码之前，先把模块边界跑通、讲清、固定下来。

## 当前可运行的 Trace 路径

`synthetic VideoFrame -> EncodedVideoAccessUnit -> FlvVideoTagPlan -> RtmpMessagePlan`

这个 trace 程序沿用的就是后续真实阶段 1 会继续使用的模块名：

- `VideoCaptureSource`
- `AvTimeline`
- `VideoEncoder`
- `FlvMuxer`
- `RtmpPublisher`
- `LiveSessionController`

当前日志依赖使用项目内 vendored 的 `spdlog`：

- `third_party/spdlog/include/spdlog`

## 每一跳分别代表什么

### 1. `VideoCaptureSource`

当前只生成一个可控的“伪采集帧描述”。
真实摄像头接入、原始帧回调、帧内存管理，仍然需要你后面亲手补写。

### 2. `AvTimeline`

负责把配置好的 fps 变成单调递增的帧序号和 `pts/dts`。
这是阶段 1 里第一次明确写出“视频时间是怎么往前走的”。

### 3. `VideoEncoder`

当前会把伪采集帧映射成 trace 版 `EncodedVideoAccessUnit`。
它还会合成一个占位版的 SPS/PPS 描述，方便后面的 mux 步骤明确“AVC sequence header 从哪来”。

### 4. `FlvMuxer`

负责把编码输出映射成 trace 版 FLV 视频 tag 规划：

- 一条 AVC sequence header
- 每个 access unit 对应一条视频 tag

### 5. `RtmpPublisher`

负责把 FLV tag 规划进一步映射成 trace 版 RTMP message 规划。
当前里程碑不会真的打开 socket。
后续仍然需要你亲手补写握手、command message、chunk 组织、发送节奏、重连状态等内容。

### 6. `LiveSessionController`

负责整体编排顺序，并且在每一跳输出可读日志。

## 需要你后面亲手补写的 TODO

- 真实摄像头设备选择与帧回调路径
- 原始帧数据布局与所有权管理
- 真实 H.264 编码器初始化与送帧
- 从编码输出中提取 SPS/PPS
- 真实 FLV AVC tag 字节布局
- RTMP 握手与 command message 流程
- RTMP chunk/message 序列化与网络发送循环
- 对接现有 Qt HTTP-FLV 看播端，完成 SRS 链路验证

## 验证入口

直接构建并运行：

```powershell
cmake -S . -B build
cmake --build build --target stage1_video_trace --config Debug
.\build\apps\stage1_video_trace\stage1_video_trace.exe
```

如果你当前更习惯脚本方式，也可以直接跑：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_stage1_video_trace.ps1
```

运行后重点看两处：

- 控制台日志
- `logs/stage1_video_trace.log`
