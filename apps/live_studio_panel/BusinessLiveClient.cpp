#include "BusinessLiveClient.hpp"

#include <algorithm>
#include <optional>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include "spdlog/spdlog.h"

namespace {

constexpr int kHeartbeatIntervalMs = 30000;
constexpr int kJoinTimeoutMs = 10000;
constexpr int kReconnectDelayMs = 5000;
constexpr int kReconnectDelayMaxMs = 30000;

std::string toStdString(const QString& value) {
    const QByteArray utf8 = value.toUtf8();
    return std::string(utf8.constData(), static_cast<std::size_t>(utf8.size()));
}

QString parseReplyErrorMessage(const QJsonObject& object, const QByteArray& body, const QString& fallback) {
    const QString errorText = object.value("error").toString().trimmed();
    if (!errorText.isEmpty()) {
        return errorText;
    }

    const QString messageText = object.value("message").toString().trimmed();
    if (!messageText.isEmpty()) {
        return messageText;
    }

    const QString rawText = QString::fromUtf8(body).trimmed();
    if (!rawText.isEmpty()) {
        return rawText;
    }

    return fallback.trimmed().isEmpty() ? QString("unknown network error") : fallback.trimmed();
}

QUrl normalizeHttpBaseUrl(const QString& rawBaseUrl) {
    QString normalized = rawBaseUrl.trimmed();
    if (normalized.isEmpty()) {
        return {};
    }

    if (!normalized.startsWith("http://", Qt::CaseInsensitive) &&
        !normalized.startsWith("https://", Qt::CaseInsensitive)) {
        normalized.prepend("http://");
    }

    QUrl url(normalized);
    if (!url.isValid()) {
        return {};
    }

    QString path = url.path().trimmed();
    while (path.endsWith('/')) {
        path.chop(1);
    }
    url.setPath(path);
    return url;
}

QString deriveSignalingUrl(const QString& apiBaseUrl) {
    const QUrl httpUrl = normalizeHttpBaseUrl(apiBaseUrl);
    if (!httpUrl.isValid()) {
        return {};
    }

    QUrl wsUrl = httpUrl;
    if (QString::compare(wsUrl.scheme(), "https", Qt::CaseInsensitive) == 0) {
        wsUrl.setScheme("wss");
    } else {
        wsUrl.setScheme("ws");
    }
    wsUrl.setPath("/ws");
    wsUrl.setQuery(QString());
    wsUrl.setFragment(QString());
    return wsUrl.toString(QUrl::FullyEncoded);
}

bool isLinkMicOrRtcSignalType(const QString& type) {
    return type == "signal.error" ||
        type.startsWith("linkmic.") ||
        type.startsWith("rtc.");
}

QString messageTypeFromJson(const QJsonObject& object) {
    return object.value("type").toString().trimmed();
}

QString firstNonEmptyString(const QJsonObject& object, std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        const QString value = object.value(QLatin1String(key)).toString().trimmed();
        if (!value.isEmpty()) {
            return value;
        }
    }
    return {};
}

qint64 firstPositiveInt64(const QJsonObject& object, std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        if (!object.contains(QLatin1String(key))) {
            continue;
        }
        const qint64 value = static_cast<qint64>(object.value(QLatin1String(key)).toDouble());
        if (value > 0) {
            return value;
        }
    }
    return 0;
}

std::optional<live_studio::business::BusinessRoomMember> parseBusinessRoomMember(const QJsonValue& value) {
    if (!value.isObject()) {
        return std::nullopt;
    }

    const QJsonObject object = value.toObject();
    live_studio::business::BusinessRoomMember member;
    member.userId = firstPositiveInt64(object, {"userId", "user_id", "id", "memberId", "member_id"});
    if (member.userId <= 0) {
        return std::nullopt;
    }

    member.username = firstNonEmptyString(object, {"username", "display_name", "nickname", "name", "user_name"});
    member.role = firstNonEmptyString(object, {"role", "participant_role"});
    member.online = object.contains("online") ? object.value("online").toBool(true) : true;
    return member;
}

