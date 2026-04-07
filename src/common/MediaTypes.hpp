#pragma once

#include <cstdint>
#include <string>

namespace live_studio::media {

struct VideoCaptureConfig {
    std::string sourceLabel = "synthetic-camera-0";
    int width = 0;
    int height = 0;
    int fps = 0;
    int frameCount = 0;
};

struct VideoFrame {
    std::string sourceLabel;
    int frameIndex = 0;
    int width = 0;
    int height = 0;
    int fps = 0;
    std::int64_t ptsMs = 0;
    bool synthetic = true;
};

struct H264ParameterSets {
    std::string profile = "baseline";
    std::string spsLabel;
    std::string ppsLabel;
};

struct EncodedVideoAccessUnit {
    int frameIndex = 0;
    bool keyFrame = false;
    std::int64_t ptsMs = 0;
    std::int64_t dtsMs = 0;
    std::string nalSummary;
};

struct FlvVideoTagPlan {
    bool sequenceHeader = false;
    int frameIndex = -1;
    bool keyFrame = false;
    std::int64_t timestampMs = 0;
    std::string payloadSummary;
};

struct RtmpMessagePlan {
    int chunkStreamId = 6;
    int messageStreamId = 1;
    int typeId = 9;
    int frameIndex = -1;
    std::int64_t timestampMs = 0;
    std::string payloadSummary;
};

}  // namespace live_studio::media
