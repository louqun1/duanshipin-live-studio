#pragma once

#include <cstdint>
#include <string>

#include "common/MediaTypes.hpp"
#include "common/Status.hpp"

namespace live_studio::capture {

struct VideoFrameDescriptor {
    int width = 0;
    int height = 0;
    int fps = 0;
    std::int64_t ptsMs = 0;
};

class VideoCaptureSource {
public:
    [[nodiscard]] std::string moduleName() const {
        return "VideoCaptureSource";
    }

    Status configureCamera(const std::string& deviceId, int width, int height, int fps) {
        if (deviceId.empty()) {
            return Status::InvalidArgument("deviceId is empty");
        }
        if (width <= 0 || height <= 0 || fps <= 0) {
            return Status::InvalidArgument("camera parameters must be positive");
        }
        return Status::NotImplemented(
            "TODO(user): in stage 1, connect a real camera or a controllable file-backed fake source");
    }

    Status configureSyntheticSource(int width, int height, int fps, int frameCount) {
        if (width <= 0 || height <= 0 || fps <= 0 || frameCount <= 0) {
            return Status::InvalidArgument("synthetic source parameters must be positive");
        }
        config_.sourceLabel = "synthetic-camera-0";
        config_.width = width;
        config_.height = height;
        config_.fps = fps;
        config_.frameCount = frameCount;
        return Status::Ok();
    }

    Status start() {
        if (config_.fps <= 0) {
            return Status::NotImplemented(
                "TODO(user): configure a real or synthetic video source before start");
        }
        started_ = true;
        return Status::Ok();
    }

    Status captureSyntheticFrame(int frameIndex, std::int64_t ptsMs, media::VideoFrame* outFrame) const {
        if (outFrame == nullptr) {
            return Status::InvalidArgument("outFrame is null");
        }
        if (!started_) {
            return Status::InvalidArgument("capture source is not started");
        }
        if (frameIndex < 0 || ptsMs < 0) {
            return Status::InvalidArgument("frameIndex and ptsMs must be >= 0");
        }
        if (config_.frameCount > 0 && frameIndex >= config_.frameCount) {
            return Status::InvalidArgument("frameIndex exceeds configured synthetic frame count");
        }

        outFrame->sourceLabel = config_.sourceLabel;
        outFrame->frameIndex = frameIndex;
        outFrame->width = config_.width;
        outFrame->height = config_.height;
        outFrame->fps = config_.fps;
        outFrame->ptsMs = ptsMs;
        outFrame->synthetic = true;
        return Status::Ok();
    }

    void stop() noexcept {
        started_ = false;
    }

    [[nodiscard]] const media::VideoCaptureConfig& config() const noexcept {
        return config_;
    }

private:
    media::VideoCaptureConfig config_;
    bool started_ = false;
};

}  // namespace live_studio::capture