std::optional<live_studio::business::BusinessRoomMember> extractBusinessRoomMember(const QJsonObject& message) {
    if (message.contains("member")) {
        if (const auto member = parseBusinessRoomMember(message.value("member")); member.has_value()) {
            return member;
        }
    }
    if (message.contains("user")) {
        if (const auto member = parseBusinessRoomMember(message.value("user")); member.has_value()) {
            return member;
        }
    }
    if (message.contains("payload") && message.value("payload").isObject()) {
        const QJsonObject payload = message.value("payload").toObject();
        if (payload.contains("member")) {
            if (const auto member = parseBusinessRoomMember(payload.value("member")); member.has_value()) {
                return member;
            }
        }
        if (payload.contains("user")) {
            if (const auto member = parseBusinessRoomMember(payload.value("user")); member.has_value()) {
                return member;
            }
        }
        if (const auto member = parseBusinessRoomMember(QJsonValue(payload)); member.has_value()) {
            return member;
        }
    }
    return parseBusinessRoomMember(QJsonValue(message));
}

qint64 extractBusinessRoomMemberId(const QJsonObject& message) {
    if (const auto member = extractBusinessRoomMember(message); member.has_value()) {
        return member->userId;
    }
    return firstPositiveInt64(message, {"userId", "user_id", "id", "memberId", "member_id"});
}

}  // namespace

namespace live_studio::business {

BusinessLiveClient::BusinessLiveClient(QObject* parent) : QObject(parent) {
    socket_.setProxy(QNetworkProxy::NoProxy);
    spdlog::info("[business-ws] initialized proxy=NoProxy");

    heartbeatTimer_.setInterval(kHeartbeatIntervalMs);
    reconnectTimer_.setSingleShot(true);
    joinTimeoutTimer_.setSingleShot(true);
    joinTimeoutTimer_.setInterval(kJoinTimeoutMs);

    connect(&heartbeatTimer_, &QTimer::timeout, this, [this]() {
        if (!snapshot_.anchorJoined || currentRoomKey().isEmpty() ||
            socket_.state() != QAbstractSocket::ConnectedState) {
            return;
        }

        const QJsonObject heartbeatObject{
            {"type", "live.anchor.heartbeat"},
            {"roomKey", currentRoomKey()},
        };
        socket_.sendTextMessage(QString::fromUtf8(QJsonDocument(heartbeatObject).toJson(QJsonDocument::Compact)));
    });

    connect(&reconnectTimer_, &QTimer::timeout, this, [this]() {
        openBusinessSocket();
    });
    connect(&joinTimeoutTimer_, &QTimer::timeout, this, [this]() {
        handleJoinTimeout();
    });

    connect(&socket_, &QWebSocket::connected, this, [this]() {
        handleSocketConnected();
    });
    connect(&socket_, &QWebSocket::disconnected, this, [this]() {
        handleSocketDisconnected();
    });
    connect(&socket_, &QWebSocket::textMessageReceived, this, [this](const QString& message) {
        handleSocketTextMessage(message);
    });
    connect(&socket_, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        spdlog::error("[business-ws] error state={} roomKey={} message={}",
                      static_cast<int>(socket_.state()),
                      toStdString(currentRoomKey()),
                      toStdString(socket_.errorString()));
        if (socket_.state() == QAbstractSocket::UnconnectedState && startSequenceActive_ && !publishingActive_) {
            failStartSequence(QString("Business WebSocket error: %1").arg(socket_.errorString()));
            return;
        }
        if (socket_.state() == QAbstractSocket::UnconnectedState && shouldReconnect()) {
            raiseWarning(QString("Business presence disconnected: %1").arg(socket_.errorString()));
        }
    });
}

BusinessLiveClient::~BusinessLiveClient() {
    shutdown();
}

bool BusinessLiveClient::sendSignalingEnvelope(const QJsonObject& object) {
    return sendSocketJsonMessage(object);
}

