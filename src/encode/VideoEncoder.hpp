#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "common/MediaTypes.hpp"
#include "common/Status.hpp"

namespace live_studio::encode {

struct VideoEncodeTrace {
    double frameCopyDurationMs = 0.0;
    double sendFrameDurationMs = 0.0;
    double receivePacketDurationMs = 0.0;
    double packetFinalizeDurationMs = 0.0;
};

class VideoEncoder {
public:
    VideoEncoder();
    ~VideoEncoder();

    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    [[nodiscard]] std::string moduleName() const {
        return "VideoEncoder";
    }

    Status configureH264(int width, int height, int fps, int bitrate);
    Status buildSequenceHeader(media::H264ParameterSets* outSets) const;
    Status encodeFrame(const media::VideoFrame& frame,
                       media::EncodedVideoAccessUnit* outUnit,
                       VideoEncodeTrace* outTrace = nullptr);

    Status encodeTraceFrame(const media::VideoFrame& frame,
                            media::EncodedVideoAccessUnit* outUnit,
                            VideoEncodeTrace* outTrace = nullptr) {
        return encodeFrame(frame, outUnit, outTrace);
    }

    Status pushRawFrame(std::int64_t ptsMs);

private:
    struct EncoderContext;

    std::unique_ptr<EncoderContext> context_;
    int width_ = 0;
    int height_ = 0;
    int fps_ = 0;
    int bitrate_ = 0;
    bool configured_ = false;
};

}  // namespace live_studio::encode
