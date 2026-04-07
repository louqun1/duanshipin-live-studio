# Stage 1 Dataflow

## Goal

Stage 1 still targets the first live streaming main path:

`camera -> H.264 encode -> FLV video tag -> RTMP publish -> SRS -> HTTP-FLV -> Qt viewer`

This submission does not claim that the full path is complete yet.
It lands a runnable trace milestone so the module boundary is executable and reviewable before real media and network code are written.

## Current Trace Path

`synthetic VideoFrame -> EncodedVideoAccessUnit -> FlvVideoTagPlan -> RtmpMessagePlan`

The trace app runs this path through the same module names that the real stage 1 path will use:

- `VideoCaptureSource`
- `AvTimeline`
- `VideoEncoder`
- `FlvMuxer`
- `RtmpPublisher`
- `LiveSessionController`

## What Each Hop Means

### 1. `VideoCaptureSource`

Produces a synthetic frame descriptor for now.
The user still needs to handwrite real camera capture and raw frame delivery.

### 2. `AvTimeline`

Turns the configured fps into monotonic frame index and `pts/dts` values.
This is the first explicit place where the stage 1 code states how video time moves forward.

### 3. `VideoEncoder`

Turns the synthetic frame descriptor into a trace-only `EncodedVideoAccessUnit`.
It also synthesizes a placeholder SPS/PPS description so the mux step can show where the AVC sequence header comes from.

### 4. `FlvMuxer`

Maps encoder output into trace-only FLV video tag plans:

- one AVC sequence header
- one video tag per access unit

### 5. `RtmpPublisher`

Maps FLV tag plans into trace-only RTMP message plans.
No socket is opened in this milestone.
The user still needs to handwrite handshake, command messages, chunk layout, send pacing, and reconnect behavior.

### 6. `LiveSessionController`

Owns the orchestration order and emits readable logs for every hop.

## User-Handwrite TODO List

- real camera source selection and frame callback path
- raw frame layout ownership
- real H.264 encoder init and frame feed
- SPS/PPS extraction from encoder output
- real FLV AVC tag byte layout
- RTMP handshake and command message flow
- RTMP chunk/message serialization and network send loop
- SRS integration validation with the existing Qt HTTP-FLV viewer

## Verification Entry

Build and run:

```powershell
cmake -S . -B build
cmake --build build --target stage1_video_trace --config Debug
.\build\apps\stage1_video_trace\stage1_video_trace.exe
```

Fallback:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_stage1_video_trace.ps1
```
