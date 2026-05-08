## duanshipin-live-studio 技术架构与实现方案

本文档基于对仓库源码与构建配置（CMake、`apps/`、`src/`、`docs/`、`scripts/`）的扫描整理，给出当前项目的**技术架构**、**核心链路实现**、**模块边界**、**构建/运行方式**与**后续扩展落点**。

该仓库定位为**Windows 主播端/导播端**工程，并包含用于连麦链路验证的 probe 程序；真实业务服务端约定在 `../duanshipin-pingtai/server`（Go）中扩展，不在本仓库维护。

---

## 1. 项目目标与边界

### 1.1 当前已落地的能力（Stage-1）

当前仓库已经可跑通的主链路（stage-1）是**本地采集并推流到 SRS/RTMP**：

```text
LiveDeviceCaptureSource (Media Foundation + WASAPI/MMDevice)
  -> VideoEncoder (FFmpeg H.264)
  -> AudioEncoder (FFmpeg AAC, 可选)
  -> FlvMuxer (FLV tags)
  -> RtmpPublisher (RTMP handshake + publish + chunked media send)
  -> SRS / CDN / 观众
```

对应说明文档见 `README.md`。

### 1.2 连麦路线（libdatachannel 方案）

仓库同时承载了连麦路线的“探针”程序（`rtc_probe` / `rtc_guest_probe`），用于验证：

- Go 业务服务（HTTP + WebSocket）转发信令（SDP/ICE）与房间事件；
- `libdatachannel` 的 PeerConnection / DataChannel / Track 建连；
- RTC 音频第一版建议采用 `Opus`（而不是复用 RTMP 链路的 AAC）；
- 收到的 Opus 音频可解码并落盘为 WAV，用于客观验证媒体链路。

主路线文档见 `docs/guest_link_mic_plan_libdatachannel.md`。

---

## 2. 代码结构总览（模块地图）

### 2.1 顶层目录

- **`src/`**：核心库 `live_studio_core` 的实现（采集/编码/封装/推流/会话控制/日志与通用类型）。
- **`apps/`**：可执行程序目标（当前仓库含 `rtc_probe`、`rtc_guest_probe`、`live_studio_panel`；stage-1 trace app 目录可能按条件存在）。
- **`cmake/`**：构建辅助（`LiveStudioRtcDeps.cmake`：libdatachannel 查找与运行时 DLL 拷贝逻辑）。
- **`docs/`**：阶段说明、连麦实施方案与 checklist。
- **`scripts/`**：联调/验收脚本（例如 `scripts/rtc_mic_smoke_test.ps1`）。
- **`server/`**：仅保留服务端职责/协议约定说明（实际服务端在外部 Go 工程）。

### 2.2 核心库：`live_studio_core`（CMake target）

顶层 `CMakeLists.txt` 会创建 `live_studio_core`，其源码聚合如下（节选）：

- **采集**：`src/capture/LiveDeviceCaptureSource.*`
- **编码**：`src/encode/VideoEncoder.*`、`src/encode/AudioEncoder.*`、`src/encode/OpusAudioEncoder.*`
- **解码**：`src/decode/OpusAudioDecoder.*`
- **封装**：`src/mux/FlvMuxer.hpp`
- **推流**：`src/publish/RtmpPublisher.*`
- **会话控制**：`src/session/LiveSessionController*.cpp/.hpp`
- **通用**：`src/common/*`（如 `Status`、`MediaTypes`、`TraceLog`、`WavFileWriter` 等）

该库面向“主播端主链路”，也被 probe app 复用（例如 probe 的音频采集/Opus 编解码/WAV 写入）。

---

## 3. 构建系统与依赖管理（CMake）

### 3.1 基本构建约定

- 语言标准：`C++17`
- 构建系统：`CMake >= 3.20`
- 平台重点：Windows（Media Foundation、WASAPI、WinSock、ole32/uuid 等链接）

### 3.2 第三方依赖（仓库内 vendored + 外部包）

#### FFmpeg（vendored）

通过 `third_party/ffmpeg6.0` 以“导入共享库”的方式链接：

- `FFmpeg::avcodec`
- `FFmpeg::avdevice`
- `FFmpeg::avformat`
- `FFmpeg::avutil`
- `FFmpeg::swresample`
- `FFmpeg::swscale`

> 说明：CMake 里以 `.lib` + `.dll` 路径导入，运行时会把 FFmpeg 的 DLL 拷贝到目标输出目录（Qt panel 目标中显式拷贝；probe 目标通过通用函数拷贝）。

