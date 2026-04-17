#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "common/MediaTypes.hpp"
#include "common/Status.hpp"

namespace live_studio::mux {
namespace detail {

inline void appendUint16Be(std::uint16_t value, std::vector<std::uint8_t>* outBytes) {
    outBytes->push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    outBytes->push_back(static_cast<std::uint8_t>(value & 0xff));
}

inline void appendUint24Be(std::uint32_t value, std::vector<std::uint8_t>* outBytes) {
    outBytes->push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    outBytes->push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    outBytes->push_back(static_cast<std::uint8_t>(value & 0xff));
}

inline void appendUint32Be(std::uint32_t value, std::vector<std::uint8_t>* outBytes) {
    outBytes->push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
    outBytes->push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    outBytes->push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    outBytes->push_back(static_cast<std::uint8_t>(value & 0xff));
}

inline bool startsWithStartCode(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::size_t* codeSize) {
    if ((offset + 3) <= bytes.size() && bytes[offset] == 0 && bytes[offset + 1] == 0 && bytes[offset + 2] == 1) {
        *codeSize = 3;
        return true;
    }
    if ((offset + 4) <= bytes.size() && bytes[offset] == 0 && bytes[offset + 1] == 0 && bytes[offset + 2] == 0 &&
        bytes[offset + 3] == 1) {
        *codeSize = 4;
        return true;
    }
    return false;
}

inline std::vector<std::vector<std::uint8_t>> splitAnnexBNalus(const std::vector<std::uint8_t>& bytes) {
    std::vector<std::vector<std::uint8_t>> nalUnits;
    std::size_t cursor = 0;
    while (cursor < bytes.size()) {
        std::size_t startCodeSize = 0;
        if (!startsWithStartCode(bytes, cursor, &startCodeSize)) {
            ++cursor;
            continue;
        }

        const std::size_t naluStart = cursor + startCodeSize;
        std::size_t next = naluStart;
        while (next < bytes.size()) {
            std::size_t nextStartCodeSize = 0;
            if (startsWithStartCode(bytes, next, &nextStartCodeSize)) {
                break;
            }
            ++next;
        }

        if (next > naluStart) {
            nalUnits.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(naluStart),
                                  bytes.begin() + static_cast<std::ptrdiff_t>(next));
        }
        cursor = next;
    }
    return nalUnits;
}

inline Status appendLengthPrefixedNalu(const std::vector<std::uint8_t>& nalu, std::vector<std::uint8_t>* outBytes) {
    if (outBytes == nullptr) {
        return Status::InvalidArgument("outBytes is null");
    }
    if (nalu.empty()) {
        return Status::InvalidArgument("NAL unit is empty");
    }
    if (nalu.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        return Status::InvalidArgument("NAL unit exceeds 32-bit AVCC length");
    }

    appendUint32Be(static_cast<std::uint32_t>(nalu.size()), outBytes);
    outBytes->insert(outBytes->end(), nalu.begin(), nalu.end());
    return Status::Ok();
}

inline Status buildAvcDecoderConfigurationRecord(const media::H264ParameterSets& parameterSets,
                                                 std::vector<std::uint8_t>* outBytes) {
    if (outBytes == nullptr) {
        return Status::InvalidArgument("outBytes is null");
    }
    if (parameterSets.spsBytes.empty() || parameterSets.ppsBytes.empty()) {
        return Status::InvalidArgument("parameterSets are incomplete");
    }
    if (parameterSets.spsBytes.size() < 4) {
        return Status::InvalidArgument("SPS is too short for AVCDecoderConfigurationRecord");
    }
    if (parameterSets.spsBytes.size() > static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()) ||
        parameterSets.ppsBytes.size() > static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max())) {
        return Status::InvalidArgument("SPS/PPS exceeds AVCDecoderConfigurationRecord limits");
    }

    std::vector<std::uint8_t> record;
    record.reserve(11 + parameterSets.spsBytes.size() + parameterSets.ppsBytes.size());
    record.push_back(1);
    record.push_back(parameterSets.spsBytes[1]);
    record.push_back(parameterSets.spsBytes[2]);
    record.push_back(parameterSets.spsBytes[3]);
    record.push_back(0xff);
    record.push_back(0xe1);
    appendUint16Be(static_cast<std::uint16_t>(parameterSets.spsBytes.size()), &record);
    record.insert(record.end(), parameterSets.spsBytes.begin(), parameterSets.spsBytes.end());
    record.push_back(1);
    appendUint16Be(static_cast<std::uint16_t>(parameterSets.ppsBytes.size()), &record);
    record.insert(record.end(), parameterSets.ppsBytes.begin(), parameterSets.ppsBytes.end());

    *outBytes = std::move(record);
    return Status::Ok();
}

