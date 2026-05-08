#pragma once

#include <functional>
#include <memory>
#include <mutex>

#include <QString>

#include "rtc/RtcSessionConfig.hpp"
#include "rtc/RtcTypes.hpp"

namespace rtc {
class DataChannel;
class PeerConnection;
}

namespace live_studio::rtc {

class RtcPeerSession {
public:
    struct Callbacks {
        std::function<void(const QString& sdp, const QString& descriptionType)> onLocalDescription;
        std::function<void(const QString& candidate, const QString& mid)> onLocalIceCandidate;
        std::function<void()> onPeerConnectionConnected;
    };

    RtcPeerSession();
    ~RtcPeerSession();

    void setCallbacks(Callbacks callbacks);

    bool start(const RtcSessionConfig& config);
    void stop(const QString& reason = {});

    bool applyRemoteDescription(const QString& sdp, const QString& descriptionType);
    bool addRemoteCandidate(const QString& candidate, const QString& mid);

    [[nodiscard]] RtcPeerSessionSnapshot snapshot() const;

private:
    void updateSnapshot(const std::function<void(RtcPeerSessionSnapshot&)>& updater);
    void createPeerConnectionLocked();
    void attachPeerConnectionHandlersLocked();
    void attachDataChannelHandlersLocked(const std::shared_ptr<::rtc::DataChannel>& channel);
    void maybeCreateOutboundDataChannelLocked();
    void handleDataChannelStringMessage(const QString& payload,
                                        const std::shared_ptr<::rtc::DataChannel>& channel);

    mutable std::mutex mutex_;
    RtcSessionConfig config_;
    RtcPeerSessionSnapshot snapshot_;
    std::shared_ptr<::rtc::PeerConnection> peerConnection_;
    std::shared_ptr<::rtc::DataChannel> dataChannel_;
    Callbacks callbacks_;
    bool started_ = false;
};

}  // namespace live_studio::rtc
