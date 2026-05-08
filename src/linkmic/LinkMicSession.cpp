#include "linkmic/LinkMicSession.hpp"

#include <QJsonArray>
#include <QJsonDocument>

namespace live_studio::linkmic {
namespace {

QString firstString(const QJsonObject& object, std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        const QString value = object.value(QLatin1String(key)).toString().trimmed();
        if (!value.isEmpty()) {
            return value;
        }
    }
    return {};
}

QJsonObject extractPayloadObject(const QJsonObject& message) {
    if (message.contains("payload") && message.value("payload").isObject()) {
        return message.value("payload").toObject();
    }
    return {};
}

QString extractType(const QJsonObject& message) {
    return message.value("type").toString().trimmed();
}

QString extractSessionId(const QJsonObject& message) {
    return firstString(message, {"sessionId", "session_id"});
}

QString extractRequestId(const QJsonObject& message) {
    return firstString(message, {"requestId", "request_id"});
}

QString extractRemoteRoomKey(const QJsonObject& message) {
    return firstString(message, {"fromRoomKey", "from_room_key"});
}

QString extractRemoteUserId(const QJsonObject& message) {
    return firstString(message, {"fromUserId", "from_user_id", "peer_id", "user_id", "member_id"});
}

QString extractToRoomKey(const QJsonObject& message) {
    return firstString(message, {"toRoomKey", "to_room_key"});
}

QString extractToUserId(const QJsonObject& message) {
    return firstString(message, {"toUserId", "to_user_id"});
}

bool messageTargetsLocalUser(const QJsonObject& message,
                             const QString& localRoomKey,
                             const QString& localUserId) {
    const QString toRoomKey = extractToRoomKey(message);
    if (!toRoomKey.isEmpty() && toRoomKey != localRoomKey) {
        return false;
    }

    const QString toUserId = extractToUserId(message);
    return toUserId.isEmpty() || toUserId == localUserId || toUserId == "*";
}

QString compactJson(const QJsonObject& object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

RtcJoinParams parseJoinParams(const QJsonObject& message) {
    const QJsonObject payload = extractPayloadObject(message);

    RtcJoinParams params;
    params.sessionId = firstString(payload, {"sessionId", "session_id"});
    params.requestId = firstString(payload, {"requestId", "request_id"});
    if (params.requestId.isEmpty()) {
        params.requestId = extractRequestId(message);
    }
    params.roomAKey = firstString(payload, {"roomAKey", "room_a_key"});
    params.roomBKey = firstString(payload, {"roomBKey", "room_b_key"});
    params.selfRoomKey = firstString(payload, {"selfRoomKey", "self_room_key"});
    params.peerRoomKey = firstString(payload, {"peerRoomKey", "peer_room_key"});
    params.selfUserId = firstString(payload, {"selfUserId", "self_user_id"});
    params.peerUserId = firstString(payload, {"peerUserId", "peer_user_id"});
    params.businessRole = firstString(payload, {"businessRole", "business_role"});
    params.rtcRole = firstString(payload, {"rtcRole", "rtc_role"});
    params.signalingUrl = firstString(payload, {"signalingUrl", "signaling_url"});
    params.rtcBackend = firstString(payload, {"rtcBackend", "rtc_backend", "transport"});
    params.mediaMode = firstString(payload, {"mediaMode", "media_mode"});
    if (payload.contains("iceServers") && payload.value("iceServers").isArray()) {
        params.iceServers = payload.value("iceServers").toArray();
    } else if (payload.contains("ice_servers") && payload.value("ice_servers").isArray()) {
        params.iceServers = payload.value("ice_servers").toArray();
    }
    params.raw = payload;
    return params;
}

}  // namespace

LinkMicSession::LinkMicSession() {
    signalingClient_.setStateChangedHandler([this](SignalingConnectionState state, const QString& detail) {
        handleTransportStateChanged(state, detail);
    });
    signalingClient_.setObservedMessageHandler([this](const QString& direction, const QJsonObject& message) {
        handleObservedMessage(direction, message);
    });
    signalingClient_.setJsonMessageHandler([this](const QJsonObject& message) {
        handleIncomingMessage(message);
    });

    requestTimer_.setSingleShot(true);
    QObject::connect(&requestTimer_, &QTimer::timeout, [&]() {
        handleRequestTimeout();
    });
}

void LinkMicSession::setUpdateHandler(UpdateHandler handler) {
    onUpdated_ = std::move(handler);
}

void LinkMicSession::setExternalMessageSender(ExternalMessageSender sender) {
    externalMessageSender_ = std::move(sender);
}

void LinkMicSession::setUseExternalTransport(bool enabled) {
    useExternalTransport_ = enabled;
}

void LinkMicSession::setExternalTransportState(SignalingConnectionState state, const QString& detail) {
    if (useExternalTransport_ && externalTransportState_ == state && externalTransportDetail_ == detail) {
        return;
    }
    externalTransportState_ = state;
    externalTransportDetail_ = detail;

    if (!useExternalTransport_) {
        return;
    }

    switch (state) {
    case SignalingConnectionState::connecting:
        roomState_.setConnectionState(state);
        appendEvent("transport", "signaling.connecting", detail, roomState_.signalState());
        break;
    case SignalingConnectionState::connected:
        roomState_.setConnectionState(state);
        roomState_.markRegistered();
        appendEvent("transport", "signaling.connected", detail, roomState_.signalState());
        break;
    case SignalingConnectionState::disconnected:
        stopRequestTimer();
        roomState_.markDisconnected();
        rtcConnectedNotifiedSessionId_.clear();
        appendEvent("transport", "signaling.disconnected", detail, roomState_.signalState());
        break;
    case SignalingConnectionState::failed:
        stopRequestTimer();
        roomState_.failTransport(detail.isEmpty() ? "business signaling failure" : detail);
        appendEvent("transport",
                    "signaling.failed",
                    roomState_.lastError(),
                    roomState_.signalState(),
                    roomState_.activeSessionId(),
                    roomState_.activeRequestId(),
                    roomState_.remoteRoomKey(),
                    roomState_.remoteUserId());
        break;
    }

    notifyUpdated();
}

void LinkMicSession::processExternalMessage(const QJsonObject& message) {
    if (!useExternalTransport_) {
        return;
    }
    handleIncomingMessage(message);
}

bool LinkMicSession::connectToRoom(const LinkMicSessionConfig& config) {
    if (!config.isValid()) {
        return false;
    }

    if (useExternalTransport_ && !externalMessageSender_) {
        return false;
    }

    config_ = config;
    eventLog_.clear();
    memberDirectory_.clear();
    memberDirectory_.setLocalUserId(config.userId);
    roomState_.reset();
    rtcConnectedNotifiedSessionId_.clear();
    stopRequestTimer();

    if (useExternalTransport_) {
        roomState_.setConnectionState(externalTransportState_ == SignalingConnectionState::connected
                                          ? SignalingConnectionState::connected
                                          : SignalingConnectionState::connecting);
        appendEvent("local", "session.connect", "binding to business signaling transport", roomState_.signalState());
        if (externalTransportState_ == SignalingConnectionState::connected) {
            roomState_.markRegistered();
            appendEvent("transport",
                        "signaling.connected",
                        externalTransportDetail_.isEmpty() ? "business websocket connected" : externalTransportDetail_,
                        roomState_.signalState());
        }
    } else {
        roomState_.setConnectionState(SignalingConnectionState::connecting);
        appendEvent("local", "session.connect", "connecting to signaling service", roomState_.signalState());
        signalingClient_.connectToServer(config_);
    }
    notifyUpdated();
    return true;
}

void LinkMicSession::disconnectFromRoom() {
    stopRequestTimer();
    rtcConnectedNotifiedSessionId_.clear();
    if (!useExternalTransport_) {
        signalingClient_.disconnectFromServer();
    }
    roomState_.markDisconnected();
    memberDirectory_.clear();
    appendEvent("local", "session.disconnect", "disconnected from signaling service", roomState_.signalState());
    notifyUpdated();
}

bool LinkMicSession::invite(const QString& targetRoomKey, const QString& targetUserId) {
    if (!roomState_.canInvite(config_.roomKey, config_.userId, targetRoomKey, targetUserId)) {
        return false;
    }

    const LinkMicRequestIds ids = roomState_.beginInvite(config_.roomKey, config_.userId, targetRoomKey, targetUserId);
    startRequestTimer("waiting for invite response");
    return sendEnvelope(
        buildEnvelope("linkmic.invite",
                      ids.sessionId,
                      ids.requestId,
                      targetRoomKey,
                      targetUserId,
                      {
                          {"displayName", config_.displayName},
                          {"display_name", config_.displayName},
                          {"businessRole", "inviter"},
                          {"business_role", "inviter"},
                          {"clientName", "live_studio_panel"},
                          {"client_name", "live_studio_panel"}
                      }),
        "sent cross-room invite");
}

bool LinkMicSession::accept() {
    if (!roomState_.canAccept()) {
        return false;
    }

    const QString sessionId = roomState_.activeSessionId();
    const QString requestId = roomState_.activeRequestId();
    const QString remoteRoomKey = roomState_.remoteRoomKey();
    const QString remoteUserId = roomState_.remoteUserId();
    roomState_.handleOutgoingAccept();
    startRequestTimer("waiting for rtc.join-params");

    return sendEnvelope(
        buildEnvelope("linkmic.accept",
                      sessionId,
                      requestId,
                      remoteRoomKey,
                      remoteUserId,
                      {
                          {"decision", "accept"},
                          {"businessRole", "invitee"},
                          {"business_role", "invitee"},
                          {"clientName", "live_studio_panel"},
                          {"client_name", "live_studio_panel"}
                      }),
        "accepted incoming cross-room invite");
}

bool LinkMicSession::reject() {
    if (!roomState_.canReject()) {
        return false;
    }

    const QString sessionId = roomState_.activeSessionId();
    const QString requestId = roomState_.activeRequestId();
    const QString remoteRoomKey = roomState_.remoteRoomKey();
    const QString remoteUserId = roomState_.remoteUserId();
    roomState_.handleOutgoingReject();
    stopRequestTimer();

    return sendEnvelope(
        buildEnvelope("linkmic.reject",
                      sessionId,
                      requestId,
                      remoteRoomKey,
                      remoteUserId,
                      {
                          {"decision", "reject"},
                          {"businessRole", "invitee"},
                          {"business_role", "invitee"},
                          {"clientName", "live_studio_panel"},
                          {"client_name", "live_studio_panel"}
                      }),
        "rejected incoming cross-room invite");
}

bool LinkMicSession::hangup() {
    if (!roomState_.canTerminate()) {
        return false;
    }

    const QString sessionId = roomState_.activeSessionId();
    const QString requestId = roomState_.activeRequestId();
    const QString remoteRoomKey = roomState_.remoteRoomKey();
    const QString remoteUserId = roomState_.remoteUserId();
    const bool shouldCancel = roomState_.signalState() == LinkMicRoomState::waitingResponseState();
    roomState_.handleOutgoingTerminate();
    stopRequestTimer();

    const bool sent = sendEnvelope(
        buildEnvelope(shouldCancel ? "linkmic.cancel" : "linkmic.hangup",
                      sessionId,
                      requestId,
                      remoteRoomKey,
                      remoteUserId,
                      {
                          {"reason", shouldCancel ? "manual-cancel" : "manual-hangup"},
                          {"clientName", "live_studio_panel"},
                          {"client_name", "live_studio_panel"}
                      }),
        shouldCancel ? "cancelled outgoing invite" : "hung up active cross-room session");
    if (sent) {
        roomState_.handleHangupOrKick(sessionId, requestId, remoteRoomKey, remoteUserId);
        rtcConnectedNotifiedSessionId_.clear();
        notifyUpdated();
    }
    return sent;
}

bool LinkMicSession::noteRtcConnected() {
    const QString sessionId = roomState_.activeSessionId();
    if (sessionId.isEmpty() || rtcConnectedNotifiedSessionId_ == sessionId) {
        return false;
    }

    rtcConnectedNotifiedSessionId_ = sessionId;
    roomState_.handleRtcConnected();
    appendEvent("local",
                "rtc.connected",
                "local PeerConnection connected",
                roomState_.signalState(),
                sessionId,
                roomState_.activeRequestId(),
                roomState_.remoteRoomKey(),
                roomState_.remoteUserId());

    const bool sent = sendEnvelope(
        buildEnvelope("linkmic.connected",
                      sessionId,
                      roomState_.activeRequestId(),
                      roomState_.remoteRoomKey(),
                      roomState_.remoteUserId(),
                      {
                          {"state", "connected"},
                          {"rtcRole", roomState_.rtcRole()},
                          {"rtc_role", roomState_.rtcRole()},
                          {"clientName", "live_studio_panel"},
                          {"client_name", "live_studio_panel"}
                      }),
        "reported local PeerConnection connected");
    notifyUpdated();
    return sent;
}

const LinkMicSessionConfig& LinkMicSession::config() const {
    return config_;
}

const RoomMemberDirectory& LinkMicSession::memberDirectory() const {
    return memberDirectory_;
}

const LinkMicRoomState& LinkMicSession::roomState() const {
    return roomState_;
}

const QVector<LinkMicEventLogEntry>& LinkMicSession::eventLog() const {
    return eventLog_;
}

bool LinkMicSession::usesExternalTransport() const {
    return useExternalTransport_;
}

void LinkMicSession::handleTransportStateChanged(SignalingConnectionState state, const QString& detail) {
    switch (state) {
    case SignalingConnectionState::connecting:
        roomState_.setConnectionState(state);
        appendEvent("transport", "signaling.connecting", detail, roomState_.signalState());
        break;
    case SignalingConnectionState::connected:
        roomState_.setConnectionState(state);
        appendEvent("transport", "signaling.connected", detail, roomState_.signalState());
        break;
    case SignalingConnectionState::disconnected:
        stopRequestTimer();
        rtcConnectedNotifiedSessionId_.clear();
        roomState_.markDisconnected();
        memberDirectory_.clear();
        appendEvent("transport", "signaling.disconnected", detail, roomState_.signalState());
        break;
    case SignalingConnectionState::failed:
        stopRequestTimer();
        roomState_.failTransport(detail.isEmpty() ? "signaling failure" : detail);
        appendEvent("transport",
                    "signaling.failed",
                    roomState_.lastError(),
                    roomState_.signalState(),
                    roomState_.activeSessionId(),
                    roomState_.activeRequestId(),
                    roomState_.remoteRoomKey(),
                    roomState_.remoteUserId());
        break;
    }

    notifyUpdated();
}

void LinkMicSession::handleObservedMessage(const QString& direction, const QJsonObject& message) {
    const QString type = extractType(message);
    if (type == "probe.register") {
        appendEvent(direction,
                    type,
                    "registered local client with signaling service",
                    roomState_.signalState(),
                    extractSessionId(message),
                    extractRequestId(message),
                    extractRemoteRoomKey(message),
                    extractRemoteUserId(message));
        notifyUpdated();
    }
}

void LinkMicSession::handleIncomingMessage(const QJsonObject& message) {
    const QString type = extractType(message);
    const QString sessionId = extractSessionId(message);
    const QString requestId = extractRequestId(message);
    const QString remoteRoomKey = extractRemoteRoomKey(message);
    const QString remoteUserId = extractRemoteUserId(message);

    if (type == "signal.error") {
        stopRequestTimer();
        roomState_.recoverFromSignalError(firstString(message, {"message"}));
        appendEvent("recv",
                    type,
                    roomState_.lastError(),
                    roomState_.signalState(),
                    sessionId,
                    requestId,
                    remoteRoomKey,
                    remoteUserId);
        notifyUpdated();
        return;
    }

    if (type == "probe.registered") {
        roomState_.markRegistered();
        appendEvent("recv",
                    type,
                    "registration acknowledged by signaling service",
                    roomState_.signalState(),
                    sessionId,
                    requestId,
                    remoteRoomKey,
                    remoteUserId);
        notifyUpdated();
        return;
    }

    if (type == "room.member-list") {
        memberDirectory_.replaceFromMessage(message);
        appendEvent("recv",
                    type,
                    QString("online_count=%1").arg(memberDirectory_.onlineCount()),
                    roomState_.signalState());
        notifyUpdated();
        return;
    }

    if (type == "room.member-join") {
        memberDirectory_.applyJoinFromMessage(message);
        appendEvent("recv", type, "member joined current room", roomState_.signalState());
        notifyUpdated();
        return;
    }

    if (type == "room.member-leave") {
        memberDirectory_.applyLeaveFromMessage(message);
        appendEvent("recv", type, "member left current room", roomState_.signalState());
        notifyUpdated();
        return;
    }

    if (!messageTargetsLocalUser(message, config_.roomKey, config_.userId)) {
        return;
    }

    if (!roomState_.matchesSession(sessionId, requestId) &&
        (type.startsWith("linkmic.") || type.startsWith("rtc."))) {
        appendEvent("recv",
                    type,
                    "ignored message for another cross-room session",
                    roomState_.signalState(),
                    sessionId,
                    requestId,
                    remoteRoomKey,
                    remoteUserId);
        notifyUpdated();
        return;
    }

    if (type == "linkmic.invite") {
        stopRequestTimer();
        roomState_.handleIncomingInvite(sessionId, requestId, remoteRoomKey, remoteUserId);
        appendEvent("recv",
                    type,
                    "incoming cross-room invite",
                    roomState_.signalState(),
                    sessionId,
                    requestId,
                    remoteRoomKey,
                    remoteUserId);
        notifyUpdated();
        return;
    }

    if (type == "linkmic.accept") {
        roomState_.handleIncomingAccept(sessionId, requestId, remoteRoomKey, remoteUserId);
        startRequestTimer("waiting for rtc.join-params");
        appendEvent("recv",
                    type,
                    "remote anchor accepted, waiting for rtc.join-params",
                    roomState_.signalState(),
                    sessionId,
                    requestId,
                    remoteRoomKey,
                    remoteUserId);
        notifyUpdated();
        return;
    }

    if (type == "rtc.join-params") {
        stopRequestTimer();
        const RtcJoinParams joinParams = parseJoinParams(message);
        roomState_.handleJoinParams(sessionId, requestId, remoteRoomKey, remoteUserId, joinParams);
        appendEvent("recv",
                    type,
                    joinParamsSummary(joinParams),
                    roomState_.signalState(),
                    sessionId,
                    requestId,
                    remoteRoomKey,
                    remoteUserId);
        notifyUpdated();
        return;
    }

    if (type == "linkmic.connected") {
        roomState_.handleRtcConnected();
        appendEvent("recv",
                    type,
                    "peer connection marked connected by remote anchor",
                    roomState_.signalState(),
                    sessionId,
                    requestId,
                    remoteRoomKey,
                    remoteUserId);
        notifyUpdated();
        return;
    }

    if (type == "linkmic.reject" || type == "linkmic.cancel") {
        stopRequestTimer();
        rtcConnectedNotifiedSessionId_.clear();
        roomState_.handleRejectOrCancel(sessionId, requestId, remoteRoomKey, remoteUserId);
        appendEvent("recv",
                    type,
                    "cross-room invite ended before RTC setup",
                    roomState_.signalState(),
                    sessionId,
                    requestId,
                    remoteRoomKey,
                    remoteUserId);
        notifyUpdated();
        return;
    }

    if (type == "linkmic.hangup" || type == "linkmic.kick") {
        stopRequestTimer();
        rtcConnectedNotifiedSessionId_.clear();
        roomState_.handleHangupOrKick(sessionId, requestId, remoteRoomKey, remoteUserId);
        appendEvent("recv",
                    type,
                    "cross-room RTC session ended",
                    roomState_.signalState(),
                    sessionId,
                    requestId,
                    remoteRoomKey,
                    remoteUserId);
        notifyUpdated();
        return;
    }

    appendEvent("recv",
                type,
                compactJson(message),
                roomState_.signalState(),
                sessionId,
                requestId,
                remoteRoomKey,
                remoteUserId);
    notifyUpdated();
}

void LinkMicSession::handleRequestTimeout() {
    const QString detail = roomState_.isWaitingJoinParams()
        ? "timed out waiting for rtc.join-params"
        : (requestTimerReason_.trimmed().isEmpty() ? "cross-room request timed out" : requestTimerReason_);
    roomState_.failSession(detail);
    appendEvent("local",
                "timer.timeout",
                detail,
                roomState_.signalState(),
                roomState_.activeSessionId(),
                roomState_.activeRequestId(),
                roomState_.remoteRoomKey(),
                roomState_.remoteUserId());
    notifyUpdated();
}

void LinkMicSession::appendEvent(const QString& direction,
                                 const QString& type,
                                 const QString& detail,
                                 const QString& state,
                                 const QString& sessionId,
                                 const QString& requestId,
                                 const QString& remoteRoomKey,
                                 const QString& remoteUserId) {
    if (eventLog_.size() >= 200) {
        eventLog_.removeFirst();
    }

    LinkMicEventLogEntry entry;
    entry.timestampUtc = QDateTime::currentDateTimeUtc();
    entry.direction = direction;
    entry.type = type;
    entry.detail = detail;
    entry.state = state;
    entry.sessionId = sessionId;
    entry.requestId = requestId;
    entry.remoteRoomKey = remoteRoomKey;
    entry.remoteUserId = remoteUserId;
    eventLog_.push_back(std::move(entry));
}

void LinkMicSession::notifyUpdated() const {
    if (onUpdated_) {
        onUpdated_();
    }
}

void LinkMicSession::startRequestTimer(const QString& reason) {
    requestTimerReason_ = reason;
    if (config_.requestTimeoutMs > 0) {
        requestTimer_.start(config_.requestTimeoutMs);
    }
}

void LinkMicSession::stopRequestTimer() {
    requestTimerReason_.clear();
    requestTimer_.stop();
}

QJsonObject LinkMicSession::buildEnvelope(const QString& type,
                                          const QString& sessionId,
                                          const QString& requestId,
                                          const QString& toRoomKey,
                                          const QString& toUserId,
                                          const QJsonObject& payload) const {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    return QJsonObject{
        {"type", type},
        {"sessionId", sessionId},
        {"session_id", sessionId},
        {"fromRoomKey", config_.roomKey},
        {"from_room_key", config_.roomKey},
        {"toRoomKey", toRoomKey.trimmed()},
        {"to_room_key", toRoomKey.trimmed()},
        {"fromUserId", config_.userId},
        {"from_user_id", config_.userId},
        {"toUserId", toUserId.trimmed()},
        {"to_user_id", toUserId.trimmed()},
        {"requestId", requestId},
        {"request_id", requestId},
        {"payload", payload},
        {"tsMs", nowMs},
        {"ts_ms", nowMs}
    };
}

bool LinkMicSession::sendEnvelope(const QJsonObject& envelope, const QString& detail) {
    appendEvent("send",
                envelope.value("type").toString().trimmed(),
                detail,
                roomState_.signalState(),
                extractSessionId(envelope),
                extractRequestId(envelope),
                firstString(envelope, {"toRoomKey", "to_room_key"}),
                firstString(envelope, {"toUserId", "to_user_id"}));

    const bool sent = useExternalTransport_
        ? externalMessageSender_(envelope)
        : signalingClient_.sendMessage(envelope);
    if (!sent) {
        stopRequestTimer();
        roomState_.failSession("failed to send " + envelope.value("type").toString().trimmed());
        appendEvent("local",
                    "send.failed",
                    roomState_.lastError(),
                    roomState_.signalState(),
                    extractSessionId(envelope),
                    extractRequestId(envelope),
                    firstString(envelope, {"toRoomKey", "to_room_key"}),
                    firstString(envelope, {"toUserId", "to_user_id"}));
        notifyUpdated();
        return false;
    }

    notifyUpdated();
    return true;
}

}  // namespace live_studio::linkmic
