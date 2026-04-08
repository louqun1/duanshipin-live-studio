# duanshipin-live-studio

`duanshipin-live-studio` is the broadcaster-side project that runs alongside `duanshipin-pingtai`.

Current milestone status:
- capture has moved from a hard-coded synthetic source to a controllable file-backed source
- `VideoEncoder` is still a trace placeholder
- `RtmpPublisher` is still a trace placeholder
- the old stage-0 `stage0_probe` entry has been removed

## Current runnable entry

The current executable entry is `stage1_video_trace`:

`file-backed VideoCaptureSource -> AvTimeline -> VideoEncoder(trace) -> FlvMuxer(trace) -> RtmpPublisher(trace)`

The default source profile lives at:

`apps/stage1_video_trace/default_video_source.txt`

It uses a simple `key=value` format:

```text
source_label=file-source-demo-0
width=1280
height=720
fps=25
frame_count=6
```

## Build and run

With CMake:

```powershell
cmake -S . -B build
cmake --build build --target stage1_video_trace
.\build\apps\stage1_video_trace\stage1_video_trace.exe
```

With the helper script:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_stage1_video_trace.ps1
```

You can also override the RTMP URL, source file, and frame count:

```powershell
.\build\apps\stage1_video_trace\stage1_video_trace.exe rtmp://127.0.0.1/live/demo
.\build\apps\stage1_video_trace\stage1_video_trace.exe rtmp://127.0.0.1/live/demo .\apps\stage1_video_trace\default_video_source.txt
.\build\apps\stage1_video_trace\stage1_video_trace.exe rtmp://127.0.0.1/live/demo .\apps\stage1_video_trace\default_video_source.txt 8
```

If the second argument is a plain integer, the app treats it as a `frame_count` override and still uses the default source file.

## Pointers

- `src/capture/VideoCaptureSource.hpp`
  - controllable file-backed source today, real camera backend later
- `src/encode/VideoEncoder.hpp`
  - trace H.264 placeholder output
- `src/publish/RtmpPublisher.hpp`
  - trace RTMP planning only, no real socket send yet
- `docs/stage_00_report.md`
  - kept as a historical report
- `docs/stage_01_report.md`
  - stage-1 incremental report

## Next step

The next stage-1 tasks remain:

1. replace the file-backed source with a real camera source, or make the file-backed source carry real raw frame payloads
2. replace the trace `VideoEncoder` with a real H.264 encoder path
3. replace the trace `RtmpPublisher` with real RTMP handshake and send logic
