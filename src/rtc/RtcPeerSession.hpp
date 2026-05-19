#pragma once

#include <functional>
#include <memory>
#include <mutex>

#include <QString>

#include "common/MediaTypes.hpp"
#include "rtc/RtcSessionConfig.hpp"
#include "rtc/RtcTypes.hpp"

namespace rtc {
class DataChannel;
class PeerConnection;
}

namespace live_studio::rtc {

class RtcPeerSession {
public:
    struct Impl;

    struct Callbacks {
        std::function<void(const QString& sdp, const QString& descriptionType)> onLocalDescription;
        std::function<void(const QString& candidate, const QString& mid)> onLocalIceCandidate;
        std::function<void()> onPeerConnectionConnected;
        std::function<void(const media::VideoFrame& frame)> onRemoteVideoFrame;
        std::function<void()> onRemoteVideoCleared;
        std::function<void(const QString& message, bool warning)> onLog;
    };

    RtcPeerSession();
    ~RtcPeerSession();

    void setCallbacks(Callbacks callbacks);

    bool start(const RtcSessionConfig& config);
    void stop(const QString& reason = {});

    bool applyRemoteDescription(const QString& sdp, const QString& descriptionType);
    bool addRemoteCandidate(const QString& candidate, const QString& mid);
    void pushLocalVideoFrame(const media::VideoFrame& frame);
    void pushLocalAudioFrame(const media::AudioFrame& frame);

    [[nodiscard]] RtcPeerSessionSnapshot snapshot() const;
    [[nodiscard]] Callbacks currentCallbacks() const;
    [[nodiscard]] RtcSessionConfig currentConfig() const;
    void emitLog(const QString& message, bool warning) const;
    void signalMediaFailure(const QString& message);

private:
    void updateSnapshot(const std::function<void(RtcPeerSessionSnapshot&)>& updater);
    bool createPeerConnection();
    void attachPeerConnectionHandlers(const std::shared_ptr<Impl>& impl);
    void attachDataChannelHandlers(const std::shared_ptr<::rtc::DataChannel>& channel);
    void maybeCreateOutboundDataChannel();
    void handleDataChannelStringMessage(const QString& payload,
                                        const std::shared_ptr<::rtc::DataChannel>& channel);

    mutable std::mutex mutex_;
    RtcSessionConfig config_;
    RtcPeerSessionSnapshot snapshot_;
    std::shared_ptr<Impl> impl_;
    Callbacks callbacks_;
    bool started_ = false;
};

}  // namespace live_studio::rtc
