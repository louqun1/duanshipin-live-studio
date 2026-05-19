# duanshipin-live-studio

`duanshipin-live-studio` 是短视频平台的 Windows 主播端导播项目，配合 `duanshipin-pingtai` 使用。项目当前以 Qt Widgets 面板为主入口，负责主播登录、创建直播间、业务 WebSocket 心跳、本地采集预览、RTMP 推流，以及基于 `libdatachannel` 的连麦 RTC 能力验证与接入。

## 项目定位

- 主播端运行在 Windows 原生 C++ 环境。
- 直播节目流继续走本地采集、编码、封装、RTMP 推送到 SRS/CDN 的链路。
- 连麦互动走 `libdatachannel + 自建信令 + coturn`，主播端作为中心汇聚点。
- 第一阶段重点支持小规模连麦场景，优先打通主播与单个/少量连麦端之间的音视频互通。

## 当前能力

- Qt Widgets 主播面板：登录、创建直播间、开播、停播、状态展示。
- 业务链路：REST 登录、REST 创建直播间、QWebSocket 加入房间与主播心跳。
- 本地媒体链路：摄像头/麦克风采集、H.264 视频编码、AAC 音频编码、FLV 封装、RTMP 推流。
- RTC 探针：`rtc_probe` 与 `rtc_guest_probe` 用于验证信令、ICE、DataChannel 和基础媒体能力。
- 连麦接入：房间成员、邀请/申请、RTC 会话配置、远端 H.264/Opus 接收与解码。
- 远端播放：OpenGL YUV 视频预览组件与 SDL 音频输出组件。
- 调试能力：RTC 媒体 dump、日志目录、探针脚本和可移植包输出。

## 主要入口

- `apps/live_studio_panel/`
  - 主播端 Qt 面板。
  - 负责业务登录、直播间创建、WebSocket 业务信令、开播推流、连麦 UI 与 RTC 会话编排。
- `apps/rtc_probe/`
  - 主控侧 RTC 探针，基于 `libdatachannel`。
- `apps/rtc_guest_probe/`
  - 连麦端 RTC 探针，基于 `libdatachannel`。
- `apps/rtc_probe_common/`
  - 探针程序共享的信令、音频探测和会话逻辑。

旧的独立 trace 程序已经从仓库中移除。

## 核心链路

当前直播节目流：

```text
LiveDeviceCaptureSource
  -> VideoEncoder(H.264) + AudioEncoder(AAC)
  -> FlvMuxer
  -> RtmpPublisher
  -> SRS / CDN
```

当前业务面板流程：

```text
REST 登录
  -> REST 创建直播间
  -> QWebSocket 加入房间/主播心跳
  -> 启动本地采集预览
  -> 启动 RTMP 推流
```

当前连麦 RTC 流程：

```text
业务 WebSocket 连麦邀请/申请/接受
  -> 下发 RTC 会话配置和 ICE Server
  -> libdatachannel 建立 PeerConnection
  -> 交换 offer / answer / ICE candidate
  -> 发送/接收 H.264 视频与 Opus 音频
  -> 主播端本地预览、播放与后续节目合流
```

## 目录说明

- `src/session/`
  - 直播会话控制，串联采集、预览、编码、推流和状态回调。
- `src/capture/`
  - Windows 设备采集。
- `src/encode/`
  - H.264、AAC、Opus 等编码能力。
- `src/decode/`
  - Opus 音频与 H.264 视频解码。
- `src/publish/`
  - RTMP 推流。
- `src/linkmic/`
  - 连麦房间状态、成员目录、业务信令客户端和连麦会话。
- `src/rtc/`
  - `libdatachannel` RTC 会话、信令桥接、远端音视频 sink。
- `src/render/`
  - OpenGL 视频预览控件。
- `src/audio/`
  - SDL 音频输出。
- `scripts/`
  - 依赖配置、探针构建和连麦冒烟测试脚本。
- `docs/`
  - 连麦方案、RTC 流程和阶段检查清单。

## 构建准备

项目默认使用仓库内的 FFmpeg 6.0、SDL2 和 vendored `spdlog`，Qt 与 `libdatachannel` 需要按本机路径配置。

推荐环境：

- Windows
- CMake 3.20+
- Qt 6.10.3 llvm-mingw 版本
- Qt Tools 中的 `llvm-mingw1706_64`
- OpenSSL runtime
- 已构建好的 `third_party/libdatachannel-install-clang`

