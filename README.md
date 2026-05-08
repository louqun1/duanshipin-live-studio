# duanshipin-live-studio

`duanshipin-live-studio` is the broadcaster-side Windows project that works with `duanshipin-pingtai`.

## Current entry points

- `live_studio_panel`
  - Qt Widgets broadcaster panel
  - business login / live-room creation / business WebSocket / anchor heartbeat
  - local capture preview + RTMP publish
- `rtc_probe`
  - controller-side RTC probe based on `libdatachannel`
- `rtc_guest_probe`
  - guest-side RTC probe based on `libdatachannel`

The old standalone trace app has been removed from this repo.

## Core pipeline

Current media pipeline:

`LiveDeviceCaptureSource -> VideoEncoder(H.264) + AudioEncoder(AAC) -> RtmpPublisher`

Current business/panel flow:

`REST login -> REST create live room -> QWebSocket join/heartbeat -> start RTMP publish`

## Build

Recommended Qt/clang build:

```powershell
cmake -S . -B build-qt-6103-llvm -G "MinGW Makefiles" `
  -DLIVE_STUDIO_BUILD_RTC_PROBES=ON `
  -DLIVE_STUDIO_QT_ROOT=D:/itffmpeg/Qt/6.10.3/llvm-mingw_64 `
  -DCMAKE_MAKE_PROGRAM=D:/itffmpeg/Qt/Tools/llvm-mingw1706_64/bin/mingw32-make.exe `
  -DCMAKE_C_COMPILER=D:/itffmpeg/Qt/Tools/llvm-mingw1706_64/bin/x86_64-w64-mingw32-clang.exe `
  -DCMAKE_CXX_COMPILER=D:/itffmpeg/Qt/Tools/llvm-mingw1706_64/bin/x86_64-w64-mingw32-clang++.exe
```

Build the panel:

```powershell
cmake --build build-qt-6103-llvm --target live_studio_panel -j 4
```

Build the probes:

```powershell
cmake --build build-qt-6103-llvm --target rtc_probe rtc_guest_probe -j 4
```

Or use the helper script for probes:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\configure_rtc_probes.ps1 `
  -BuildDir .\build-qt-6103-llvm `
  -CCompiler "D:\itffmpeg\Qt\Tools\llvm-mingw1706_64\bin\x86_64-w64-mingw32-clang.exe" `
  -CxxCompiler "D:\itffmpeg\Qt\Tools\llvm-mingw1706_64\bin\x86_64-w64-mingw32-clang++.exe" `
  -Build
```

## Run

Run the panel:

```powershell
.\build-qt-6103-llvm\apps\live_studio_panel\live_studio_panel.exe
```

Run the probes:

```powershell
.\build-qt-6103-llvm\apps\rtc_guest_probe\rtc_guest_probe.exe --signaling-url ws://192.168.3.28:8080/ws --room-id stage0 --ice-server turn://probe:probe_turn_123@192.168.3.28:3478?transport=udp --relay-only --timeout-ms 30000

.\build-qt-6103-llvm\apps\rtc_probe\rtc_probe.exe --signaling-url ws://192.168.3.28:8080/ws --room-id stage0 --ice-server turn://probe:probe_turn_123@192.168.3.28:3478?transport=udp --relay-only --timeout-ms 30000
```

## Pointers

- `src/session/LiveSessionController.*`
  - capture / preview / publish session orchestration
- `apps/live_studio_panel/`
  - business login, live-room, anchor presence, UI wiring
- `src/linkmic/`
  - Qt WebSocket-based signaling client and link-mic room state
- `apps/rtc_probe_common/`
  - shared RTC probe signaling and audio probe logic
- `docs/guest_link_mic_plan_libdatachannel.md`
  - current centered `libdatachannel + coturn + SRS` plan
- `docs/guest_link_mic_stage0_checklist.md`
  - current RTC probe checklist