inline Status buildAvcVideoPayload(const std::vector<std::uint8_t>& avcPayload,
                                   bool sequenceHeader,
                                   bool keyFrame,
                                   std::int32_t compositionTimeMs,
                                   std::vector<std::uint8_t>* outBytes) {
    if (outBytes == nullptr) {
        return Status::InvalidArgument("outBytes is null");
    }
    if (avcPayload.empty()) {
        return Status::InvalidArgument("AVC payload is empty");
    }
    if (compositionTimeMs < -8388608 || compositionTimeMs > 8388607) {
        return Status::InvalidArgument("compositionTimeMs exceeds signed 24-bit FLV range");
    }

    std::vector<std::uint8_t> payload;
    payload.reserve(5 + avcPayload.size());
    payload.push_back(static_cast<std::uint8_t>(((keyFrame ? 1 : 2) << 4) | 7));
    payload.push_back(static_cast<std::uint8_t>(sequenceHeader ? 0 : 1));

    const std::uint32_t compositionTimeField =
        static_cast<std::uint32_t>(compositionTimeMs < 0 ? compositionTimeMs + 0x1000000 : compositionTimeMs);
    appendUint24Be(compositionTimeField, &payload);
    payload.insert(payload.end(), avcPayload.begin(), avcPayload.end());

    *outBytes = std::move(payload);
    return Status::Ok();
}

inline Status buildFlvTagBytes(std::int64_t timestampMs,
                               std::uint8_t tagType,
                               const std::vector<std::uint8_t>& payloadBytes,
                               std::vector<std::uint8_t>* outTagBytes,
                               std::uint32_t* outPreviousTagSize) {
    if (outTagBytes == nullptr || outPreviousTagSize == nullptr) {
        return Status::InvalidArgument("FLV tag outputs are null");
    }
    if (timestampMs < 0) {
        return Status::InvalidArgument("FLV tag timestamp must be >= 0");
    }
    if (timestampMs > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
        return Status::InvalidArgument("FLV tag timestamp exceeds 32-bit range");
    }
    if (payloadBytes.empty()) {
        return Status::InvalidArgument("FLV tag payload is empty");
    }
    if (payloadBytes.size() > 0x00ffffffu) {
        return Status::InvalidArgument("FLV tag payload exceeds 24-bit size field");
    }

    std::vector<std::uint8_t> tagBytes;
    tagBytes.reserve(11 + payloadBytes.size());
    tagBytes.push_back(tagType);
    appendUint24Be(static_cast<std::uint32_t>(payloadBytes.size()), &tagBytes);

    const std::uint32_t timestampField = static_cast<std::uint32_t>(timestampMs);
    appendUint24Be(timestampField & 0x00ffffffu, &tagBytes);
    tagBytes.push_back(static_cast<std::uint8_t>((timestampField >> 24) & 0xff));
    appendUint24Be(0, &tagBytes);
    tagBytes.insert(tagBytes.end(), payloadBytes.begin(), payloadBytes.end());

    *outPreviousTagSize = static_cast<std::uint32_t>(tagBytes.size());
    *outTagBytes = std::move(tagBytes);
    return Status::Ok();
}

