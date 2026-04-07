# Stage 1 Report

## A. Goal

Start stage 1 without skipping directly to a black-box live publisher.
This milestone makes the stage 1 video-only chain executable as a trace pipeline.

## B. Scope Implemented

Implemented:

- stage 1 video chain data objects
- a trace clock for video `pts/dts`
- a synthetic capture source hook
- trace-only encoder, FLV mux, and RTMP publish planning
- a runnable `stage1_video_trace` console app
- stage 1 dataflow documentation

Not implemented yet:

- real camera capture
- real H.264 bytes
- real FLV bytes
- real RTMP network transport
- SRS end-to-end verification
- Qt viewer first-frame verification

## C. Core Dataflow

Current runnable trace:

`VideoCaptureSource -> AvTimeline -> VideoEncoder -> FlvMuxer -> RtmpPublisher`

Target stage 1 chain remains:

`camera -> H.264 -> FLV -> RTMP -> SRS -> HTTP-FLV -> Qt viewer`

## D. Files Changed

Added:

- `apps/stage1_video_trace/main.cpp`
- `apps/stage1_video_trace/CMakeLists.txt`
- `src/common/TraceLog.hpp`
- `src/common/MediaTypes.hpp`
- `src/timeline/AvTimeline.hpp`
- `scripts/build_stage1_video_trace.ps1`
- `docs/stage_01_dataflow.md`
- `docs/stage_01_report.md`

Updated:

- `CMakeLists.txt`
- `src/capture/VideoCaptureSource.hpp`
- `src/encode/VideoEncoder.hpp`
- `src/mux/FlvMuxer.hpp`
- `src/publish/RtmpPublisher.hpp`
- `src/session/LiveSessionController.hpp`

## E. How To Run

```powershell
cmake -S . -B build
cmake --build build --target stage1_video_trace --config Debug
.\build\apps\stage1_video_trace\stage1_video_trace.exe
```

Fallback:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_stage1_video_trace.ps1
```

Optional custom arguments:

```powershell
.\build\apps\stage1_video_trace\stage1_video_trace.exe rtmp://127.0.0.1/live/demo 8
```

## F. How To Verify

Check that the console trace clearly shows:

- session bootstrap
- capture configuration
- timeline-generated frame index and `pts`
- encoder sequence header plan
- FLV tag planning
- RTMP message planning
- orderly teardown

Expected result:

- the app exits with code `0`
- the logs show one sequence header and multiple video frame plans
- no real socket or camera is required in this milestone

## G. Known Gaps

- this is not a stage 1 pass yet
- there is no real media payload
- there is no real RTMP handshake or socket send path
- there is no SRS or viewer verification log yet

## H. Recommendation

Do not move to stage 2 yet.
The next step should still stay inside stage 1:

1. replace synthetic capture with a controllable file-backed or camera-backed source
2. replace trace encoder output with real H.264 access units
3. replace RTMP message plans with real handshake and send code
4. verify `RTMP -> SRS -> HTTP-FLV -> Qt viewer` end to end