#### spdlog（vendored include）

`third_party/spdlog/include` 作为 include-only 使用；找不到则 CMake 直接 `FATAL_ERROR`。

#### Qt 6（可选）

`LIVE_STUDIO_BUILD_QT_PANEL` 控制是否构建 `apps/live_studio_panel`：

- 默认：在 GCC + `llvm-mingw` Qt 的组合下默认关闭（避免 ABI/工具链不匹配）。
- panel 目标使用 `Qt6::Core/Gui/Widgets/Concurrent`。
- 目标后置步骤：调用 `windeployqt` 进行运行时部署（若能找到）。

#### libdatachannel（可选）

`LIVE_STUDIO_BUILD_RTC_PROBES` 控制是否构建 RTC probe：

- `live_studio_require_libdatachannel()` 通过 `find_package(LibDataChannel CONFIG REQUIRED)` 查找；
- 可通过 `LIVE_STUDIO_LIBDATACHANNEL_INSTALL_DIR` 指定安装目录 hint；
- probe app 会链接 `LibDataChannel::LibDataChannel`。

#### nlohmann/json（header-only）

probe 代码 include 了 `third_party/libdatachannel-src/deps/json/single_include` 作为 JSON 解析依赖。

### 3.3 关键 CMake 选项与目标

- **`live_studio_core`**：核心库（采集/编码/封装/推流/会话控制）。
- **`rtc_probe`**：probe（controller 侧），源码来自 `apps/rtc_probe_common/*` + `apps/rtc_probe/main.cpp`。
- **`rtc_guest_probe`**：probe（guest 侧），源码来自 `apps/rtc_probe_common/*` + `apps/rtc_guest_probe/main.cpp`。
- **`live_studio_panel`**：Qt Widgets 发布面板。

### 3.4 常用构建与运行命令（Windows / PowerShell）

#### 仅构建核心链路（默认）

```powershell
cmake -S . -B build -D CMAKE_BUILD_TYPE=Debug
cmake --build build --target live_studio_core
```


#### 构建 RTC probes（需要 libdatachannel 已安装）

建议使用独立构建目录（例如 `build-rtc`）：

```powershell
cmake -S . -B build-rtc -D CMAKE_BUILD_TYPE=Debug `
  -D LIVE_STUDIO_BUILD_RTC_PROBES=ON `
  -D LIVE_STUDIO_LIBDATACHANNEL_INSTALL_DIR="D:/path/to/libdatachannel-install"

cmake --build build-rtc --target rtc_probe
cmake --build build-rtc --target rtc_guest_probe
```

运行时（参数以 `SignalProbeApp` usage 为准）：

```powershell
.\build-rtc\apps\rtc_probe\rtc_probe.exe --help
.\build-rtc\apps\rtc_guest_probe\rtc_guest_probe.exe --help
```

#### 构建 Qt panel（可选，工具链需与 Qt 包匹配）

仓库 README 提供了一个典型的 clang/llvm-mingw Qt 构建目录示例：

```powershell
cmake -S . -B build-qt -G "MinGW Makefiles" `
  -D CMAKE_BUILD_TYPE=Debug `
  -D CMAKE_C_COMPILER=D:/itffmpeg/Qt/Tools/llvm-mingw1706_64/bin/clang.exe `
  -D CMAKE_CXX_COMPILER=D:/itffmpeg/Qt/Tools/llvm-mingw1706_64/bin/clang++.exe `
  -D LIVE_STUDIO_QT_ROOT=D:/itffmpeg/Qt/6.10.2/llvm-mingw_64

cmake --build build-qt --target live_studio_panel
.\build-qt\apps\live_studio_panel\live_studio_panel.exe
```

#### RTC 麦克风互通 smoke test

在完成 `build-rtc` 后，可直接跑脚本：

```powershell
.\scripts\rtc_mic_smoke_test.ps1 `
  -GuestJoinParamsFile ".\guest-join.json" `
  -ControllerJoinParamsFile ".\host-join.json"
```

---

## 4. 运行时架构（Stage-1 推流链路）

### 4.1 入口与会话控制

stage-1 链路由 `LiveSessionController` 串联，公开能力包括：

- `open/openSingleVideoStage/startPreview/startPublish/stopPublish/close`：用于 UI 或长期运行会话（panel 方向）

其中 `LiveSessionController` 的诊断快照 `SessionDiagnostics` 用于 UI/日志显示：

