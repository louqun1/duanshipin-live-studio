# 阶段 1 报告

## A. 当前目标

阶段 1 不能一上来就做成黑盒推流器。
这次里程碑的目标是：先把“单人视频直播主链路”的模块边界做成一个可运行的 trace 管线。

## B. 本次实际实现范围

已经实现：

- 阶段 1 视频链路的数据对象
- 一个用于视频 `pts/dts` 的 trace 时钟
- 一个可控的 synthetic 采集源入口
- trace 版编码、FLV 封装规划、RTMP 发送规划
- 项目内 vendored `spdlog` 控制台 + 文件日志
- 一个可运行的 `stage1_video_trace` 控制台程序
- 阶段 1 数据流文档

还没有实现：

- 真实摄像头采集
- 真实 H.264 字节流
- 真实 FLV 字节布局
- 真实 RTMP 网络发送
- SRS 端到端验收
- Qt 观众端首帧验收

## C. 当前核心数据流

当前已经能跑起来的 trace 链路：

`VideoCaptureSource -> AvTimeline -> VideoEncoder -> FlvMuxer -> RtmpPublisher`

阶段 1 的真实目标链路仍然是：

`摄像头 -> H.264 -> FLV -> RTMP -> SRS -> HTTP-FLV -> Qt 观众端`

## D. 本次改动文件

新增：

- `apps/stage1_video_trace/main.cpp`
- `apps/stage1_video_trace/CMakeLists.txt`
- `src/common/TraceLog.hpp`
- `src/logging/StudioLogger.hpp`
- `src/common/MediaTypes.hpp`
- `src/timeline/AvTimeline.hpp`
- `scripts/build_stage1_video_trace.ps1`
- `docs/stage_01_dataflow.md`
- `docs/stage_01_report.md`
- `third_party/spdlog/include/spdlog/*`

修改：

- `CMakeLists.txt`
- `src/capture/VideoCaptureSource.hpp`
- `src/encode/VideoEncoder.hpp`
- `src/mux/FlvMuxer.hpp`
- `src/publish/RtmpPublisher.hpp`
- `src/session/LiveSessionController.hpp`

## E. 运行方法

```powershell
cmake -S . -B build
cmake --build build --target stage1_video_trace --config Debug
.\build\apps\stage1_video_trace\stage1_video_trace.exe
```

脚本方式：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_stage1_video_trace.ps1
```

如果你想改推流地址和帧数，也可以这样：

```powershell
.\build\apps\stage1_video_trace\stage1_video_trace.exe rtmp://127.0.0.1/live/demo 8
```

## F. 验证方式

重点检查控制台和日志文件里是否清楚出现以下节点：

- 会话启动
- 采集源配置
- 时间线生成的帧序号与 `pts`
- 编码器输出 sequence header 规划
- FLV tag 规划
- RTMP message 规划
- 结束收尾
- 同样内容是否落盘到 `logs/stage1_video_trace.log`

预期现象：

- 程序退出码为 `0`
- 日志里能看到 1 条 sequence header 和多条视频帧规划
- 当前里程碑不依赖真实摄像头，也不会真的发网络包

## G. 当前已知缺口

- 这还不能算阶段 1 通过
- 还没有真实媒体 payload
- 还没有真实 RTMP 握手和 socket 发送路径
- 还没有 SRS 侧和观众端的验收日志

## H. 下一步建议

现在还不应该进入阶段 2。
下一步仍然应该继续留在阶段 1 内，把 trace 版逐步替换成真实链路：

1. 用可控文件源或真实摄像头替换 synthetic capture
2. 用真实 H.264 access unit 替换 trace 编码输出
3. 用真实握手和发送逻辑替换 RTMP message 规划
4. 完成 `RTMP -> SRS -> HTTP-FLV -> Qt viewer` 端到端验证
