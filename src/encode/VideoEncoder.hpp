#pragma once

#include <cstdint>
#include <string>

#include "common/MediaTypes.hpp"
#include "common/Status.hpp"

namespace live_studio::encode {

struct EncodedVideoPacketDescriptor {
    bool keyFrame = false;
    std::int64_t ptsMs = 0;
    std::int64_t dtsMs = 0;
};

class VideoEncoder {
public:
    [[nodiscard]] std::string moduleName() const {
        return "VideoEncoder";
    }

    Status configureH264(int width, int height, int fps, int bitrate) {
        if (width <= 0 || height <= 0 || fps <= 0 || bitrate <= 0) {
            return Status::InvalidArgument("encoder parameters must be positive");
        }
        width_ = width;
        height_ = height;
        fps_ = fps;
        bitrate_ = bitrate;
        configured_ = true;
        return Status::Ok();
    }

    Status buildSequenceHeader(media::H264ParameterSets* outSets) const {
        if (outSets == nullptr) {
            return Status::InvalidArgument("outSets is null");
        }
        if (!configured_) {
            return Status::InvalidArgument("encoder is not configured");
        }

        outSets->profile = "baseline";
        outSets->spsLabel =
            "synthetic-sps-" + std::to_string(width_) + "x" + std::to_string(height_) + "-fps" + std::to_string(fps_);
        outSets->ppsLabel = "synthetic-pps-bitrate-" + std::to_string(bitrate_);
        return Status::Ok();
    }

    Status encodeTraceFrame(const media::VideoFrame& frame, media::EncodedVideoAccessUnit* outUnit) const {
        if (outUnit == nullptr) {
            return Status::InvalidArgument("outUnit is null");
        }
        if (!configured_) {
            return Status::InvalidArgument("encoder is not configured");
        }
        if (frame.width != width_ || frame.height != height_ || frame.fps != fps_) {
            return Status::InvalidArgument("input frame does not match encoder configuration");
        }

        outUnit->frameIndex = frame.frameIndex;
        outUnit->keyFrame = (frame.frameIndex % fps_) == 0;
        outUnit->ptsMs = frame.ptsMs;
        outUnit->dtsMs = frame.ptsMs;
        outUnit->nalSummary = outUnit->keyFrame ? "idr+nalu(trace)" : "pframe+nalu(trace)";
        return Status::Ok();
    }

    Status pushRawFrame(std::int64_t ptsMs) {
        if (ptsMs < 0) {
            return Status::InvalidArgument("ptsMs must be >= 0");
        }
        return Status::NotImplemented(
            "TODO(user): replace encodeTraceFrame with a real H.264 frame-feed path");
    }

private:
    int width_ = 0;
    int height_ = 0;
    int fps_ = 0;
    int bitrate_ = 0;
    bool configured_ = false;
};

}  // namespace live_studio::encode