inline Status buildAacAudioPayload(const std::vector<std::uint8_t>& audioPayload,
                                   bool sequenceHeader,
                                   int channels,
                                   std::vector<std::uint8_t>* outBytes) {
    if (outBytes == nullptr) {
        return Status::InvalidArgument("outBytes is null");
    }
    if (audioPayload.empty()) {
        return Status::InvalidArgument("AAC audio payload is empty");
    }
    if (channels <= 0) {
        return Status::InvalidArgument("AAC audio payload requires positive channels");
    }

    std::vector<std::uint8_t> payload;
    payload.reserve(2 + audioPayload.size());
    const std::uint8_t soundFormat = 10;
    const std::uint8_t soundRate = 3;
    const std::uint8_t soundSize = 1;
    const std::uint8_t soundType = channels > 1 ? 1 : 0;
    payload.push_back(static_cast<std::uint8_t>((soundFormat << 4) | (soundRate << 2) | (soundSize << 1) | soundType));
    payload.push_back(static_cast<std::uint8_t>(sequenceHeader ? 0 : 1));
    payload.insert(payload.end(), audioPayload.begin(), audioPayload.end());

    *outBytes = std::move(payload);
    return Status::Ok();
}

}  // namespace detail

class FlvMuxer {
public:
    [[nodiscard]] std::string moduleName() const {
        return "FlvMuxer";
    }

    Status buildVideoSequenceHeader(const media::H264ParameterSets& parameterSets,
                                    media::FlvVideoTagPlan* outTag) const {
        if (outTag == nullptr) {
            return Status::InvalidArgument("outTag is null");
        }
        if (parameterSets.spsLabel.empty() || parameterSets.ppsLabel.empty()) {
            return Status::InvalidArgument("parameterSets are incomplete");
        }

        std::vector<std::uint8_t> avcConfigBytes;
        Status status = detail::buildAvcDecoderConfigurationRecord(parameterSets, &avcConfigBytes);
        if (!status.ok()) {
            return status;
        }

        std::vector<std::uint8_t> payloadBytes;
        status = detail::buildAvcVideoPayload(avcConfigBytes, true, true, 0, &payloadBytes);
        if (!status.ok()) {
            return status;
        }

        std::vector<std::uint8_t> tagBytes;
        std::uint32_t previousTagSize = 0;
        status = detail::buildFlvTagBytes(0, 9, payloadBytes, &tagBytes, &previousTagSize);
        if (!status.ok()) {
            return status;
        }

        outTag->sequenceHeader = true;
        outTag->frameIndex = -1;
        outTag->keyFrame = true;
        outTag->timestampMs = 0;
        outTag->compositionTimeMs = 0;
        outTag->payloadSummary =
            "avc-sequence-header(" + parameterSets.spsLabel + "," + parameterSets.ppsLabel + ")";
        outTag->payloadBytes = std::move(payloadBytes);
        outTag->tagBytes = std::move(tagBytes);
        outTag->previousTagSize = previousTagSize;
        return Status::Ok();
    }

