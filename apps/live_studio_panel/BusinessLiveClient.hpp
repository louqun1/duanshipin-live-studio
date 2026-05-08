#pragma once

#include <QObject>

#include <QByteArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

#include "BusinessLiveTypes.hpp"

class QNetworkReply;

namespace live_studio::business {

class BusinessLiveClient final : public QObject {
    Q_OBJECT

public:
    explicit BusinessLiveClient(QObject* parent = nullptr);
    ~BusinessLiveClient() override;

    BusinessLiveClient(const BusinessLiveClient&) = delete;
    BusinessLiveClient& operator=(const BusinessLiveClient&) = delete;

    void beginStartSequence(const BusinessStartConfig& config);
    void beginStopSequence();
    void setPublishingActive(bool active);
    void shutdown();
    bool sendSignalingEnvelope(const QJsonObject& object);

    [[nodiscard]] const BusinessLiveSnapshot& snapshot() const {
        return snapshot_;
    }

    [[nodiscard]] bool isReadyToPublish() const {
        return snapshot_.anchorJoined;
    }

signals:
    void stateChanged();
    void readyToPublish();
    void warningRaised(const QString& message);
    void errorRaised(const QString& message);
    void signalingMessageReceived(const QJsonObject& message);

private:
    struct ParsedReply {
        int statusCode = 0;
        QByteArray body;
        QJsonObject object;
        QString errorMessage;
    };

    void resetForNewStart();
    void clearRoomStatePreservingLogin();
    void emitStateChanged();
    void updateState(BusinessLiveState state);
    void failStartSequence(const QString& message);
    void raiseWarning(const QString& message);
    void startLoginRequest();
    void startCreateRoomRequest();
    void startExistingRoomLookup();
    void startCloseRoomRequest(const QString& roomKey, const QString& accessToken);
    void openBusinessSocket();
    void scheduleReconnect();
    void stopHeartbeat();
    void sendAnchorJoin();
    void sendAnchorLeaveBestEffort(const QString& roomKey);
    void handleSocketTextMessage(const QString& message);
    void handleSocketConnected();
    void handleSocketDisconnected();
    void handleJoinTimeout();
    void handleAnchorJoined(const QJsonObject& object);
    void handleAnchorError(const QJsonObject& object);
    void handleRoomMemberList(const QJsonObject& object);
    void handleRoomMemberJoin(const QJsonObject& object);
    void handleRoomMemberLeave(const QJsonObject& object);
    [[nodiscard]] bool applyRoomFromResponse(const QJsonObject& roomObject);

    [[nodiscard]] QString currentRoomKey() const;
    [[nodiscard]] QString derivedSignalingUrl() const;
    [[nodiscard]] QString normalizedApiBaseUrl() const;
    [[nodiscard]] QUrl apiUrlForPath(const QString& path) const;
    [[nodiscard]] bool shouldReconnect() const;
    [[nodiscard]] bool sendSocketJsonMessage(const QJsonObject& object);
    [[nodiscard]] static ParsedReply parseReply(QNetworkReply* reply);

    QNetworkAccessManager network_;
    QWebSocket socket_;
    QTimer heartbeatTimer_;
    QTimer reconnectTimer_;
    QTimer joinTimeoutTimer_;
    BusinessStartConfig startConfig_;
    BusinessLiveSnapshot snapshot_;
    bool startSequenceActive_ = false;
    bool publishingActive_ = false;
    bool awaitingJoinAck_ = false;
    bool suppressDisconnectHandling_ = false;
    int reconnectAttempt_ = 0;
};

}  // namespace live_studio::business
