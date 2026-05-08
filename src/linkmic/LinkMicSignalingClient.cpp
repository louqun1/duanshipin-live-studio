#include "linkmic/LinkMicSignalingClient.hpp"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkProxy>

#include "spdlog/spdlog.h"

namespace live_studio::linkmic {
namespace {

std::string toStdString(const QString& value) {
    const QByteArray utf8 = value.toUtf8();
    return std::string(utf8.constData(), static_cast<std::size_t>(utf8.size()));
}

QJsonObject buildEnvelope(const LinkMicSessionConfig& config,
                          const QString& type,
                          const QString& toUserId,
                          const QString& requestId,
                          const QJsonObject& payload) {
    QJsonObject message{
        {"type", type},
        {"room_id", config.roomKey},
        {"from_user_id", config.userId},
        {"to_user_id", toUserId},
        {"request_id", requestId},
        {"payload", payload},
        {"ts_ms", static_cast<qint64>(QDateTime::currentMSecsSinceEpoch())}
    };

    if (!config.displayName.trimmed().isEmpty()) {
        message.insert("display_name", config.displayName);
    }

    return message;
}

}  // namespace

LinkMicSignalingClient::LinkMicSignalingClient() {
    socket_.setProxy(QNetworkProxy::NoProxy);
    spdlog::info("[linkmic-ws] initialized proxy=NoProxy");

    QObject::connect(&socket_, &QWebSocket::connected, &socket_, [this]() {
        handleSocketConnected();
    });
    QObject::connect(&socket_, &QWebSocket::disconnected, &socket_, [this]() {
        handleSocketDisconnected();
    });
    QObject::connect(&socket_, &QWebSocket::errorOccurred, &socket_, [this](QAbstractSocket::SocketError error) {
        handleSocketError(error);
    });
    QObject::connect(&socket_, &QWebSocket::textMessageReceived, &socket_, [this](const QString& text) {
        handleSocketTextMessage(text);
    });
}

void LinkMicSignalingClient::setJsonMessageHandler(JsonHandler handler) {
    onJsonMessage_ = std::move(handler);
}

void LinkMicSignalingClient::setStateChangedHandler(StateChangedHandler handler) {
    onStateChanged_ = std::move(handler);
}

void LinkMicSignalingClient::setObservedMessageHandler(ObservedMessageHandler handler) {
    onObservedMessage_ = std::move(handler);
}

void LinkMicSignalingClient::connectToServer(const LinkMicSessionConfig& config) {
    config_ = config;
    lastError_.clear();

    if (socket_.state() != QAbstractSocket::UnconnectedState) {
        socket_.abort();
    }

    spdlog::info("[linkmic-ws] opening url={} roomId={} userId={} role={}",
                 toStdString(config.signalingUrl.toString(QUrl::FullyEncoded)),
                 toStdString(config.roomKey),
                 toStdString(config.userId),
                 "anchor");
    transitionState(SignalingConnectionState::connecting, "Opening signaling WebSocket");
    socket_.open(config.signalingUrl);
}

void LinkMicSignalingClient::disconnectFromServer() {
    if (socket_.state() != QAbstractSocket::UnconnectedState) {
        socket_.close();
    }
}

bool LinkMicSignalingClient::isConnected() const {
    return state_ == SignalingConnectionState::connected;
}

SignalingConnectionState LinkMicSignalingClient::connectionState() const {
    return state_;
}

QString LinkMicSignalingClient::lastError() const {
    return lastError_;
}

bool LinkMicSignalingClient::sendMessage(const QJsonObject& message) {
    return sendRawMessage(message);
}

bool LinkMicSignalingClient::sendRawMessage(const QJsonObject& message) {
    if (!isConnected()) {
        lastError_ = "Signaling WebSocket is not connected.";
        return false;
    }

    if (onObservedMessage_) {
        onObservedMessage_("send", message);
    }

    const QString text = QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact));
    if (socket_.sendTextMessage(text) < 0) {
        lastError_ = "Failed to send signaling message.";
        return false;
    }

    return true;
}

void LinkMicSignalingClient::transitionState(SignalingConnectionState state, const QString& detail) {
    state_ = state;
    if (state != SignalingConnectionState::failed) {
        lastError_.clear();
    }
    if (onStateChanged_) {
        onStateChanged_(state, detail);
    }
}

void LinkMicSignalingClient::sendRegistration() {
    QJsonObject registerMessage{
        {"type", "probe.register"},
        {"room_id", config_.roomKey},
        {"peer_id", config_.userId},
        {"role", "anchor"},
        {"mode", "signal"}
    };

    if (!config_.displayName.trimmed().isEmpty()) {
        registerMessage.insert("display_name", config_.displayName);
    }

    QJsonObject payload{
        {"client_name", "live_studio_panel"},
        {"display_name", config_.displayName}
    };
    registerMessage.insert("payload", payload);

    spdlog::info("[linkmic-ws] send probe.register roomId={} peerId={} role={}",
                 toStdString(config_.roomKey),
                 toStdString(config_.userId),
                 "anchor");
    sendRawMessage(registerMessage);
}

void LinkMicSignalingClient::handleSocketConnected() {
    spdlog::info("[linkmic-ws] connected url={} roomId={} userId={}",
                 toStdString(config_.signalingUrl.toString(QUrl::FullyEncoded)),
                 toStdString(config_.roomKey),
                 toStdString(config_.userId));
    transitionState(SignalingConnectionState::connected, "WebSocket connected");
    sendRegistration();
}

void LinkMicSignalingClient::handleSocketDisconnected() {
    spdlog::warn("[linkmic-ws] disconnected roomId={} userId={} state={} error={}",
                 toStdString(config_.roomKey),
                 toStdString(config_.userId),
                 static_cast<int>(socket_.state()),
                 toStdString(socket_.errorString()));
    if (state_ != SignalingConnectionState::failed) {
        transitionState(SignalingConnectionState::disconnected, "WebSocket disconnected");
    }
}

void LinkMicSignalingClient::handleSocketError(QAbstractSocket::SocketError error) {
    Q_UNUSED(error);

    lastError_ = socket_.errorString().trimmed();
    if (lastError_.isEmpty()) {
        lastError_ = "Unknown signaling WebSocket error.";
    }
    spdlog::error("[linkmic-ws] error roomId={} userId={} message={}",
                  toStdString(config_.roomKey),
                  toStdString(config_.userId),
                  toStdString(lastError_));
    transitionState(SignalingConnectionState::failed, lastError_);
}

void LinkMicSignalingClient::handleSocketTextMessage(const QString& text) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        lastError_ = "Invalid signaling message: " + parseError.errorString();
        if (onStateChanged_) {
            onStateChanged_(state_, lastError_);
        }
        return;
    }

    const QJsonObject message = document.object();
    spdlog::info("[linkmic-ws] recv type={} roomId={} peerId={}",
                 toStdString(message.value("type").toString().trimmed()),
                 toStdString(message.value("room_id").toString().trimmed()),
                 toStdString(message.value("peer_id").toString().trimmed()));
    if (onObservedMessage_) {
        onObservedMessage_("recv", message);
    }
    if (onJsonMessage_) {
        onJsonMessage_(message);
    }
}

}  // namespace live_studio::linkmic