- 采集/预览/推流状态（`opened/previewRunning/publishing`）
- 推流计数与首包延迟（first-media latency）
- 选中的设备名与最后一条 payload summary
- 最近错误 `lastError`
- 设备探测结果 `capture::LiveDeviceProbeResult`

### 4.2 采集模块：`LiveDeviceCaptureSource`

职责：从 Windows 实时设备采集并输出统一事件流。

- **视频**：Media Foundation 枚举与采集，输出原始 `YUV420P`（见 `README.md` 说明）。
- **音频**：WASAPI/MMDevice 枚举与采集，输出交错 `S16 PCM`。
- **配置**：支持从文本配置加载（如 `width/height/fps/sample_rate/channels/video_device/audio_device/frame_count` 等）。
- **事件模型**：`CaptureEvent`（kind=video/audio）+ 共享队列；提供 `readNext()` 拉取事件。

该模块是整个项目“设备侧”的单一入口点，也是后续做设备列表模式/多设备/热切换的扩展落点。

### 4.3 编码模块：Video(H.264) / Audio(AAC) / Opus(用于 RTC)

#### VideoEncoder（H.264）

- 使用 FFmpeg H.264 编码器输出 access unit；
- 从 extradata 提取 SPS/PPS（供 FLV/RTMP 的 AVC sequence header）。

#### AudioEncoder（AAC，用于 RTMP/FLV）

- 将交错 S16 PCM 转换到编码器需要的采样格式；
- 通过 `AVAudioFifo` 进行样本缓存，输出 0..N 个 AAC access units；
- 规范化时间戳（避免 FLV/RTMP 出现负时间戳；并对 DTS/PTS 做单调修正）。

#### OpusAudioEncoder / OpusAudioDecoder（用于 RTC 探针与后续连麦）

仓库已新增 Opus 的编解码器（用于 RTC 链路验证）：

- `encode/OpusAudioEncoder`
- `decode/OpusAudioDecoder`

它们与 `RtcAudioProbe` 组合，用于“麦克风→Opus RTP→对端→Opus 解码→PCM WAV”验证链路。

### 4.4 封装模块：`FlvMuxer`

职责：将编码后的 H.264/AAC 封装为 FLV tag 字节与辅助计划结构。

支持：

- **视频**：
  - AVC sequence header（AVCDecoderConfigurationRecord）
  - AVC NALU tag（Annex-B NALU 切分 → AVCC length-prefixed → FLV tag）
  - 计算 composition time（\(PTS - DTS\)）并写入 FLV 24-bit signed 字段
- **音频**：
  - AAC sequence header（AudioSpecificConfig）
  - AAC raw tag（带 FLV sound format/rate/size/type header）

输出为 `media::FlvVideoTagPlan` / `media::FlvAudioTagPlan`，上游只需关注“payload summary + tag bytes”。

### 4.5 推流模块：`RtmpPublisher`

职责：将 FLV tag 映射为 RTMP message，并完成 RTMP 连接/发布流程。

核心能力（见 `README.md`）：

- RTMP URL 解析、TCP 连接
- RTMP simple handshake
- `connect/createStream/publish`
- `Set Chunk Size`
- `@setDataFrame/onMetaData`
- 发送 video/audio message（含 sequence header 与媒体帧）

状态机：`RtmpConnectionState`（idle/connecting/connected/publishing/reconnecting/failed）。

### 4.6 线程与队列（流水线实现要点）

`LiveSessionControllerPipeline.cpp` 体现了当前“推流阶段”的典型实现方式：

- **采集线程（设备侧）**：产出 `CaptureEvent`。
- **编码线程（视频/音频分离）**：
  - `videoEncodeThreadMain`：准备/时间戳映射 → H.264 编码 → 投递 sender 队列
  - `audioEncodeThreadMain`：AAC 编码 → 时间戳归一化 → 投递 sender 队列
- **发送线程**：
  - `senderThreadMain`：`FlvMuxer` 打 tag → `RtmpPublisher` 映射 message → socket 发送

并附带：

- 统计窗口（按 1s 窗口计算 input fps / encode fps / 编码耗时分解 / 队列深度变化）
- 丢帧/排队等待指标
- 首包延迟统计（从捕获 wall-clock 到 RTMP send 完成的延迟）

这套设计的价值在于：

- 把“耗时大/阻塞风险高”的网络发送从编码阶段隔离；
- 为 UI（panel）和未来稳定性增强（重连、动态改码率/分辨率）提供可观测性基础；
- 通过队列容量与 drop 策略控制实时性（宁可丢帧也不无限堆积导致延迟爆炸）。

