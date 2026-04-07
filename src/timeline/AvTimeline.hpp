#pragma once

#include <cstdint>

#include "common/Status.hpp"

namespace live_studio::timeline {

struct VideoTick {
    int frameIndex = 0;
    std::int64_t ptsMs = 0;
    std::int64_t dtsMs = 0;
};

class AvTimeline {
public:
    [[nodiscard]] Status configureVideoClock(int fps) {
        if (fps <= 0) {
            return Status::InvalidArgument("fps must be positive");
        }
        fps_ = fps;
        nextFrameIndex_ = 0;
        elapsedUs_ = 0;
        baseFrameDurationUs_ = 1000000 / fps_;
        remainderUs_ = 1000000 % fps_;
        remainderBucket_ = 0;
        return Status::Ok();
    }

    [[nodiscard]] Status nextVideoTick(VideoTick* outTick) {
        if (outTick == nullptr) {
            return Status::InvalidArgument("outTick is null");
        }
        if (fps_ <= 0) {
            return Status::InvalidArgument("video clock is not configured");
        }

        outTick->frameIndex = nextFrameIndex_;
        outTick->ptsMs = elapsedUs_ / 1000;
        outTick->dtsMs = outTick->ptsMs;

        ++nextFrameIndex_;
        elapsedUs_ += baseFrameDurationUs_;
        remainderBucket_ += remainderUs_;
        if (remainderBucket_ >= fps_) {
            elapsedUs_ += remainderBucket_ / fps_;
            remainderBucket_ %= fps_;
        }
        return Status::Ok();
    }

private:
    int fps_ = 0;
    int nextFrameIndex_ = 0;
    std::int64_t elapsedUs_ = 0;
    std::int64_t baseFrameDurationUs_ = 0;
    std::int64_t remainderUs_ = 0;
    std::int64_t remainderBucket_ = 0;
};

}  // namespace live_studio::timeline