void BusinessLiveClient::beginStartSequence(const BusinessStartConfig& config) {
    if (!config.isValid()) {
        failStartSequence("API Base URL, username, and password are required.");
        return;
    }

    spdlog::info("[business-start] begin apiBaseUrl={} username={} roomKey={} title={}",
                 toStdString(config.apiBaseUrl),
                 toStdString(config.username),
                 toStdString(config.roomKey),
                 toStdString(config.title));

    resetForNewStart();
    startConfig_ = config;
    snapshot_.apiBaseUrl = config.apiBaseUrl.trimmed();
    startSequenceActive_ = true;
    updateState(BusinessLiveState::logging_in);
    startLoginRequest();
}

void BusinessLiveClient::beginStopSequence() {
    reconnectTimer_.stop();
    joinTimeoutTimer_.stop();
    stopHeartbeat();

    startSequenceActive_ = false;
    awaitingJoinAck_ = false;
    publishingActive_ = false;

    updateState(BusinessLiveState::stopping);

    const QString roomKey = currentRoomKey();
    const QString accessToken = snapshot_.accessToken;
    sendAnchorLeaveBestEffort(roomKey);

    if (socket_.state() != QAbstractSocket::UnconnectedState) {
        suppressDisconnectHandling_ = true;
        socket_.close();
    }

    clearRoomStatePreservingLogin();
    if (!roomKey.isEmpty() && !accessToken.isEmpty()) {
        startCloseRoomRequest(roomKey, accessToken);
    }
}

void BusinessLiveClient::setPublishingActive(bool active) {
    publishingActive_ = active;
}

void BusinessLiveClient::shutdown() {
    reconnectTimer_.stop();
    joinTimeoutTimer_.stop();
    stopHeartbeat();
    startSequenceActive_ = false;
    awaitingJoinAck_ = false;
    publishingActive_ = false;

    if (!currentRoomKey().isEmpty() && socket_.state() == QAbstractSocket::ConnectedState) {
        sendAnchorLeaveBestEffort(currentRoomKey());
    }
    suppressDisconnectHandling_ = true;
    socket_.close();
}

void BusinessLiveClient::resetForNewStart() {
    reconnectTimer_.stop();
    joinTimeoutTimer_.stop();
    stopHeartbeat();

    startSequenceActive_ = false;
    awaitingJoinAck_ = false;
    publishingActive_ = false;
    reconnectAttempt_ = 0;

    if (socket_.state() != QAbstractSocket::UnconnectedState) {
        suppressDisconnectHandling_ = true;
        socket_.close();
    }

    snapshot_ = BusinessLiveSnapshot{};
    emitStateChanged();
}

void BusinessLiveClient::clearRoomStatePreservingLogin() {
    snapshot_.roomKey.clear();
    snapshot_.title.clear();
    snapshot_.signalingUrl.clear();
    snapshot_.businessWsConnected = false;
    snapshot_.anchorJoined = false;
    snapshot_.members.clear();
    snapshot_.lastError.clear();
    snapshot_.lastWarning.clear();

    if (snapshot_.accessToken.isEmpty() || snapshot_.userId <= 0) {
        updateState(BusinessLiveState::idle);
    } else {
        updateState(BusinessLiveState::logged_in);
    }
}

void BusinessLiveClient::emitStateChanged() {
    emit stateChanged();
}

void BusinessLiveClient::updateState(BusinessLiveState state) {
    snapshot_.state = state;
    emitStateChanged();
}

void BusinessLiveClient::failStartSequence(const QString& message) {
    spdlog::error("[business-start] failed roomKey={} message={}",
                  toStdString(currentRoomKey()),
                  toStdString(message));
    reconnectTimer_.stop();
    joinTimeoutTimer_.stop();
    stopHeartbeat();

    startSequenceActive_ = false;
    awaitingJoinAck_ = false;
    snapshot_.businessWsConnected = false;
    snapshot_.anchorJoined = false;
    snapshot_.lastError = message.trimmed();

    if (socket_.state() != QAbstractSocket::UnconnectedState) {
        suppressDisconnectHandling_ = true;
        socket_.close();
    }

    updateState(BusinessLiveState::error);
    emit errorRaised(snapshot_.lastError);
}

