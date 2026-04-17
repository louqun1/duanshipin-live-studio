# duanshipin-live-studio stage notes

This document tracks the code path that is actually implemented in the repository today.

## Current boundary

The current runnable entry is:

`apps/stage1_video_trace/main.cpp`

The current stage-1 chain is:

`main -> initializeDefaultLogger -> LiveSessionController::runStage1VideoTrace -> LiveDeviceCaptureSource(Media Foundation + WASAPI) -> VideoEncoder(H.264) + AudioEncoder(AAC) -> FlvMuxer -> RtmpPublisher`

Current state:

- `stage1_video_trace` no longer uses media-file input.
- Camera capture is wired through `Media Foundation`.
- Microphone capture is wired through `WASAPI/MMDevice`.
- Video is encoded to real H.264 access units.
- Audio is encoded to real AAC access units.
- FLV video and audio tags are built from real encoded bytes.
- RTMP connect / createStream / publish / metadata / chunked media send are real.

## Entry and arguments

`main(int argc, char** argv)` in `apps/stage1_video_trace/main.cpp` currently:

1. Resolves `projectRoot`.
2. Uses `apps/stage1_video_trace/default_live_devices.txt` as the default live profile.
3. Parses:
   - `rtmpUrl`
   - `captureConfigPath`
   - `frameCountOverride`
4. Initializes the logger.
5. Calls `LiveSessionController::runStage1VideoTrace(...)`.

## Live session controller

`src/session/LiveSessionController.hpp`

`runStage1VideoTrace(...)` currently orchestrates:

1. `capture.configureDeviceSource(captureConfigPath)`
2. `capture.start()`
3. `videoEncoder.configureH264(...)`
4. optional `audioEncoder.configureAac(...)`
5. `publisher.configureVideoStream(...)`
6. optional `publisher.configureAudioStream(...)`
7. `publisher.connect(rtmpUrl)`
8. publish H.264 sequence header
9. optional publish AAC sequence header
10. loop over live capture events until the target video-frame count is reached
11. flush pending AAC packets
12. `capture.stop()` and `publisher.disconnect()`

The capture loop is event-driven:

- video events go through `VideoEncoder -> FlvMuxer::buildVideoNaluTag -> RtmpPublisher`
- audio events go through `AudioEncoder -> FlvMuxer::buildAudioRawTag -> RtmpPublisher`

## Live device capture

`src/capture/LiveDeviceCaptureSource.hpp`
`src/capture/LiveDeviceCaptureSource.cpp`

Implemented pieces:

- text profile parsing for:
  - `source_label`
  - `video_device`
  - `audio_device`
  - `width`
  - `height`
  - `fps`
  - `sample_rate`
  - `channels`
  - `frame_count`
- camera selection through Media Foundation device enumeration
- microphone selection through WASAPI/MMDevice endpoint enumeration
- Media Foundation sample conversion into raw YUV420P frames
- WASAPI packet conversion into interleaved S16 PCM frames
- capture threads for audio/video and a shared event queue
- event output as:
  - `CaptureEventKind::video`
  - `CaptureEventKind::audio`

Current selection behavior:

- `video_device=default` picks the first available Media Foundation camera
- `audio_device=default` picks the default WASAPI capture endpoint
- explicit friendly names are also supported

## Encoders

`src/encode/VideoEncoder.cpp`

- real FFmpeg H.264 encoder
- extracts SPS/PPS from encoder extradata
- returns `media::EncodedVideoAccessUnit`

`src/encode/AudioEncoder.cpp`

- real FFmpeg AAC encoder
- converts interleaved S16 PCM into the encoder sample format
- buffers samples with `AVAudioFifo`
- normalizes AAC packet timestamps so FLV/RTMP timestamps stay non-negative
- returns zero or more `media::EncodedAudioAccessUnit`
- exposes AAC sequence-header config from encoder extradata

## FLV mux

`src/mux/FlvMuxer.hpp`

Implemented:

- AVC sequence header
- AVC NALU video tag
- AAC sequence header
- AAC raw audio tag
- real FLV tag header bytes for both tag type `9` and tag type `8`

## RTMP publisher

`src/publish/RtmpPublisher.cpp`

Implemented:

- RTMP URL parse
- TCP connect
- RTMP simple handshake
- `connect`
- `createStream`
- `publish`
- `Set Chunk Size`
- `@setDataFrame/onMetaData`
- video message mapping/publish
- audio message mapping/publish

Metadata carries video fields when video is configured and audio fields when audio is configured.

## Verification status

Verified in code/build:

- `stage1_video_trace` builds successfully on the current MinGW toolchain
- Media Foundation and WASAPI link cleanly with:
  - `mf`
  - `mfplat`
  - `mfreadwrite`
  - `mfuuid`
  - `mmdevapi`
  - `ole32`
  - `uuid`
  - `avrt`
  - `ksuser`
- FFmpeg-backed encode/mux/publish still builds successfully with:
  - `FFmpeg::avcodec`
  - `FFmpeg::avutil`
  - `FFmpeg::swscale`
  - `FFmpeg::swresample`

Verified at runtime on the current workstation:

- fake RTMP server smoke test succeeded with default camera + default microphone
- the server observed:
  - `connect`
  - `createStream`
  - `publish`
  - `@setDataFrame/onMetaData`
  - H.264 sequence header + video frames
  - AAC sequence header + audio frames
- real SRS publish also succeeded with camera + microphone:
  - entered publishing state for `rtmp://192.168.99.128:1935/live/livestream`
  - sent H.264 and AAC sequence headers
  - ran through `video frame#299`
  - finished with `stage1 trace finished cleanly`
- live-device logs now report:
  - requested camera/microphone names before start
  - selected camera/microphone friendly names after startup
  - offered device-name lists when lookup fails

## Removed from stage1 entry

Removed from `stage1_video_trace`:

- media-file input as the live source
- synthetic/file-backed stage-1 usage and docs
- DirectShow capture as the default camera/microphone path

If deterministic offline testing is needed later, it should use a separate raw `yuv + pcm` path rather than reusing the live-device entry.
