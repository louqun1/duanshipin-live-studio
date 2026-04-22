# duanshipin-live-studio

`duanshipin-live-studio` is the broadcaster-side project that runs alongside `duanshipin-pingtai`.

## Current milestone

- `stage1_video_trace` now targets Windows live-device capture instead of media-file input.
- Camera capture now goes through `Media Foundation`.
- Microphone capture now goes through `WASAPI/MMDevice`.
- Video is encoded with FFmpeg H.264 and audio is encoded with FFmpeg AAC.
- `FlvMuxer` emits real FLV video/audio payloads and tags.
- `RtmpPublisher` performs real RTMP handshake, metadata send, chunking, and media publish.
- FFmpeg 6 remains vendored under `third_party/ffmpeg6.0`.

Current stage-1 pipeline:

`LiveDeviceCaptureSource(Media Foundation + WASAPI) -> VideoEncoder(H.264) + AudioEncoder(AAC) -> FlvMuxer(FLV tags) -> RtmpPublisher(RTMP publish)`

Media-file input has been removed from `stage1_video_trace`. If offline testing is needed later, it should move to dedicated raw `yuv + pcm` tooling instead of the live-device entry.

## Default live capture profile

The default capture profile lives at:

`apps/stage1_video_trace/default_live_devices.txt`

Example:

```text
source_label=live-device-demo-0
video_device=default
audio_device=default
width=640
height=480
fps=30
sample_rate=48000
channels=2
frame_count=300
```

Notes:

- `video_device=default` picks the first available camera via Media Foundation.
- `audio_device=default` picks the default capture endpoint via WASAPI/MMDevice.
- You can also set friendly names explicitly when you want a specific camera or microphone.
- Leave `audio_device` empty to run camera-only.

## Build and run

```powershell
cmake -S . -B build
cmake --build build --target stage1_video_trace
.\build\apps\stage1_video_trace\stage1_video_trace.exe
```

Override RTMP URL, capture profile, or target video-frame count:

```powershell
.\build\apps\stage1_video_trace\stage1_video_trace.exe rtmp://127.0.0.1/live/demo
.\build\apps\stage1_video_trace\stage1_video_trace.exe rtmp://127.0.0.1/live/demo .\apps\stage1_video_trace\default_live_devices.txt
.\build\apps\stage1_video_trace\stage1_video_trace.exe rtmp://127.0.0.1/live/demo .\apps\stage1_video_trace\default_live_devices.txt 150
```

If the second argument is a plain integer, the app treats it as a `frame_count` override and still uses the default live capture profile.

## Minimal Qt publish panel

The repo now includes a thin Qt Widgets panel target:

`live_studio_panel`

Current UI scope:

- RTMP URL input
- video-device dropdown
- audio-device dropdown
- resolution / fps / sample-rate selectors
- start / stop publish buttons
- local preview surface
- log window
- live status bar with `idle / starting / publishing / stopping / failed`, video fps, audio packet rate, publish duration, first-packet latency, and latest error

Because the available Qt package is `llvm-mingw`, the existing GCC `build/` directory skips this target by default. Use a dedicated Qt build directory with the matching clang toolchain:

```powershell
cmake -S . -B build-qt -G "MinGW Makefiles" `
  -D CMAKE_BUILD_TYPE=Debug `
  -D CMAKE_C_COMPILER=D:/itffmpeg/Qt/Tools/llvm-mingw1706_64/bin/clang.exe `
  -D CMAKE_CXX_COMPILER=D:/itffmpeg/Qt/Tools/llvm-mingw1706_64/bin/clang++.exe `
  -D LIVE_STUDIO_QT_ROOT=D:/itffmpeg/Qt/6.10.2/llvm-mingw_64

cmake --build build-qt --target live_studio_panel
.\build-qt\apps\live_studio_panel\live_studio_panel.exe
```

`windeployqt` is run as a post-build step for `live_studio_panel`, so the deployed executable directory is directly runnable after the build finishes.

## Verification status

Verified on the current workstation:

- `stage1_video_trace` builds successfully with the current MinGW + vendored FFmpeg toolchain.
- A local fake-server smoke test succeeded with:
  - `video_device=default`
  - `audio_device=default`
  - `640x480 @ 30fps`
- The fake server observed:
  - RTMP `connect -> createStream -> publish`
  - `@setDataFrame/onMetaData`
  - video sequence header
  - AAC sequence header
  - H.264 video frames
  - AAC audio frames
- A real SRS publish also succeeded with the Windows-native live-device path:
  - `publisher entered publishing state for rtmp://192.168.99.128:1935/live/livestream`
  - video and audio sequence headers were sent
  - media ran through `video frame#299`
  - the session ended with `stage1 trace finished cleanly`
- Device-selection logs now distinguish requested names from selected names.
- If a requested camera or microphone is not found, the error now includes the offered friendly-name list.

## Pointers

- `src/capture/LiveDeviceCaptureSource.hpp`
  - Media Foundation camera capture, WASAPI microphone capture, and unified event output
- `src/encode/VideoEncoder.hpp`
  - FFmpeg H.264 encoder output
- `src/encode/AudioEncoder.hpp`
  - FFmpeg AAC encoder output
- `src/mux/FlvMuxer.hpp`
  - FLV video/audio payload and tag assembly
- `src/publish/RtmpPublisher.hpp`
  - RTMP transport for stage-1 publish
- `docs/stage.md`
  - stage document for the current runnable chain
- `docs/guest_link_mic_plan.md`
  - backup staged implementation document for the `LiveKit + SRS` route
- `docs/guest_link_mic_plan_libdatachannel.md`
  - current staged implementation document for the centered `libdatachannel + coturn + SRS` route
- `docs/guest_link_mic_stage0_checklist.md`
  - executable stage-0 checklist for the current libdatachannel-based route

## Next step

1. Add an explicit device-list mode so camera and microphone names can be copied without relying on failure logs.
2. Add reconnect and publish-failure recovery around the live RTMP session.
3. Add dedicated offline raw `yuv + pcm` test tooling if longer deterministic tests are needed.
