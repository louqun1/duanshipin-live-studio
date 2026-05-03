#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/MediaTypes.hpp"
#include "common/Status.hpp"

namespace live_studio::decode {

class OpusAudioDecoder {
public:
    OpusAudioDecoder();
    ~OpusAudioDecoder();

    OpusAudioDecoder(const OpusAudioDecoder&) = delete;
    OpusAudioDecoder& operator=(const OpusAudioDecoder&) = delete;

    [[nodiscard]] std::string moduleName() const {
        return "OpusAudioDecoder";
    }

    Status configure(int expectedSampleRate, int expectedChannels);
    Status decodePacket(const std::uint8_t* data,
                        std::size_t size,
                        std::int64_t ptsSamplesHint,
                        std::vector<media::AudioFrame>* outFrames);
    Status flush(std::vector<media::AudioFrame>* outFrames);

private:
    struct DecoderContext;

    Status drainFrames(std::int64_t ptsSamplesHint, std::vector<media::AudioFrame>* outFrames);

    std::unique_ptr<DecoderContext> context_;
    int expectedSampleRate_ = 0;
    int expectedChannels_ = 0;
    bool configured_ = false;
};

}  // namespace live_studio::decode