void BusinessLiveClient::raiseWarning(const QString& message) {
    spdlog::warn("[business-start] warning roomKey={} message={}",
                 toStdString(currentRoomKey()),
                 toStdString(message));
    snapshot_.lastWarning = message.trimmed();
    emitStateChanged();
    emit warningRaised(snapshot_.lastWarning);
}

void BusinessLiveClient::startLoginRequest() {
    const QUrl url = apiUrlForPath("/api/auth/login");
    if (!url.isValid()) {
        failStartSequence("API Base URL is invalid. Login request was not sent.");
        return;
    }

    spdlog::info("[business-http] login url={} username={}",
                 toStdString(url.toString(QUrl::FullyEncoded)),
                 toStdString(startConfig_.username.trimmed()));

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    const QJsonObject payload{
        {"username", startConfig_.username.trimmed()},
        {"password", startConfig_.password},
    };

    auto* reply = network_.post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const ParsedReply parsed = parseReply(reply);
        reply->deleteLater();

        spdlog::info("[business-http] login response status={} error={}",
                     parsed.statusCode,
                     toStdString(parsed.errorMessage));

        if (parsed.statusCode < 200 || parsed.statusCode >= 300) {
            failStartSequence(QString("Login failed: %1").arg(parsed.errorMessage));
            return;
        }

        const QJsonObject userObject = parsed.object.value("user").toObject();
        const QString accessToken = parsed.object.value("accessToken").toString().trimmed();
        const qint64 userId = static_cast<qint64>(userObject.value("id").toDouble());
        const QString username = userObject.value("username").toString().trimmed();
        if (accessToken.isEmpty() || userId <= 0 || username.isEmpty()) {
            failStartSequence("Login response is missing accessToken or user info.");
            return;
        }

        snapshot_.accessToken = accessToken;
        snapshot_.userId = userId;
        snapshot_.username = username;
        snapshot_.lastError.clear();
        updateState(BusinessLiveState::logged_in);
        startCreateRoomRequest();
    });
}

void BusinessLiveClient::startCreateRoomRequest() {
    updateState(BusinessLiveState::creating_room);

    const QUrl url = apiUrlForPath("/api/live-rooms");
    if (!url.isValid()) {
        failStartSequence("API Base URL is invalid. Live room creation was not sent.");
        return;
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", QByteArray("Bearer ") + snapshot_.accessToken.toUtf8());

    QJsonObject payload;
    if (!startConfig_.roomKey.trimmed().isEmpty()) {
        payload.insert("roomKey", startConfig_.roomKey.trimmed());
    }
    if (!startConfig_.title.trimmed().isEmpty()) {
        payload.insert("title", startConfig_.title.trimmed());
    }

    auto* reply = network_.post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const ParsedReply parsed = parseReply(reply);
        reply->deleteLater();

        spdlog::info("[business-http] create-room response status={} error={}",
                     parsed.statusCode,
                     toStdString(parsed.errorMessage));

        if (parsed.statusCode == 409 &&
            parsed.errorMessage.contains("user already has a live room", Qt::CaseInsensitive)) {
            startExistingRoomLookup();
            return;
        }

        if (parsed.statusCode < 200 || parsed.statusCode >= 300) {
            failStartSequence(QString("Create live room failed: %1").arg(parsed.errorMessage));
            return;
        }

        if (!applyRoomFromResponse(parsed.object.value("room").toObject())) {
            return;
        }
        openBusinessSocket();
    });
}