    Status buildVideoNaluTag(const media::EncodedVideoAccessUnit& accessUnit,
                             media::FlvVideoTagPlan* outTag) const {
        if (outTag == nullptr) {
            return Status::InvalidArgument("outTag is null");
        }
        if (accessUnit.ptsMs < 0 || accessUnit.dtsMs < 0) {
            return Status::InvalidArgument("access unit timestamps must be >= 0");
        }
        if (accessUnit.annexBPayload.empty()) {
            return Status::InvalidArgument("access unit Annex B payload is empty");
        }

        const std::vector<std::vector<std::uint8_t>> nalUnits = detail::splitAnnexBNalus(accessUnit.annexBPayload);
        if (nalUnits.empty()) {
            return Status::InvalidArgument("access unit Annex B payload does not contain NAL units");
        }

        std::vector<std::uint8_t> avcPayload;
        avcPayload.reserve(accessUnit.annexBPayload.size());
        for (const std::vector<std::uint8_t>& nalu : nalUnits) {
            Status status = detail::appendLengthPrefixedNalu(nalu, &avcPayload);
            if (!status.ok()) {
                return status;
            }
        }

        const std::int64_t compositionTime64 = accessUnit.ptsMs - accessUnit.dtsMs;
        if (compositionTime64 < static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()) ||
            compositionTime64 > static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())) {
            return Status::InvalidArgument("composition time exceeds 32-bit integer range");
        }
        const std::int32_t compositionTimeMs = static_cast<std::int32_t>(compositionTime64);

        std::vector<std::uint8_t> payloadBytes;
        Status status =
            detail::buildAvcVideoPayload(avcPayload, false, accessUnit.keyFrame, compositionTimeMs, &payloadBytes);
        if (!status.ok()) {
            return status;
        }

        std::vector<std::uint8_t> tagBytes;
        std::uint32_t previousTagSize = 0;
        status = detail::buildFlvTagBytes(accessUnit.dtsMs, 9, payloadBytes, &tagBytes, &previousTagSize);
        if (!status.ok()) {
            return status;
        }

        outTag->sequenceHeader = false;
        outTag->frameIndex = accessUnit.frameIndex;
        outTag->keyFrame = accessUnit.keyFrame;
        outTag->timestampMs = accessUnit.dtsMs;
        outTag->compositionTimeMs = compositionTimeMs;
        outTag->payloadSummary = (accessUnit.keyFrame ? "avc-keyframe-tag(" : "avc-interframe-tag(") +
                                 accessUnit.nalSummary + ")";
        outTag->payloadBytes = std::move(payloadBytes);
        outTag->tagBytes = std::move(tagBytes);
        outTag->previousTagSize = previousTagSize;
        return Status::Ok();
    }

    Status buildAudioSequenceHeader(const media::AacDecoderConfig& decoderConfig,
                                    media::FlvAudioTagPlan* outTag) const {
        if (outTag == nullptr) {
            return Status::InvalidArgument("outTag is null");
        }
        if (decoderConfig.audioSpecificConfigBytes.empty()) {
            return Status::InvalidArgument("AAC decoder config is empty");
        }
        if (decoderConfig.channels <= 0) {
            return Status::InvalidArgument("AAC decoder config channels must be positive");
        }

        std::vector<std::uint8_t> payloadBytes;
        Status status = detail::buildAacAudioPayload(
            decoderConfig.audioSpecificConfigBytes, true, decoderConfig.channels, &payloadBytes);
        if (!status.ok()) {
            return status;
        }

        std::vector<std::uint8_t> tagBytes;
        std::uint32_t previousTagSize = 0;
        status = detail::buildFlvTagBytes(0, 8, payloadBytes, &tagBytes, &previousTagSize);
        if (!status.ok()) {
            return status;
        }

        outTag->sequenceHeader = true;
        outTag->frameIndex = -1;
        outTag->timestampMs = 0;
        outTag->payloadSummary = "aac-sequence-header(" + decoderConfig.configLabel + ")";
        outTag->payloadBytes = std::move(payloadBytes);
        outTag->tagBytes = std::move(tagBytes);
        outTag->previousTagSize = previousTagSize;
        return Status::Ok();
    }

    Status buildAudioRawTag(const media::EncodedAudioAccessUnit& accessUnit,
                            int channels,
                            media::FlvAudioTagPlan* outTag) const {
        if (outTag == nullptr) {
            return Status::InvalidArgument("outTag is null");
        }
        if (channels <= 0) {
            return Status::InvalidArgument("AAC channels must be positive");
        }
        if (accessUnit.ptsMs < 0 || accessUnit.dtsMs < 0) {
            return Status::InvalidArgument("AAC access unit timestamps must be >= 0");
        }
        if (accessUnit.rawPayload.empty()) {
            return Status::InvalidArgument("AAC access unit payload is empty");
        }

        std::vector<std::uint8_t> payloadBytes;
        Status status = detail::buildAacAudioPayload(accessUnit.rawPayload, false, channels, &payloadBytes);
        if (!status.ok()) {
            return status;
        }

        std::vector<std::uint8_t> tagBytes;
        std::uint32_t previousTagSize = 0;
        status = detail::buildFlvTagBytes(accessUnit.dtsMs, 8, payloadBytes, &tagBytes, &previousTagSize);
        if (!status.ok()) {
            return status;
        }

        outTag->sequenceHeader = false;
        outTag->frameIndex = accessUnit.frameIndex;
        outTag->timestampMs = accessUnit.dtsMs;
        outTag->payloadSummary = "aac-raw-tag(" + accessUnit.payloadSummary + ")";
        outTag->payloadBytes = std::move(payloadBytes);
        outTag->tagBytes = std::move(tagBytes);
        outTag->previousTagSize = previousTagSize;
        return Status::Ok();
    }
};

}  // namespace live_studio::mux
