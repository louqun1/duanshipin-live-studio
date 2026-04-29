#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/MediaTypes.hpp"
#include "common/Status.hpp"

namespace live_studio::encode {

struct EncodedOpusFrame {
    int frameIndex = 0;
    int sampleRate = 48000;
    int channels = 0;
    int samplesPerChannel = 0;
    std::int64_t ptsSamples = 0;
    std::int64_t dtsSamples = 0;
    std::string payloadSummary;
    std::vector<std::uint8_t> payload;
};

class OpusAudioEncoder {
public:
    OpusAudioEncoder();
    ~OpusAudioEncoder();

    OpusAudioEncoder(const OpusAudioEncoder&) = delete;
    OpusAudioEncoder& operator=(const OpusAudioEncoder&) = delete;

    [[nodiscard]] std::string moduleName() const {
        return "OpusAudioEncoder";
    }

    Status configure(int inputSampleRate, int inputChannels, int bitrate);
    Status encodeFrame(const media::AudioFrame& frame, std::vector<EncodedOpusFrame>* outFrames);
    Status flush(std::vector<EncodedOpusFrame>* outFrames);

    [[nodiscard]] int outputSampleRate() const noexcept {
        return outputSampleRate_;
    }

    [[nodiscard]] int channels() const noexcept {
        return channels_;
    }

private:
    struct EncoderContext;

    Status encodeReadyFrames(std::vector<EncodedOpusFrame>* outFrames);
    Status drainPackets(int frameIndexHint,
                        int samplesPerChannelHint,
                        std::vector<EncodedOpusFrame>* outFrames);

    std::unique_ptr<EncoderContext> context_;
    int inputSampleRate_ = 0;
    int outputSampleRate_ = 48000;
    int channels_ = 0;
    int bitrate_ = 0;
    bool configured_ = false;
};

}  // namespace live_studio::encode
