#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/MediaTypes.hpp"
#include "common/Status.hpp"

namespace live_studio::capture {

struct LiveDeviceContext;

struct LiveDeviceProbeResult {
    std::vector<std::string> availableVideoDeviceNames;
    std::vector<std::string> availableAudioDeviceNames;
    std::string selectedVideoDeviceName;
    std::string selectedAudioDeviceName;
    int negotiatedVideoWidth = 0;
    int negotiatedVideoHeight = 0;
    int negotiatedVideoFps = 0;
    int negotiatedSampleRate = 0;
    int negotiatedChannels = 0;
};

Status loadLiveDeviceCaptureConfig(const std::string& configPath, media::LiveDeviceCaptureConfig* outConfig);
Status probeLiveDeviceCaptureConfig(const media::LiveDeviceCaptureConfig& config, LiveDeviceProbeResult* outProbe);
Status enumerateLiveDeviceCaptureDevices(LiveDeviceProbeResult* outProbe);

enum class CaptureEventKind {
    video,
    audio,
};

struct CaptureEvent {
    CaptureEventKind kind = CaptureEventKind::video;
    media::VideoFrame videoFrame;
    media::AudioFrame audioFrame;
};

class LiveDeviceCaptureSource {
public:
    LiveDeviceCaptureSource();
    ~LiveDeviceCaptureSource();

    LiveDeviceCaptureSource(const LiveDeviceCaptureSource&) = delete;
    LiveDeviceCaptureSource& operator=(const LiveDeviceCaptureSource&) = delete;

    [[nodiscard]] std::string moduleName() const {
        return "LiveDeviceCaptureSource";
    }

    Status configureDeviceSource(const std::string& configPath);
    Status configureDeviceSource(const media::LiveDeviceCaptureConfig& config);
    Status start();
    Status readNext(CaptureEvent* outEvent);
    void stop() noexcept;
    [[nodiscard]] int pendingEventCount() const noexcept;

    [[nodiscard]] const media::LiveDeviceCaptureConfig& config() const noexcept {
        return config_;
    }

    [[nodiscard]] bool hasAudio() const noexcept {
        return !config_.audioDeviceId.empty();
    }

private:
    media::LiveDeviceCaptureConfig config_;
    bool started_ = false;
    std::unique_ptr<LiveDeviceContext> context_;
};

}  // namespace live_studio::capture
