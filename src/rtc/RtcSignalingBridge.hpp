#pragma once

#include <functional>

#include <QJsonObject>
#include <QString>

#include "rtc/RtcSessionConfig.hpp"

namespace live_studio::rtc {

class RtcSignalingBridge {
public:
    using SendEnvelope = std::function<bool(const QJsonObject&)>;

    struct Callbacks {
        std::function<void(const QString& sdp, const QString& descriptionType)> onRemoteDescription;
        std::function<void(const QString& candidate, const QString& mid)> onRemoteIceCandidate;
        std::function<void(const QString& message)> onWarning;
    };

    void setCallbacks(Callbacks callbacks);
    bool start(const RtcSessionConfig& config, SendEnvelope sender);
    void stop();

    [[nodiscard]] bool isActive() const;
    [[nodiscard]] const RtcSessionConfig& config() const;
    [[nodiscard]] QString sessionId() const;

    bool handleIncomingMessage(const QJsonObject& message);
    bool sendLocalDescription(const QString& sdp, const QString& descriptionType);
    bool sendLocalIceCandidate(const QString& candidate, const QString& mid);
    bool sendConnected();

private:
    [[nodiscard]] bool messageTargetsSession(const QJsonObject& message) const;
    [[nodiscard]] QJsonObject buildEnvelope(const QString& type, const QJsonObject& payload) const;

    RtcSessionConfig config_;
    SendEnvelope sendEnvelope_;
    Callbacks callbacks_;
    bool active_ = false;
};

}  // namespace live_studio::rtc
