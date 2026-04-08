#pragma once

#include <cstdint>
#include <string>

#include "common/MediaTypes.hpp"
#include "common/Status.hpp"

namespace live_studio::mux {

class FlvMuxer {
public:
    [[nodiscard]] std::string moduleName() const {
        return "FlvMuxer";
    }

    Status buildVideoSequenceHeader(const media::H264ParameterSets& parameterSets,
                                    media::FlvVideoTagPlan* outTag) const {//构建视频序列头信息
        if (outTag == nullptr) {
            return Status::InvalidArgument("outTag is null");
        }
        if (parameterSets.spsLabel.empty() || parameterSets.ppsLabel.empty()) {
            return Status::InvalidArgument("parameterSets are incomplete");
        }

        outTag->sequenceHeader = true;
        outTag->frameIndex = -1;
        outTag->keyFrame = true;
        outTag->timestampMs = 0;
        outTag->payloadSummary =
            "avc-sequence-header(" + parameterSets.spsLabel + "," + parameterSets.ppsLabel + ")";
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

        outTag->sequenceHeader = false;
        outTag->frameIndex = accessUnit.frameIndex;
        outTag->keyFrame = accessUnit.keyFrame;
        outTag->timestampMs = accessUnit.dtsMs;
        outTag->payloadSummary = (accessUnit.keyFrame ? "avc-keyframe-tag(" : "avc-interframe-tag(") +
                                 accessUnit.nalSummary + ")";
        return Status::Ok();
    }
};

}  // namespace live_studio::mux