如果还没有构建 `libdatachannel`，可以先运行：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\setup_libdatachannel.ps1 `
  -OpenSslRootDir "D:\itffmpeg\x86_64-15.2.0-release-posix-seh-ucrt-rt_v13-rev1\mingw64\opt" `
  -CCompiler "D:\itffmpeg\Qt\Tools\llvm-mingw1706_64\bin\x86_64-w64-mingw32-clang.exe" `
  -CxxCompiler "D:\itffmpeg\Qt\Tools\llvm-mingw1706_64\bin\x86_64-w64-mingw32-clang++.exe"
```

## 构建

推荐使用 Qt/clang 构建目录：

```powershell
cmake -S . -B build-qt-6103-llvm -G "MinGW Makefiles" `
  -DLIVE_STUDIO_BUILD_QT_PANEL=ON `
  -DLIVE_STUDIO_BUILD_RTC_PROBES=ON `
  -DLIVE_STUDIO_QT_ROOT=D:/itffmpeg/Qt/6.10.3/llvm-mingw_64 `
  -DCMAKE_MAKE_PROGRAM=D:/itffmpeg/Qt/Tools/llvm-mingw1706_64/bin/mingw32-make.exe `
  -DCMAKE_C_COMPILER=D:/itffmpeg/Qt/Tools/llvm-mingw1706_64/bin/x86_64-w64-mingw32-clang.exe `
  -DCMAKE_CXX_COMPILER=D:/itffmpeg/Qt/Tools/llvm-mingw1706_64/bin/x86_64-w64-mingw32-clang++.exe
```

构建主播面板：

```powershell
cmake --build build-qt-6103-llvm --target live_studio_panel -j 4
```

构建 RTC 探针：

```powershell
cmake --build build-qt-6103-llvm --target rtc_probe rtc_guest_probe -j 4
```

也可以使用探针辅助脚本：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\configure_rtc_probes.ps1 `
  -BuildDir .\build-qt-6103-llvm `
  -CCompiler "D:\itffmpeg\Qt\Tools\llvm-mingw1706_64\bin\x86_64-w64-mingw32-clang.exe" `
  -CxxCompiler "D:\itffmpeg\Qt\Tools\llvm-mingw1706_64\bin\x86_64-w64-mingw32-clang++.exe" `
  -Build
```

## 运行

运行主播面板：

```powershell
.\build-qt-6103-llvm\apps\live_studio_panel\live_studio_panel.exe
```

运行连麦探针：

```powershell
.\build-qt-6103-llvm\apps\rtc_guest_probe\rtc_guest_probe.exe --signaling-url ws://192.168.3.28:8080/ws --room-id stage0 --ice-server turn://probe:probe_turn_123@192.168.3.28:3478?transport=udp --relay-only --timeout-ms 30000

.\build-qt-6103-llvm\apps\rtc_probe\rtc_probe.exe --signaling-url ws://192.168.3.28:8080/ws --room-id stage0 --ice-server turn://probe:probe_turn_123@192.168.3.28:3478?transport=udp --relay-only --timeout-ms 30000
```

运行前需要确认平台后端、信令 WebSocket、SRS、coturn 和 RTMP 地址已经按环境启动并配置正确。

## 打包

构建完成后，可以用构建产物覆盖 `dist/live_studio_panel-portable` 并重新生成 zip：

```powershell
robocopy `
  build-qt-6103-llvm\apps\live_studio_panel `
  dist\live_studio_panel-portable `
  /MIR `
  /XD CMakeFiles live_studio_panel_autogen `
  /XF Makefile cmake_install.cmake

if (Test-Path dist\live_studio_panel-portable.zip) {
    Remove-Item dist\live_studio_panel-portable.zip -Force
}

Compress-Archive `
  -Path dist\live_studio_panel-portable\* `
  -DestinationPath dist\live_studio_panel-portable.zip `
  -Force
```

## 文档索引

- `docs/technical_architecture.md`
  - 技术架构说明。
- `docs/LinkMic 信令与 RTC 流程.md`
  - 连麦业务信令与 RTC 建连流程。
- `docs/guest_link_mic_plan_libdatachannel.md`
  - 当前主线方案：`libdatachannel + coturn + SRS`。
- `docs/guest_link_mic_plan.md`
  - 备选方案：`LiveKit + SRS`。
- `docs/guest_link_mic_stage0_checklist.md`
  - 阶段 0 探针和依赖验证清单。

## 注意事项

- `build/`、`build-*`、`dist/` 和 `logs/` 默认不作为源码目录维护。
- `dist/live_studio_panel-portable.zip` 是可发布产物，只有确认需要更新时再提交。
- RTC 音频优先使用 Opus，RTMP 节目流继续使用 AAC。
- 第一版连麦以主播端中心汇聚为主，不做客户端全互连 mesh，也不让 SRS 参与 RTC 媒体交换。