void BusinessLiveClient::startExistingRoomLookup() {
    const QUrl url = apiUrlForPath("/api/live-rooms");
    if (!url.isValid()) {
        failStartSequence("API Base URL is invalid. Existing live room lookup was not sent.");
        return;
    }

    QNetworkRequest request(url);
    request.setRawHeader("Authorization", QByteArray("Bearer ") + snapshot_.accessToken.toUtf8());

    auto* reply = network_.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const ParsedReply parsed = parseReply(reply);
        reply->deleteLater();

        spdlog::info("[business-http] list-rooms response status={} error={}",
                     parsed.statusCode,
                     toStdString(parsed.errorMessage));

        if (parsed.statusCode < 200 || parsed.statusCode >= 300) {
            failStartSequence(QString("List live rooms failed: %1").arg(parsed.errorMessage));
            return;
        }

        const QJsonArray items = parsed.object.value("items").toArray();
        for (const QJsonValue& value : items) {
            const QJsonObject roomObject = value.toObject();
            const QJsonObject ownerObject = roomObject.value("owner").toObject();
            const qint64 ownerId = static_cast<qint64>(ownerObject.value("id").toDouble());
            const QString status = roomObject.value("status").toString().trimmed();
            if (ownerId == snapshot_.userId && QString::compare(status, "live", Qt::CaseInsensitive) == 0) {
                if (!applyRoomFromResponse(roomObject)) {
                    return;
                }
                openBusinessSocket();
                return;
            }
        }

        failStartSequence("Server reported an existing live room, but no reusable live room owned by this user was found.");
    });
}

