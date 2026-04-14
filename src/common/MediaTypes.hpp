#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace live_studio::media {

enum class VideoPixelFormat {
    unknown,
    yuv420p,
};

struct VideoCaptureConfig {
    std::string sourceKind = "unconfigured";
    std::string sourceLabel = "unconfigured-source";
    std::string sourcePath;
    std::string deviceId;
    int width = 0;
    int height = 0;
    int fps = 0;
    int frameCount = 0;
};

struct VideoFrame {
    std::string sourceKind = "unconfigured";
    std::string sourceLabel = "unconfigured-source";
    int frameIndex = 0;
    int width = 0;
    int height = 0;
    int fps = 0;
    std::int64_t ptsMs = 0;
    bool hasPixelPayload = false;
    VideoPixelFormat pixelFormat = VideoPixelFormat::unknown;
    int lineStrideY = 0;
    int lineStrideU = 0;
    int lineStrideV = 0;
    std::vector<std::uint8_t> pixelPayload;
};

struct H264ParameterSets {
    std::string profile = "baseline";
    std::string spsLabel;
    std::string ppsLabel;
    std::vector<std::uint8_t> spsBytes;
    std::vector<std::uint8_t> ppsBytes;
    std::vector<std::uint8_t> extradataBytes;
};

struct EncodedVideoAccessUnit {
    int frameIndex = 0;
    bool keyFrame = false;
    std::int64_t ptsMs = 0;
    std::int64_t dtsMs = 0;
    std::string codec = "h264";
    std::string nalSummary;
    std::vector<std::uint8_t> annexBPayload;
};

struct FlvVideoTagPlan {
    bool sequenceHeader = false;
    int frameIndex = -1;
    bool keyFrame = false;
    std::int64_t timestampMs = 0;
    std::int32_t compositionTimeMs = 0; //合成时间偏移，dts + compositionTime = pts
    std::string payloadSummary;
    std::vector<std::uint8_t> payloadBytes;//不包含前4字节的previousTagSize字段
    std::vector<std::uint8_t> tagBytes;
    std::uint32_t previousTagSize = 0;
};

struct RtmpMessagePlan {
    int chunkStreamId = 6;
    int messageStreamId = 1;
    int typeId = 9;
    int frameIndex = -1;
    std::int64_t timestampMs = 0;
    std::string payloadSummary;
    std::vector<std::uint8_t> payloadBytes;//不包含前12字节的RTMP消息头
};

}  // namespace live_studio::media
