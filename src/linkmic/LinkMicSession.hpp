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

    LinkMicSession();

    void setUpdateHandler(UpdateHandler handler);

    bool connectToRoom(const LinkMicSessionConfig& config);
    void disconnectFromRoom();

    bool invite(const QString& targetUserId);
    bool apply(const QString& targetUserId);
    bool accept();
    bool reject();
    bool hangup();

    [[nodiscard]] const LinkMicSessionConfig& config() const;
    [[nodiscard]] const RoomMemberDirectory& memberDirectory() const;
    [[nodiscard]] const LinkMicRoomState& roomState() const;
    [[nodiscard]] const QVector<LinkMicEventLogEntry>& eventLog() const;

private:
    void handleTransportStateChanged(SignalingConnectionState state, const QString& detail);
    void handleObservedMessage(const QString& direction, const QJsonObject& message);
    void handleIncomingMessage(const QJsonObject& message);
    void handleRequestTimeout();

    void appendEvent(const QString& direction,
                     const QString& type,
                     const QString& detail,
                     const QString& state = {},
                     const QString& requestId = {},
                     const QString& remoteUserId = {});
    void notifyUpdated() const;
    void startRequestTimer(const QString& reason);
    void stopRequestTimer();
    bool sendBusinessMessage(const QString& type,
                             const QString& toUserId,
                             const QString& requestId,
                             const QJsonObject& payload,
                             const QString& detail);

    LinkMicSessionConfig config_;
    RoomMemberDirectory memberDirectory_;
    LinkMicRoomState roomState_;
    LinkMicSignalingClient signalingClient_;
    QVector<LinkMicEventLogEntry> eventLog_;
    QTimer requestTimer_;
    QString requestTimerReason_;
    UpdateHandler onUpdated_;
};

}  // namespace live_studio::linkmic
