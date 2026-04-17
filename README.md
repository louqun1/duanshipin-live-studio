# duanshipin-live-studio

`duanshipin-live-studio` is the broadcaster-side project that runs alongside `duanshipin-pingtai`.

Current milestone status:
- capture has moved from a hard-coded synthetic source to a controllable file-backed source
- the file-backed source now generates synthetic YUV420P raw frames
- `VideoEncoder` now runs a real FFmpeg H.264 encoder path
- `FlvMuxer` now emits real FLV video payload/tag bytes
- `RtmpPublisher` now performs real RTMP handshake, command exchange, chunking, and socket send
- FFmpeg 6 is vendored under `third_party/ffmpeg6.0`
- the old stage-0 `stage0_probe` entry has been removed

## Current runnable entry

The current executable entry is `stage1_video_trace`:

`file-backed VideoCaptureSource(raw YUV420P) -> AvTimeline -> VideoEncoder(FFmpeg H.264) -> FlvMuxer(real FLV video bytes) -> RtmpPublisher(real RTMP publish)`

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

The project now expects FFmpeg to live in:

`third_party/ffmpeg6.0`

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

For a local protocol smoke test without SRS:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\rtmp_fake_server_smoke_test.ps1
```

## Pointers

- `src/capture/VideoCaptureSource.hpp`
  - controllable file-backed source today, real camera backend later
  - currently synthesizes YUV420P payloads from the source profile
- `src/encode/VideoEncoder.hpp`
  - real FFmpeg-backed H.264 encoder output
- `src/publish/RtmpPublisher.hpp`
  - real RTMP transport for stage-1 video publish
- `docs/stage.md`
  - single evolving stage document from `main()` to the current publish boundary

## Next step

The next stage-1 tasks remain:

1. replace the synthetic file-backed raw frame generator with a real camera source or a true file payload reader
2. connect to SRS and verify the first viewer-side HTTP-FLV frame end to end
3. extend stage 1 beyond video-only into audio capture, AAC encode, and A/V interleave
