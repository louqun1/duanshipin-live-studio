#pragma once

#include <memory>
#include <string>

#include "capture/LiveDeviceCaptureSource.hpp"
#include "common/MediaTypes.hpp"
#include "common/Status.hpp"

namespace live_studio::capture {

struct DesktopCaptureContext;

class DesktopCaptureSource {
public:
    DesktopCaptureSource();
    ~DesktopCaptureSource();

    DesktopCaptureSource(const DesktopCaptureSource&) = delete;
    DesktopCaptureSource& operator=(const DesktopCaptureSource&) = delete;

    [[nodiscard]] std::string moduleName() const {
        return "DesktopCaptureSource";
    }

    Status configure(const media::DesktopCaptureConfig& config);
    Status start();
    Status readNext(CaptureEvent* outEvent);
    void stop() noexcept;
    [[nodiscard]] int pendingEventCount() const noexcept;

    [[nodiscard]] const media::DesktopCaptureConfig& config() const noexcept {
        return config_;
    }

    [[nodiscard]] bool hasAudio() const noexcept {
        return false;
    }

private:
    media::DesktopCaptureConfig config_;
    bool started_ = false;
    std::unique_ptr<DesktopCaptureContext> context_;
};

}  // namespace live_studio::capture