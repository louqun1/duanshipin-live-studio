#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "capture/LiveDeviceCaptureSource.hpp"
#include "common/MediaTypes.hpp"
#include "common/Status.hpp"
#include "common/TraceLog.hpp"

namespace live_studio::session {

struct LiveSessionConfig {
    media::LiveDeviceCaptureConfig capture;
    std::string rtmpUrl;
    int videoBitrate = 2000000;
    int audioBitrate = 128000;
};

struct AudioLevelSnapshot {
    bool available = false;
    int frameIndex = -1;
    std::int64_t ptsMs = 0;
    double peak = 0.0;
    double rms = 0.0;
};

struct PreviewFrameSnapshot {
    bool available = false;
    int frameIndex = -1;
    std::int64_t ptsMs = 0;
    media::VideoFrame frame;
};

struct SessionDiagnostics {
    bool opened = false;
    bool previewRunning = false;
    bool publishing = false;
    bool audioEnabled = false;
    int previewedVideoFrames = 0;
    int previewedAudioFrames = 0;
    int publishedVideoFrames = 0;
    int publishedAudioUnits = 0;
    bool firstMediaPacketSent = false;
    double firstMediaLatencyMs = 0.0;
    std::string selectedVideoDeviceName;
    std::string selectedAudioDeviceName;
    std::string lastPublishedSummary;
    std::string lastError;
    capture::LiveDeviceProbeResult probe;
};

class LiveSessionController {
public:
    struct ControllerState;

    LiveSessionController();
    ~LiveSessionController();

    LiveSessionController(const LiveSessionController&) = delete;
    LiveSessionController& operator=(const LiveSessionController&) = delete;

    [[nodiscard]] std::string moduleName() const {
        return "LiveSessionController";
    }

    Status open(const LiveSessionConfig& config);
    Status openSingleVideoStage(const LiveSessionConfig& config);
    Status startPreview();
    Status startPublish();
    Status stopPublish();
    void close() noexcept;

    [[nodiscard]] PreviewFrameSnapshot latestPreviewFrame() const;
    [[nodiscard]] AudioLevelSnapshot latestAudioLevel() const;
    [[nodiscard]] SessionDiagnostics diagnostics() const;

    Status runStage1VideoTrace(const std::string& rtmpUrl,
                               const std::string& captureConfigPath,
                               int frameCountOverride,
                               TraceLog* trace);

private:
    std::unique_ptr<ControllerState> state_;
};

}  // namespace live_studio::session
