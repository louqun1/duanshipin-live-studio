#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/MediaTypes.hpp"
#include "common/Status.hpp"

namespace live_studio::encode {

class AudioEncoder {
public:
    AudioEncoder();
    ~AudioEncoder();

    AudioEncoder(const AudioEncoder&) = delete;
    AudioEncoder& operator=(const AudioEncoder&) = delete;

    [[nodiscard]] std::string moduleName() const {
        return "AudioEncoder";
    }

    Status configureAac(int sampleRate, int channels, int bitrate);
    Status buildSequenceHeader(media::AacDecoderConfig* outConfig) const;
    Status encodeFrame(const media::AudioFrame& frame, std::vector<media::EncodedAudioAccessUnit>* outUnits);
    Status flush(std::vector<media::EncodedAudioAccessUnit>* outUnits);

private:
    struct EncoderContext;

    Status encodeReadyFrames(std::vector<media::EncodedAudioAccessUnit>* outUnits);
    Status drainPackets(int frameIndexHint,
                        int samplesPerChannelHint,
                        std::vector<media::EncodedAudioAccessUnit>* outUnits);

    std::unique_ptr<EncoderContext> context_;
    int sampleRate_ = 0;
    int channels_ = 0;
    int bitrate_ = 0;
    bool configured_ = false;
};

}  // namespace live_studio::encode
