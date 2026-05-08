#include "linkmic/LinkMicSession.hpp"

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

QString extractRequestId(const QJsonObject& message) {
    return message.value("request_id").toString().trimmed();
}

QString extractRemoteUserId(const QJsonObject& message) {
    return firstString(message, {"from_user_id", "peer_id", "user_id", "member_id"});
}

QString extractStateSyncValue(const QJsonObject& message) {
    if (message.contains("payload") && message.value("payload").isObject()) {
        const QJsonObject payload = message.value("payload").toObject();
        const QString payloadState = payload.value("state").toString().trimmed();
        if (!payloadState.isEmpty()) {
            return payloadState;
        }
    }
    return message.value("state").toString().trimmed();
}

bool messageTargetsLocalUser(const QJsonObject& message, const QString& localUserId) {
    if (!message.contains("to_user_id")) {
        return true;
    }

    const QString toUserId = message.value("to_user_id").toString().trimmed();
    return toUserId.isEmpty() || toUserId == localUserId || toUserId == "*";
}

QString compactJson(const QJsonObject& object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QString joinParamsDetail(const RtcJoinParams& params) {
    if (!params.isValid()) {
        return "{}";
    }
    return joinParamsSummary(params);
}

RtcJoinParams parseJoinParams(const QJsonObject& message) {
    QJsonObject payload;
    if (message.contains("payload") && message.value("payload").isObject()) {
        payload = message.value("payload").toObject();
    }

    RtcJoinParams params;
    params.livekitUrl = firstString(payload, {"livekit_url", "livekitUrl", "url"});
    params.roomName = firstString(payload, {"room_name", "roomName"});
    params.token = firstString(payload, {"token"});
    params.participantRole = firstString(payload, {"participant_role", "participantRole", "role"});
    params.raw = payload;
    return params;
}

bool isTerminalState(const LinkMicRoomState& state) {
    return state.signalState() == state.resetStateForRole() || state.signalState() == "failed";
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

bool LinkMicSession::connectToRoom(const LinkMicSessionConfig& config) {
    if (!config.isValid()) {
        return false;
    }

    config_ = config;
    eventLog_.clear();
    memberDirectory_.clear();
    memberDirectory_.setLocalUserId(config.userId);
    roomState_.reset(config.role);
    roomState_.setConnectionState(SignalingConnectionState::connecting);
    stopRequestTimer();
    appendEvent("local", "session.connect", "connecting to signaling service", roomState_.signalState());
    signalingClient_.connectToServer(config_);
    notifyUpdated();
    return true;
}

void LinkMicSession::disconnectFromRoom() {
    stopRequestTimer();
    signalingClient_.disconnectFromServer();
    roomState_.markDisconnected();
    memberDirectory_.clear();
    appendEvent("local", "session.disconnect", "disconnected from signaling service", roomState_.signalState());
    notifyUpdated();
}

bool LinkMicSession::invite(const QString& targetUserId) {
    const auto targetMember = memberDirectory_.memberById(targetUserId);
    const bool targetInLinkMic = targetMember.has_value() && targetMember->inLinkMic;
    if (targetUserId.trimmed() == config_.userId ||
        !roomState_.canInvite(targetUserId, targetInLinkMic)) {
        return false;
    }

    const QString requestId = roomState_.beginInvite(config_.userId, targetUserId);
    startRequestTimer("waiting for invite response");
    return sendBusinessMessage("linkmic.invite",
                               targetUserId,
                               requestId,
                               {
                                   {"initiator_role", config_.role},
                                   {"display_name", config_.displayName},
                                   {"client_name", "live_studio_panel"}
                               },
                               "sent linkmic.invite");
}

bool LinkMicSession::apply(const QString& targetUserId) {
    const auto targetMember = memberDirectory_.memberById(targetUserId);
    const bool targetInLinkMic = targetMember.has_value() && targetMember->inLinkMic;
    if (targetUserId.trimmed() == config_.userId ||
        !roomState_.canApply(targetUserId, targetInLinkMic)) {
        return false;
    }

    const QString requestId = roomState_.beginApply(config_.userId, targetUserId);
    startRequestTimer("waiting for host response");
    return sendBusinessMessage("linkmic.apply",
                               targetUserId,
                               requestId,
                               {
                                   {"initiator_role", config_.role},
                                   {"display_name", config_.displayName},
                                   {"client_name", "live_studio_panel"}
                               },
                               "sent linkmic.apply");
}

bool LinkMicSession::accept() {
    if (!roomState_.canAccept()) {
        return false;
    }

    const QString requestId = roomState_.activeRequestId();
    const QString remoteUserId = roomState_.remoteUserId();
    const QString acceptedFrom = roomState_.incomingRequestType();
    roomState_.handleOutgoingAccept();
    startRequestTimer("waiting for rtc.join-params");

    return sendBusinessMessage("linkmic.accept",
                               remoteUserId,
                               requestId,
                               {
                                   {"decision", "accept"},
                                   {"accepted_from", acceptedFrom},
                                   {"client_name", "live_studio_panel"}
                               },
                               "sent linkmic.accept");
}

bool LinkMicSession::reject() {
    if (!roomState_.canReject()) {
        return false;
    }

    const QString requestId = roomState_.activeRequestId();
    const QString remoteUserId = roomState_.remoteUserId();
    const QString rejectedFrom = roomState_.incomingRequestType();
    roomState_.handleOutgoingReject();
    stopRequestTimer();

    return sendBusinessMessage("linkmic.reject",
                               remoteUserId,
                               requestId,
                               {
                                   {"decision", "reject"},
                                   {"rejected_from", rejectedFrom},
                                   {"client_name", "live_studio_panel"}
                               },
                               "sent linkmic.reject");
}

bool LinkMicSession::hangup() {
    if (!roomState_.canTerminate()) {
        return false;
    }

    const QString requestId = roomState_.activeRequestId();
    const QString remoteUserId = roomState_.remoteUserId();
    const QString currentState = roomState_.signalState();
    const bool shouldCancel =
        currentState == "host-inviting" || currentState == "waiting-host";
    roomState_.handleOutgoingTerminate();
    stopRequestTimer();

    return sendBusinessMessage(shouldCancel ? "linkmic.cancel" : "linkmic.hangup",
                               remoteUserId,
                               requestId,
                               {
                                   {"reason", shouldCancel ? "manual-cancel" : "manual-hangup"},
                                   {"client_name", "live_studio_panel"}
                               },
                               shouldCancel ? "sent linkmic.cancel" : "sent linkmic.hangup");
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
                    roomState_.activeRequestId(),
                    roomState_.remoteUserId());
        break;
    }

    notifyUpdated();
}