---

## 5. RTC Probe 架构（libdatachannel + 信令 + Opus 音频探测）

> 这一部分是“连麦路线”的工程化验证工具：它不等于最终产品架构，但它为后续把 RTC 能力集成进主播端提供了可运行的基线。

### 5.1 构建目标与代码复用

- `apps/rtc_probe/`：`rtc_probe.exe`（controller 侧）
- `apps/rtc_guest_probe/`：`rtc_guest_probe.exe`（guest 侧）
- 公共实现：`apps/rtc_probe_common/`
  - `SignalProbeApp.*`：参数解析、WebSocket 信令、PeerConnection 生命周期编排
  - `SignalProbeSession.*`：业务信令阶段（stage1）状态机与超时/自动 hangup
  - `RtcAudioProbe.*`：RTC 媒体阶段（Opus RTP 音频收发与 WAV 落盘）

probe app 直接复用 `live_studio_core` 的能力：

- 麦克风采集：`capture/LiveDeviceCaptureSource`
- Opus 编解码：`encode/OpusAudioEncoder`、`decode/OpusAudioDecoder`
- WAV 落盘：`common/WavFileWriter`

### 5.2 信令层：WebSocket + JSON Envelope

`SignalProbeApp.cpp` 中，probe 会连接默认信令地址（可配置）：

- 默认 `ws://127.0.0.1:8080/ws`
- 启动后会发送 `probe.register`，带 `room_id/peer_id/role/mode` 等字段

统一 envelope 形态（概念上）：

- `type`
- `room_id`
- `from_user_id`
- `to_user_id`
- `request_id`
- `payload`
- `ts_ms`

消息类型包含两类：

- **业务状态类**：`linkmic.*`、成员列表/加入/离开等（由 `SignalProbeSession` 处理）
- **RTC 建连类**：`rtc.offer` / `rtc.answer` / `rtc.ice-candidate`（由 `SignalProbeApp` 转换到 PeerConnection API）

### 5.3 `SignalProbeSession`：业务阶段状态机（stage1）

`SignalProbeSession` 负责“邀请/申请/接受/拒绝/结束”等业务流程的最小闭环，并提供：

- 本地是否已注册、目标是否在线
- 当前信令状态（`currentSignalState()`）
- 当前 requestId（`currentRequestId()`）与 active request 匹配
- requestTimeout / autoHangup 定时器
- 当“业务阶段达成某条件”时回调 `onRtcOfferRequested`，触发进入 RTC 建连阶段（controller 侧发起 offer）

它把“业务状态”与“RTC SDP/ICE”之间做了边界隔离：业务状态先闭环，再进入 RTC。

### 5.4 `SignalProbeApp`：PeerConnection 与 DataChannel/Track 编排

在 RTC 模式下，`SignalProbeApp` 的关键行为是：

- 创建 `rtc::PeerConnection`（根据参数选择 ICE server / relay-only）
- 监听 `onLocalDescription`：将本地 SDP（offer/answer）通过 WebSocket 转发
- 监听 `onLocalCandidate`：将 ICE candidate 转发
- 接收远端 `rtc.offer/rtc.answer/rtc.ice-candidate`：应用到 PeerConnection
- 创建/接收 DataChannel（label: `stage0`）并用于探测消息与完成信号
- 监听 `onTrack`：将 inbound track 交给 `RtcAudioProbe` 处理（音频 RTP）

### 5.5 `RtcAudioProbe`：Opus RTP 收发与 WAV 落盘

`RtcAudioProbe` 提供两套状态：

- **OutboundState**（发送麦克风）：
  - `LiveDeviceCaptureSource` 采集音频（按 `micCaptureConfigPath`）
  - `OpusAudioEncoder` 编码
  - 通过 libdatachannel Track 发送 RTP（payloadType 默认 111，含 profile 生成逻辑）
  - 记录首包、发送包数、丢帧等统计
- **InboundState**（接收远端音频）：
  - 收到 RTP → 提取 Opus payload（在实现文件中完成）
  - `OpusAudioDecoder` 解码为 PCM frame
  - `WavFileWriter` 写入到 `recvAudioWavPath`
  - 以“连续音频解码达到阈值”作为成功判定（用于 smoke test）

它还会在 DataChannel 上交换“完成信号”（例如双方媒体跑满 `mediaRunMs` 后结束场景），以便脚本化验收。

### 5.6 联调脚本：`scripts/rtc_mic_smoke_test.ps1`

