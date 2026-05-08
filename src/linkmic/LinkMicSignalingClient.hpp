#pragma once

#include <functional>

#include <QAbstractSocket>
#include <QJsonObject>
#include <QWebSocket>

#include "linkmic/LinkMicTypes.hpp"

namespace live_studio::linkmic {

class LinkMicSignalingClient {
public:
    using JsonHandler = std::function<void(const QJsonObject&)>;
    using StateChangedHandler = std::function<void(SignalingConnectionState state, const QString& detail)>;
    using ObservedMessageHandler = std::function<void(const QString& direction, const QJsonObject& message)>;

    LinkMicSignalingClient();

    void setJsonMessageHandler(JsonHandler handler);
    void setStateChangedHandler(StateChangedHandler handler);
    void setObservedMessageHandler(ObservedMessageHandler handler);

    void connectToServer(const LinkMicSessionConfig& config);
    void disconnectFromServer();

    [[nodiscard]] bool isConnected() const;
    [[nodiscard]] SignalingConnectionState connectionState() const;
    [[nodiscard]] QString lastError() const;

    bool sendMessage(const QJsonObject& message);

private:
    void handleSocketConnected();
    void handleSocketDisconnected();
    void handleSocketError(QAbstractSocket::SocketError error);
    void handleSocketTextMessage(const QString& text);
    bool sendRawMessage(const QJsonObject& message);
    void transitionState(SignalingConnectionState state, const QString& detail);
    void sendRegistration();

    LinkMicSessionConfig config_;
    QWebSocket socket_;
    SignalingConnectionState state_ = SignalingConnectionState::disconnected;
    QString lastError_;
    JsonHandler onJsonMessage_;
    StateChangedHandler onStateChanged_;
    ObservedMessageHandler onObservedMessage_;
};

}  // namespace live_studio::linkmic
