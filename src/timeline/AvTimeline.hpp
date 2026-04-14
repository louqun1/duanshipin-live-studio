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
        elapsedUs_ += baseFrameDurationUs_;//这是因为1000000 / fps_可能不是整数，所以每帧的实际持续时间可能会有一个小数部分，remainderUs_就是这个小数部分的微秒数。通过将remainderUs_累积到remainderBucket_中，当remainderBucket_达到或超过fps_时，就说明已经累积了足够的微秒数来补偿一个完整的帧时间，这时就将elapsedUs_增加一个帧时间，并将remainderBucket_重置为余数。这种方式可以确保即使baseFrameDurationUs_不是整数，也能保持整体时间的准确性，避免随着时间推移产生较大的误差。
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
