#pragma once

#include "common/WavFileWriter.hpp"
#include "capture/LiveDeviceCaptureSource.hpp"
#include "decode/OpusAudioDecoder.hpp"
#include "encode/OpusAudioEncoder.hpp"

#include <rtc/rtc.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace rtc_probe_common {

struct RtcAudioProbeConfig {
    std::string role;
    std::string peerId;
    std::string targetId;
    std::string roomId;
    std::string micCaptureConfigPath;
    std::string recvAudioWavPath;
    bool sendMic = false;
    bool expectRemoteAudio = false;
    int mediaRunMs = 0;
    int opusBitrate = 0;
};

struct RtcAudioProbeCallbacks {
    std::function<std::string()> currentRequestId;
    std::function<void(const std::string& event,
                       const std::string& detail,
                       const std::string& remoteUserId)> logEvent;
    std::function<void(const std::string& message)> recordFailure;
    std::function<void()> markDone;
    std::function<bool()> isDone;
    std::function<bool()> isShutdownRequested;
};

class RtcAudioProbe {
public:
    RtcAudioProbe(RtcAudioProbeConfig config, RtcAudioProbeCallbacks callbacks);
    ~RtcAudioProbe();

    RtcAudioProbe(const RtcAudioProbe&) = delete;
    RtcAudioProbe& operator=(const RtcAudioProbe&) = delete;

    [[nodiscard]] bool shouldSendMic() const noexcept;
    [[nodiscard]] bool shouldExpectRemoteAudio() const noexcept;
    [[nodiscard]] bool hasSatisfiedInboundAudioProbe() const;

    void bindDataChannel(const std::shared_ptr<rtc::DataChannel>& dc);
    void ensureInboundReady(const std::shared_ptr<rtc::PeerConnection>& pc);
    void ensureOutboundReady(const std::shared_ptr<rtc::PeerConnection>& pc);
    void attachInboundTrackHandlers(const std::shared_ptr<rtc::Track>& track, const std::string& peerId);
    void onPeerConnectionStateChange(rtc::PeerConnection::State state);
    void onDataChannelOpen();
    bool handleDataChannelStringMessage(const std::string& payload, const std::string& peerId);
    void checkMediaTimers();

private:
    struct OutboundState {
        std::shared_ptr<rtc::Track> track;
        std::thread worker;
        std::atomic_bool stopRequested{false};
        std::atomic_bool trackOpen{false};
        std::atomic_bool firstPacketSent{false};
        std::atomic_int capturedFrames{0};
        std::atomic_int sentPackets{0};
        std::atomic_int droppedFramesBeforeTrackOpen{0};
        std::atomic_llong sentSamples{0};
        std::optional<std::chrono::steady_clock::time_point> completionDeadline;
        live_studio::capture::LiveDeviceCaptureSource captureSource;
        live_studio::encode::OpusAudioEncoder encoder;
        int payloadType = 111;
        bool initialized = false;
    };

    struct InboundState {
        std::shared_ptr<rtc::Track> track;
        live_studio::decode::OpusAudioDecoder decoder;
        live_studio::common::WavFileWriter wavWriter;
        std::string wavPath;
        mutable std::mutex mutex;
        std::atomic_bool firstDecodedFrameLogged{false};
        std::atomic_bool continuousAudioLogged{false};
        std::atomic_int decodedFrames{0};
        std::atomic_llong decodedSamples{0};
        std::atomic_int outputSampleRate{0};
        std::atomic_int outputChannels{0};
        std::optional<std::uint32_t> firstRtpTimestamp;
        bool initialized = false;
    };

    [[nodiscard]] std::string currentRequestId() const;
    [[nodiscard]] bool isDone() const;
    [[nodiscard]] bool isShutdownRequested() const;
    void logEvent(const std::string& event,
                  const std::string& detail,
                  const std::string& remoteUserId = "") const;
    void recordFailure(const std::string& message) const;
    void markDone() const;

    [[nodiscard]] std::string outboundAudioMid() const;
    [[nodiscard]] std::string inboundAudioMid() const;
    [[nodiscard]] std::uint32_t stableSsrc() const;
    [[nodiscard]] std::string buildOpusProfile(int channels, int bitrate) const;
    [[nodiscard]] std::string defaultRecvAudioWavPath() const;
    [[nodiscard]] std::string buildInboundAudioSummary() const;
    [[nodiscard]] std::string buildOutboundAudioSummary() const;
    [[nodiscard]] std::int64_t minimumDecodedSamplesForSuccess() const;
    [[nodiscard]] bool hasDecodedContinuousAudio() const;
    [[nodiscard]] std::string buildAudioProbeCompleteMessage() const;

    void attachOutboundTrackHandlers(const std::shared_ptr<rtc::Track>& track, const std::string& peerId);
    void outboundAudioWorkerMain();
    void maybeMarkMediaScenarioDone(const std::string& reason);
    void maybeSendOutboundCompletionSignal();

    RtcAudioProbeConfig config_;
    RtcAudioProbeCallbacks callbacks_;
    mutable std::mutex stateMutex_;
    std::weak_ptr<rtc::DataChannel> dataChannel_;
    OutboundState outbound_;
    InboundState inbound_;
    std::atomic_bool inboundAudioFirstPacketLogged_{false};
    std::atomic_int inboundAudioPackets_{0};
    std::atomic_bool localOutboundAudioCompleted_{false};
    std::atomic_bool remoteOutboundAudioCompleted_{false};
    std::atomic_bool outboundCompletionSignalSent_{false};
    std::atomic_bool mediaCompletionLogged_{false};
};

}  // namespace rtc_probe_common
