#pragma once

#include <functional>

#include <QJsonObject>
#include <QTimer>

#include "linkmic/LinkMicRoomState.hpp"
#include "linkmic/LinkMicSignalingClient.hpp"
#include "linkmic/RoomMemberDirectory.hpp"

namespace live_studio::linkmic {

class LinkMicSession {
public:
    using UpdateHandler = std::function<void()>;
    using ExternalMessageSender = std::function<bool(const QJsonObject&)>;

    LinkMicSession();

    void setUpdateHandler(UpdateHandler handler);
    void setExternalMessageSender(ExternalMessageSender sender);
    void setUseExternalTransport(bool enabled);
    void setExternalTransportState(SignalingConnectionState state, const QString& detail);
    void processExternalMessage(const QJsonObject& message);

    bool connectToRoom(const LinkMicSessionConfig& config);
    void disconnectFromRoom();

    bool invite(const QString& targetRoomKey, const QString& targetUserId);
    bool accept();
    bool reject();
    bool hangup();
    bool noteRtcConnected();

    [[nodiscard]] const LinkMicSessionConfig& config() const;
    [[nodiscard]] const RoomMemberDirectory& memberDirectory() const;
    [[nodiscard]] const LinkMicRoomState& roomState() const;
    [[nodiscard]] const QVector<LinkMicEventLogEntry>& eventLog() const;
    [[nodiscard]] bool usesExternalTransport() const;

private:
    void handleTransportStateChanged(SignalingConnectionState state, const QString& detail);
    void handleObservedMessage(const QString& direction, const QJsonObject& message);
    void handleIncomingMessage(const QJsonObject& message);
    void handleRequestTimeout();

    void appendEvent(const QString& direction,
                     const QString& type,
                     const QString& detail,
                     const QString& state = {},
                     const QString& sessionId = {},
                     const QString& requestId = {},
                     const QString& remoteRoomKey = {},
                     const QString& remoteUserId = {});
    void notifyUpdated() const;
    void startRequestTimer(const QString& reason);
    void stopRequestTimer();
    bool sendEnvelope(const QJsonObject& envelope, const QString& detail);
    [[nodiscard]] QJsonObject buildEnvelope(const QString& type,
                                            const QString& sessionId,
                                            const QString& requestId,
                                            const QString& toRoomKey,
                                            const QString& toUserId,
                                            const QJsonObject& payload) const;

    LinkMicSessionConfig config_;
    RoomMemberDirectory memberDirectory_;
    LinkMicRoomState roomState_;
    LinkMicSignalingClient signalingClient_;
    ExternalMessageSender externalMessageSender_;
    QVector<LinkMicEventLogEntry> eventLog_;
    QTimer requestTimer_;
    QString requestTimerReason_;
    bool useExternalTransport_ = false;
    SignalingConnectionState externalTransportState_ = SignalingConnectionState::disconnected;
    QString externalTransportDetail_;
    QString rtcConnectedNotifiedSessionId_;
    UpdateHandler onUpdated_;
};

}  // namespace live_studio::linkmic
