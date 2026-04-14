#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "common/MediaTypes.hpp"
#include "common/Status.hpp"

namespace live_studio::encode {

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
    Status encodeFrame(const media::VideoFrame& frame, media::EncodedVideoAccessUnit* outUnit);

    Status encodeTraceFrame(const media::VideoFrame& frame, media::EncodedVideoAccessUnit* outUnit) {
        return encodeFrame(frame, outUnit);
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