该 PowerShell 脚本用于一次性跑通“双方发麦克风并互收音频”的自动验收：

- 启动 `rtc_guest_probe`，随后启动 `rtc_probe`
- 默认强制 `--relay-only`（更贴近 TURN 场景）
- 生成 `logs/rtc-mic-smoke-<timestamp>/`，收集 stdout/stderr 与两侧接收 WAV
- 判定失败条件包含：
  - 进程退出码非 0
  - stderr 非空
  - WAV 体积 \(\le 44\)（只写了 header，等价于无有效音频）

这类脚本的价值是：为后续 RTC 集成提供“可重复、可比对”的验收基线。

---

## 6. 技术方案总结（当前与下一步）

### 6.1 当前技术架构的主线

- **主播端推流（已可运行）**：Windows 实时采集 → FFmpeg 编码 → FLV → RTMP → SRS
- **连麦路线验证（probe）**：Go(WebSocket) 信令转发 → libdatachannel PeerConnection → Opus RTP 音频收发 → WAV 落盘验收
- **UI（可选）**：Qt Widgets panel 复用同一 `live_studio_core` 会话控制器

### 6.2 架构优势

- **边界清晰**：采集/编码/封装/推流是强边界模块；RTC/业务信令通过 probe 独立验证。
- **可观测性强**：`LiveSessionController` pipeline 内置队列深度、编码耗时分解、首包延迟等关键指标。
- **复用策略明确**：RTMP/FLV 输出链路继续保持 H.264 + AAC；RTC 音频建议 Opus（与 WebRTC 生态对齐）。

### 6.3 已知风险与工程落点（面向连麦集成）

`libdatachannel` 路线的核心风险并不在“能否建连”，而在“媒体工程细节”：

- RTP packetize/depacketize（H.264/Opus）
- 多路远端媒体的解码与同步（时钟、抖动、丢包）
- 本地节目混流（视频布局、音频混音）与回声控制
- 状态机与稳定性（ICE 失败、断线重连、重协商策略）

仓库文档 `docs/guest_link_mic_plan_libdatachannel.md` 已给出分阶段实施建议（先信令闭环，再 DataChannel，再单向视频，再双向视频，再音频，最后节目混流）。

---

## 7. 推荐的工程化扩展点（与现有代码对齐）

以下建议以“不推翻现有 `live_studio_core`”为原则，给出与当前仓库最贴合的扩展组织方式：

### 7.1 新增模块建议（来自连麦方案文档并结合现状）

- **`src/linkmic/`**：业务态（成员目录、邀请/申请状态机、房间状态同步）
- **`src/rtc/`**：PeerConnection 封装、SDP/ICE 桥接、TURN 配置注入
- **`src/rtc/packet/`**：H264/Opus RTP 打包与解包
- **`src/decode/`**：视频解码器（H.264）补齐，音频解码器继续扩展
- **`src/mix/`**：节目混流（视频布局策略、音频混音策略、同步）

### 7.2 与 `LiveSessionController` 的集成策略

建议保持两条链路并行但共享底座：

- **节目输出链路**（给观众）：仍走 `VideoEncoder(H.264) + AudioEncoder(AAC) -> FlvMuxer -> RtmpPublisher`
- **连麦互动链路**（给上麦端）：走 `H.264 + Opus`，并围绕 `libdatachannel` 组织 Track 与 RTP

最终在主播端做“节目混流输入层”，把：

- 本地主播采集（原始帧）
- 多路远端参与者解码后的原始帧

统一喂给节目混流器，输出一路节目帧进入现有 RTMP 链路。

---

## 8. 快速索引（读代码从哪里开始）

- **Stage-1 主链路说明**：`README.md`
- **核心会话控制**：`src/session/LiveSessionController.hpp`、`src/session/LiveSessionControllerPipeline.cpp`
- **设备采集**：`src/capture/LiveDeviceCaptureSource.hpp/.cpp`
- **编码**：`src/encode/VideoEncoder.*`、`src/encode/AudioEncoder.*`
- **封装**：`src/mux/FlvMuxer.hpp`
- **推流**：`src/publish/RtmpPublisher.hpp/.cpp`
- **RTC probe 入口**：`apps/rtc_probe_common/SignalProbeApp.cpp`
- **RTC 业务态**：`apps/rtc_probe_common/SignalProbeSession.hpp/.cpp`
- **RTC 音频探测**：`apps/rtc_probe_common/RtcAudioProbe.hpp/.cpp`
- **RTC smoke 脚本**：`scripts/rtc_mic_smoke_test.ps1`
