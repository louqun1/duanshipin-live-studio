#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "common/MediaTypes.hpp"
#include "common/Status.hpp"

namespace live_studio::decode {

class H264VideoDecoder {
public:
    H264VideoDecoder();
    ~H264VideoDecoder();

    H264VideoDecoder(const H264VideoDecoder&) = delete;
    H264VideoDecoder& operator=(const H264VideoDecoder&) = delete;

    Status configure();
    Status decodeAccessUnit(const std::uint8_t* data,
                            std::size_t size,
                            std::int64_t ptsMsHint,
                            std::vector<media::VideoFrame>* outFrames);
    Status flush(std::vector<media::VideoFrame>* outFrames);

private:
    struct DecoderContext;

    Status drainFrames(std::int64_t ptsMsHint, std::vector<media::VideoFrame>* outFrames);

    std::unique_ptr<DecoderContext> context_;
    bool configured_ = false;
};

}  // namespace live_studio::decode