void LinkMicSession::handleObservedMessage(const QString& direction, const QJsonObject& message) {
    const QString type = message.value("type").toString().trimmed();
    if (type == "probe.register") {
        appendEvent(direction,
                    type,
                    "registered local client with signaling service",
                    roomState_.signalState(),
                    extractRequestId(message),
                    message.value("peer_id").toString().trimmed());
        notifyUpdated();
    }
}

void LinkMicSession::handleIncomingMessage(const QJsonObject& message) {
    const QString type = message.value("type").toString().trimmed();
    const QString requestId = extractRequestId(message);
    const QString remoteUserId = extractRemoteUserId(message);

    if (type == "signal.error") {
        stopRequestTimer();
        roomState_.failSession(firstString(message, {"message"}));
        appendEvent("recv",
                    type,
                    roomState_.lastError(),
                    roomState_.signalState(),
                    requestId,
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
                    requestId,
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
        appendEvent("recv",
                    type,
                    "member joined room",
                    roomState_.signalState(),
                    requestId,
                    remoteUserId);
        notifyUpdated();
        return;
    }

    if (type == "room.member-leave") {
        memberDirectory_.applyLeaveFromMessage(message);
        appendEvent("recv",
                    type,
                    "member left room",
                    roomState_.signalState(),
                    requestId,
                    remoteUserId);
        if (!roomState_.remoteUserId().isEmpty() && roomState_.remoteUserId() == remoteUserId) {
            stopRequestTimer();
            roomState_.handleHangupOrKick(roomState_.activeRequestId(), remoteUserId);
            memberDirectory_.clearLinkMicFlags();
            appendEvent("local",
                        "linkmic.reset",
                        "active partner left the room",
                        roomState_.signalState(),
                        requestId,
                        remoteUserId);
        }
        notifyUpdated();
        return;
    }

    if (!messageTargetsLocalUser(message, config_.userId)) {
        return;
    }

    if (remoteUserId == config_.userId && !remoteUserId.isEmpty()) {
        return;
    }

    if (!roomState_.matchesRequestId(requestId) &&
        (type.startsWith("linkmic.") || type == "rtc.join-params")) {
        appendEvent("recv",
                    type,
                    "ignored message for another active request",
                    roomState_.signalState(),
                    requestId,
                    remoteUserId);
        notifyUpdated();
        return;
    }

    if (type == "linkmic.apply") {
        stopRequestTimer();
        roomState_.handleIncomingApply(requestId, remoteUserId);
        appendEvent("recv",
                    type,
                    "incoming guest apply",
                    roomState_.signalState(),
                    requestId,
                    remoteUserId);
        notifyUpdated();
        return;
    }

    if (type == "linkmic.invite") {
        stopRequestTimer();
        roomState_.handleIncomingInvite(requestId, remoteUserId);
        appendEvent("recv",
                    type,
                    "incoming host invite",
                    roomState_.signalState(),
                    requestId,
                    remoteUserId);
        notifyUpdated();
        return;
    }

    if (type == "linkmic.accept") {
        roomState_.handleIncomingAccept(requestId, remoteUserId);
        startRequestTimer("waiting for rtc.join-params");
        appendEvent("recv",
                    type,
                    "remote side accepted, waiting for rtc.join-params",
                    roomState_.signalState(),
                    requestId,
                    remoteUserId);
        notifyUpdated();
        return;
    }

    if (type == "rtc.join-params" || type == "linkmic.connected") {
        stopRequestTimer();
        const RtcJoinParams joinParams = parseJoinParams(message);
        roomState_.handleJoinParams(requestId, remoteUserId, joinParams);
        memberDirectory_.clearLinkMicFlags();
        memberDirectory_.setMemberLinkMicState(remoteUserId, true);
        appendEvent("recv",
                    type,
                    joinParamsDetail(joinParams),
                    roomState_.signalState(),
                    requestId,
                    remoteUserId);
        notifyUpdated();
        return;
    }

    if (type == "linkmic.reject" || type == "linkmic.cancel") {
        stopRequestTimer();
        roomState_.handleRejectOrCancel(requestId, remoteUserId);
        memberDirectory_.clearLinkMicFlags();
        appendEvent("recv",
                    type,
                    "request ended before RTC join",
                    roomState_.signalState(),
                    requestId,
                    remoteUserId);
        notifyUpdated();
        return;
    }

    if (type == "linkmic.hangup" || type == "linkmic.kick") {
        stopRequestTimer();
        roomState_.handleHangupOrKick(requestId, remoteUserId);
        memberDirectory_.clearLinkMicFlags();
        appendEvent("recv",
                    type,
                    "linkmic session ended",
                    roomState_.signalState(),
                    requestId,
                    remoteUserId);
        notifyUpdated();
        return;
    }

    if (type == "linkmic.state-sync") {
        const QString syncedState = extractStateSyncValue(message);
        roomState_.handleStateSync(requestId, remoteUserId, syncedState);
        if (roomState_.signalState() == roomState_.activeStateForRole()) {
            memberDirectory_.clearLinkMicFlags();
            memberDirectory_.setMemberLinkMicState(roomState_.remoteUserId(), true);
        } else if (roomState_.signalState() == roomState_.resetStateForRole()) {
            memberDirectory_.clearLinkMicFlags();
            stopRequestTimer();
        }
        appendEvent("recv",
                    type,
                    syncedState.isEmpty() ? compactJson(message) : syncedState,
                    roomState_.signalState(),
                    requestId,
                    remoteUserId);
        notifyUpdated();
        return;
    }

    appendEvent("recv",
                type,
                compactJson(message),
                roomState_.signalState(),
                requestId,
                remoteUserId);
    notifyUpdated();
}

void LinkMicSession::handleRequestTimeout() {
    const QString detail = roomState_.isWaitingJoinParams()
        ? "timed out waiting for rtc.join-params"
        : (requestTimerReason_.trimmed().isEmpty() ? "linkmic request timed out" : requestTimerReason_);
    roomState_.failSession(detail);
    appendEvent("local",
                "timer.timeout",
                detail,
                roomState_.signalState(),
                roomState_.activeRequestId(),
                roomState_.remoteUserId());
    notifyUpdated();
}

void LinkMicSession::appendEvent(const QString& direction,
                                 const QString& type,
                                 const QString& detail,
                                 const QString& state,
                                 const QString& requestId,
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
    entry.requestId = requestId;
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

bool LinkMicSession::sendBusinessMessage(const QString& type,
                                         const QString& toUserId,
                                         const QString& requestId,
                                         const QJsonObject& payload,
                                         const QString& detail) {
    appendEvent("send", type, detail, roomState_.signalState(), requestId, toUserId);

    if (!signalingClient_.sendBusinessMessage(type, toUserId, requestId, payload)) {
        stopRequestTimer();
        roomState_.failSession("failed to send " + type);
        appendEvent("local",
                    "send.failed",
                    roomState_.lastError(),
                    roomState_.signalState(),
                    requestId,
                    toUserId);
        notifyUpdated();
        return false;
    }

    if (isTerminalState(roomState_)) {
        memberDirectory_.clearLinkMicFlags();
    }

    notifyUpdated();
    return true;
}

}  // namespace live_studio::linkmic
