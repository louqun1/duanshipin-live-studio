#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include <QString>

#include "common/MediaTypes.hpp"
#include "common/Status.hpp"

namespace live_studio::rtc {

class RtcRemoteVideoSink {
public:
    struct Config {
        bool enableH264Dump = false;
        QString h264DumpPath;
    };

    using FrameCallback = std::function<void(const media::VideoFrame& frame)>;
    using ClearCallback = std::function<void()>;
    using LogCallback = std::function<void(const QString& message, bool warning)>;

    RtcRemoteVideoSink();
    ~RtcRemoteVideoSink();

    RtcRemoteVideoSink(const RtcRemoteVideoSink&) = delete;
    RtcRemoteVideoSink& operator=(const RtcRemoteVideoSink&) = delete;

    void setFrameCallback(FrameCallback callback);
    void setClearCallback(ClearCallback callback);
    void setLogCallback(LogCallback callback);
    Status start(const Config& config);
    void stop();
    void submitEncodedFrame(const std::uint8_t* data, std::size_t size, std::uint32_t rtpTimestamp);

private:
    struct State;

    void emitLog(const QString& message, bool warning) const;

    FrameCallback frameCallback_;
    ClearCallback clearCallback_;
    LogCallback logCallback_;
    std::unique_ptr<State> state_;
};

}  // namespace live_studio::rtc
