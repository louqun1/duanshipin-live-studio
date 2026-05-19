#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include <QString>

#include "common/Status.hpp"

namespace live_studio::rtc {

class RtcRemoteAudioSink {
public:
    struct Config {
        int expectedSampleRate = 48000;
        int expectedChannels = 2;
        bool enablePcmDump = false;
        QString pcmDumpPath;
    };

    using LogCallback = std::function<void(const QString& message, bool warning)>;

    RtcRemoteAudioSink();
    ~RtcRemoteAudioSink();

    RtcRemoteAudioSink(const RtcRemoteAudioSink&) = delete;
    RtcRemoteAudioSink& operator=(const RtcRemoteAudioSink&) = delete;

    void setLogCallback(LogCallback callback);
    Status start(const Config& config);
    void stop();
    void submitEncodedFrame(const std::uint8_t* data, std::size_t size, std::uint32_t rtpTimestamp);

private:
    struct State;

    void emitLog(const QString& message, bool warning) const;

    LogCallback logCallback_;
    std::unique_ptr<State> state_;
};

}  // namespace live_studio::rtc
