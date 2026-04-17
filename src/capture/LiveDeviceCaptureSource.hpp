#pragma once

#include <memory>
#include <string>

#include "common/MediaTypes.hpp"
#include "common/Status.hpp"

namespace live_studio::capture {

struct LiveDeviceContext;

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
    Status start();
    Status readNext(CaptureEvent* outEvent);
    void stop() noexcept;

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