void BusinessLiveClient::startCloseRoomRequest(const QString& roomKey, const QString& accessToken) {
    const QUrl url = apiUrlForPath(QString("/api/live-rooms/%1/close").arg(roomKey));
    if (!url.isValid()) {
        raiseWarning(QString("close room skipped: invalid API Base URL for roomKey=%1").arg(roomKey));
        return;
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", QByteArray("Bearer ") + accessToken.toUtf8());

    auto* reply = network_.post(request, QByteArray("{}"));
    connect(reply, &QNetworkReply::finished, this, [this, reply, roomKey]() {
        const ParsedReply parsed = parseReply(reply);
        reply->deleteLater();

        if (parsed.statusCode < 200 || parsed.statusCode >= 300) {
            raiseWarning(QString("Close live room failed (warning only): roomKey=%1, error=%2")
                             .arg(roomKey, parsed.errorMessage));
        }
    });
}

void BusinessLiveClient::openBusinessSocket() {
    const QString signalingUrl = snapshot_.signalingUrl.trimmed().isEmpty() ? derivedSignalingUrl() : snapshot_.signalingUrl;
    if (signalingUrl.trimmed().isEmpty()) {
        failStartSequence("Missing signalingUrl. Business WebSocket cannot connect.");
        return;
    }

    snapshot_.signalingUrl = signalingUrl;
    snapshot_.businessWsConnected = false;
    snapshot_.anchorJoined = false;
    updateState(BusinessLiveState::ws_connecting);

    if (socket_.state() != QAbstractSocket::UnconnectedState) {
        suppressDisconnectHandling_ = true;
        socket_.close();
    }

    spdlog::info("[business-ws] opening url={} roomKey={} userId={}",
                 toStdString(signalingUrl),
                 toStdString(currentRoomKey()),
                 snapshot_.userId);
    socket_.open(QUrl(signalingUrl));
}

void BusinessLiveClient::scheduleReconnect() {
    if (!shouldReconnect()) {
        return;
    }

    reconnectAttempt_ = std::min(reconnectAttempt_ + 1, 6);
    const int delayMs = std::min(kReconnectDelayMs * reconnectAttempt_, kReconnectDelayMaxMs);
    if (!reconnectTimer_.isActive()) {
        reconnectTimer_.start(delayMs);
    }
    updateState(BusinessLiveState::reconnecting);
}

void BusinessLiveClient::stopHeartbeat() {
    heartbeatTimer_.stop();
}

void BusinessLiveClient::sendAnchorJoin() {
    if (snapshot_.accessToken.trimmed().isEmpty() || currentRoomKey().isEmpty()) {
        failStartSequence("Business join requires accessToken and roomKey.");
        return;
    }

    const QJsonObject joinObject{
        {"type", "live.anchor.join"},
        {"token", snapshot_.accessToken},
        {"roomKey", currentRoomKey()},
        {"role", "anchor"},
    };

    awaitingJoinAck_ = true;
    joinTimeoutTimer_.start();
    spdlog::info("[business-ws] send live.anchor.join roomKey={} tokenPresent=yes",
                 toStdString(currentRoomKey()));
    socket_.sendTextMessage(QString::fromUtf8(QJsonDocument(joinObject).toJson(QJsonDocument::Compact)));
}

void BusinessLiveClient::sendAnchorLeaveBestEffort(const QString& roomKey) {
    if (roomKey.trimmed().isEmpty() || socket_.state() != QAbstractSocket::ConnectedState) {
        return;
    }

    const QJsonObject leaveObject{
        {"type", "live.anchor.leave"},
        {"roomKey", roomKey.trimmed()},
    };

    socket_.sendTextMessage(QString::fromUtf8(QJsonDocument(leaveObject).toJson(QJsonDocument::Compact)));
}

void BusinessLiveClient::handleSocketTextMessage(const QString& message) {
    const QJsonDocument document = QJsonDocument::fromJson(message.toUtf8());
    if (!document.isObject()) {
        spdlog::warn("[business-ws] received non-object message");
        return;
    }

    const QJsonObject object = document.object();
    const QString type = messageTypeFromJson(object);
    spdlog::info("[business-ws] recv type={} roomKey={}",
                 toStdString(type),
                 toStdString(object.value("roomKey").toString().trimmed()));
    if (type == "live.anchor.joined") {
        handleAnchorJoined(object);
        return;
    }
    if (type == "live.anchor.error") {
        handleAnchorError(object);
        return;
    }
    if (type == "room.member-list" && object.contains("roomKey")) {
        handleRoomMemberList(object);
        emit signalingMessageReceived(object);
        return;
    }
    if (type == "room.member-join" && object.contains("roomKey")) {
        handleRoomMemberJoin(object);
        emit signalingMessageReceived(object);
        return;
    }
    if (type == "room.member-leave" && object.contains("roomKey")) {
        handleRoomMemberLeave(object);
        emit signalingMessageReceived(object);
        return;
    }
    if (isLinkMicOrRtcSignalType(type)) {
        emit signalingMessageReceived(object);
    }
}

void BusinessLiveClient::handleSocketConnected() {
    spdlog::info("[business-ws] connected url={} roomKey={}",
                 toStdString(snapshot_.signalingUrl),
                 toStdString(currentRoomKey()));
    reconnectTimer_.stop();
    snapshot_.businessWsConnected = true;
    snapshot_.lastError.clear();
    reconnectAttempt_ = 0;
    updateState(BusinessLiveState::ws_connected);
    sendAnchorJoin();
}

void BusinessLiveClient::handleSocketDisconnected() {
    spdlog::warn("[business-ws] disconnected state={} roomKey={} awaitingJoinAck={} startSequenceActive={} publishingActive={} error={}",
                 static_cast<int>(socket_.state()),
                 toStdString(currentRoomKey()),
                 awaitingJoinAck_ ? 1 : 0,
                 startSequenceActive_ ? 1 : 0,
                 publishingActive_ ? 1 : 0,
                 toStdString(socket_.errorString()));
    joinTimeoutTimer_.stop();
    stopHeartbeat();
    snapshot_.businessWsConnected = false;
    snapshot_.anchorJoined = false;

    if (suppressDisconnectHandling_) {
        suppressDisconnectHandling_ = false;
        emitStateChanged();
        return;
    }

    if (startSequenceActive_ && !publishingActive_) {
        failStartSequence("Business WebSocket disconnected before live.anchor.joined.");
        return;
    }

    if (shouldReconnect()) {
        raiseWarning("Business presence disconnected. Reconnecting in 5 seconds.");
        scheduleReconnect();
        return;
    }

    emitStateChanged();
}

void BusinessLiveClient::handleJoinTimeout() {
    if (!awaitingJoinAck_) {
        return;
    }

    spdlog::warn("[business-ws] join timeout roomKey={}", toStdString(currentRoomKey()));

    awaitingJoinAck_ = false;
    snapshot_.businessWsConnected = false;
    snapshot_.anchorJoined = false;

    if (socket_.state() != QAbstractSocket::UnconnectedState) {
        suppressDisconnectHandling_ = true;
        socket_.close();
    }

    if (shouldReconnect()) {
        raiseWarning("Anchor business join timed out. Retrying business reconnect.");
        scheduleReconnect();
        return;
    }

    failStartSequence("Timed out waiting for live.anchor.joined.");
}

void BusinessLiveClient::handleAnchorJoined(const QJsonObject& object) {
    const QString roomKey = object.value("roomKey").toString().trimmed();
    if (!roomKey.isEmpty()) {
        snapshot_.roomKey = roomKey;
    }

    spdlog::info("[business-ws] anchor joined roomKey={} userId={}",
                 toStdString(snapshot_.roomKey),
                 static_cast<qint64>(object.value("userId").toDouble()));

    awaitingJoinAck_ = false;
    joinTimeoutTimer_.stop();
    snapshot_.businessWsConnected = true;
    snapshot_.anchorJoined = true;
    snapshot_.lastError.clear();
    snapshot_.lastWarning.clear();
    heartbeatTimer_.start();
    updateState(BusinessLiveState::anchor_online);

    if (startSequenceActive_ && !publishingActive_) {
        startSequenceActive_ = false;
        emit readyToPublish();
    }
}

void BusinessLiveClient::handleAnchorError(const QJsonObject& object) {
    const QString message = object.value("message").toString().trimmed();
    spdlog::error("[business-ws] anchor error roomKey={} message={}",
                  toStdString(object.value("roomKey").toString().trimmed()),
                  toStdString(message));
    awaitingJoinAck_ = false;
    joinTimeoutTimer_.stop();
    stopHeartbeat();
    snapshot_.anchorJoined = false;

    if (publishingActive_) {
        raiseWarning(QString("Anchor business join failed. Reconnecting: %1").arg(message));
        if (socket_.state() != QAbstractSocket::UnconnectedState) {
            suppressDisconnectHandling_ = true;
            socket_.close();
        }
        scheduleReconnect();
        return;
    }

    failStartSequence(message.isEmpty() ? QString("Anchor business join failed.") : message);
}

void BusinessLiveClient::handleRoomMemberList(const QJsonObject& object) {
    const QString roomKey = object.value("roomKey").toString().trimmed();
    if (!currentRoomKey().isEmpty() && !roomKey.isEmpty() && roomKey != currentRoomKey()) {
        return;
    }

    QList<BusinessRoomMember> members;
    const QJsonArray memberArray = object.value("members").toArray();
    members.reserve(memberArray.size());
    for (const QJsonValue& value : memberArray) {
        if (const auto member = parseBusinessRoomMember(value); member.has_value()) {
            members.push_back(*member);
        }
    }

    snapshot_.members = members;
    emitStateChanged();
}

void BusinessLiveClient::handleRoomMemberJoin(const QJsonObject& object) {
    const QString roomKey = object.value("roomKey").toString().trimmed();
    if (!currentRoomKey().isEmpty() && !roomKey.isEmpty() && roomKey != currentRoomKey()) {
        return;
    }

    const auto joinedMember = extractBusinessRoomMember(object);
    if (!joinedMember.has_value()) {
        return;
    }

    const auto it = std::find_if(snapshot_.members.begin(),
                                 snapshot_.members.end(),
                                 [&](const BusinessRoomMember& existingMember) {
                                     return existingMember.userId == joinedMember->userId;
                                 });
    if (it == snapshot_.members.end()) {
        snapshot_.members.push_back(*joinedMember);
    } else {
        *it = *joinedMember;
    }
    emitStateChanged();
}

void BusinessLiveClient::handleRoomMemberLeave(const QJsonObject& object) {
    const QString roomKey = object.value("roomKey").toString().trimmed();
    if (!currentRoomKey().isEmpty() && !roomKey.isEmpty() && roomKey != currentRoomKey()) {
        return;
    }

    const qint64 userId = extractBusinessRoomMemberId(object);
    if (userId <= 0) {
        return;
    }

    const auto newEnd = std::remove_if(snapshot_.members.begin(),
                                       snapshot_.members.end(),
                                       [userId](const BusinessRoomMember& member) {
                                           return member.userId == userId;
                                       });
    if (newEnd == snapshot_.members.end()) {
        return;
    }
    snapshot_.members.erase(newEnd, snapshot_.members.end());
    emitStateChanged();
}

bool BusinessLiveClient::applyRoomFromResponse(const QJsonObject& roomObject) {
    const QString roomKey = roomObject.value("roomKey").toString().trimmed();
    if (roomKey.isEmpty()) {
        failStartSequence("Create live room response is missing room.roomKey.");
        return false;
    }

    snapshot_.roomKey = roomKey;
    snapshot_.title = roomObject.value("title").toString().trimmed();
    snapshot_.signalingUrl = roomObject.value("signalingUrl").toString().trimmed();
    if (snapshot_.signalingUrl.isEmpty()) {
        snapshot_.signalingUrl = derivedSignalingUrl();
    }
    snapshot_.members.clear();
    snapshot_.lastError.clear();
    updateState(BusinessLiveState::room_created);
    return true;
}

QString BusinessLiveClient::currentRoomKey() const {
    return snapshot_.roomKey.trimmed();
}

QString BusinessLiveClient::derivedSignalingUrl() const {
    return ::deriveSignalingUrl(snapshot_.apiBaseUrl);
}

QString BusinessLiveClient::normalizedApiBaseUrl() const {
    const QUrl url = normalizeHttpBaseUrl(snapshot_.apiBaseUrl);
    if (!url.isValid()) {
        return {};
    }
    return url.toString(QUrl::FullyEncoded);
}

QUrl BusinessLiveClient::apiUrlForPath(const QString& path) const {
    const QUrl baseUrl(normalizedApiBaseUrl());
    if (!baseUrl.isValid()) {
        return {};
    }

    QUrl url(baseUrl);
    QString normalizedPath = path.trimmed();
    if (!normalizedPath.startsWith('/')) {
        normalizedPath.prepend('/');
    }
    url.setPath(normalizedPath);
    url.setQuery(QString());
    url.setFragment(QString());
    return url;
}

bool BusinessLiveClient::shouldReconnect() const {
    return publishingActive_ && !currentRoomKey().isEmpty() && !snapshot_.signalingUrl.trimmed().isEmpty() &&
           !snapshot_.accessToken.trimmed().isEmpty();
}

bool BusinessLiveClient::sendSocketJsonMessage(const QJsonObject& object) {
    if (socket_.state() != QAbstractSocket::ConnectedState) {
        spdlog::warn("[business-ws] send skipped type={} reason=socket-not-connected",
                     toStdString(messageTypeFromJson(object)));
        return false;
    }

    const QString payload = QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
    if (socket_.sendTextMessage(payload) < 0) {
        spdlog::error("[business-ws] send failed type={} error={}",
                      toStdString(messageTypeFromJson(object)),
                      toStdString(socket_.errorString()));
        return false;
    }

    spdlog::info("[business-ws] send type={} roomKey={} from={} to={} requestId={}",
                 toStdString(messageTypeFromJson(object)),
                 toStdString(firstNonEmptyString(object, {"fromRoomKey", "from_room_key", "roomKey", "room_key"})),
                 toStdString(firstNonEmptyString(object, {"fromUserId", "from_user_id"})),
                 toStdString(firstNonEmptyString(object, {"toUserId", "to_user_id"})),
                 toStdString(firstNonEmptyString(object, {"requestId", "request_id"})));
    return true;
}

BusinessLiveClient::ParsedReply BusinessLiveClient::parseReply(QNetworkReply* reply) {
    ParsedReply parsed;
    if (reply == nullptr) {
        parsed.errorMessage = "empty network reply";
        return parsed;
    }

    parsed.statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    parsed.body = reply->readAll();

    const QJsonDocument document = QJsonDocument::fromJson(parsed.body);
    if (document.isObject()) {
        parsed.object = document.object();
    }

    const QString networkError =
        reply->error() == QNetworkReply::NoError ? QString() : reply->errorString().trimmed();
    parsed.errorMessage = parseReplyErrorMessage(parsed.object, parsed.body, networkError);
    return parsed;
}

}  // namespace live_studio::business
