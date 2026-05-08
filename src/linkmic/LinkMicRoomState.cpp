#include "linkmic/LinkMicRoomState.hpp"

#include <QDateTime>

namespace live_studio::linkmic {

LinkMicRoomState::LinkMicRoomState() {
    reset();
}

void LinkMicRoomState::reset() {
    signalState_ = idleState();
    lastError_.clear();
    connectionState_ = SignalingConnectionState::disconnected;
    registered_ = false;
    clearRequestState();
}

void LinkMicRoomState::markRegistered() {
    registered_ = true;
    lastError_.clear();
}

void LinkMicRoomState::markDisconnected() {
    connectionState_ = SignalingConnectionState::disconnected;
    registered_ = false;
    signalState_ = idleState();
    lastError_.clear();
    clearRequestState();
}

void LinkMicRoomState::setConnectionState(SignalingConnectionState state) {
    connectionState_ = state;
    if (state == SignalingConnectionState::connected) {
        lastError_.clear();
    }
}

void LinkMicRoomState::failTransport(const QString& error) {
    connectionState_ = SignalingConnectionState::failed;
    signalState_ = "failed";
    lastError_ = error.trimmed();
}

void LinkMicRoomState::failSession(const QString& error) {
    signalState_ = "failed";
    lastError_ = error.trimmed();
    waitingJoinParams_ = false;
}

void LinkMicRoomState::recoverFromSignalError(const QString& error) {
    signalState_ = idleState();
    lastError_ = error.trimmed();
    clearRequestState();
}

LinkMicRequestIds LinkMicRoomState::beginInvite(const QString& localRoomKey,
                                                const QString& localUserId,
                                                const QString& targetRoomKey,
                                                const QString& targetUserId) {
    lastError_.clear();
    activeSessionId_ = nextSessionId(localRoomKey, localUserId, targetRoomKey, targetUserId);
    activeRequestId_ = nextRequestId(localRoomKey, localUserId, targetRoomKey, targetUserId);
    businessRole_ = "inviter";
    rtcRole_.clear();
    remoteRoomKey_ = targetRoomKey.trimmed();
    remoteUserId_ = targetUserId.trimmed();
    waitingJoinParams_ = false;
    hasJoinParams_ = false;
    joinParams_ = {};
    signalState_ = waitingResponseState();
    return {activeSessionId_, activeRequestId_};
}

void LinkMicRoomState::handleOutgoingAccept() {
    signalState_ = waitingJoinParamsState();
    businessRole_ = "invitee";
    waitingJoinParams_ = true;
    hasJoinParams_ = false;
    joinParams_ = {};
    lastError_.clear();
}

void LinkMicRoomState::handleOutgoingReject() {
    signalState_ = idleState();
    lastError_.clear();
    clearRequestState();
}

void LinkMicRoomState::handleOutgoingTerminate() {
    signalState_ = endingState();
    waitingJoinParams_ = false;
    lastError_.clear();
}

void LinkMicRoomState::handleIncomingInvite(const QString& sessionId,
                                            const QString& requestId,
                                            const QString& remoteRoomKey,
                                            const QString& remoteUserId) {
    lastError_.clear();
    activeSessionId_ = sessionId.trimmed();
    activeRequestId_ = requestId.trimmed();
    businessRole_ = "invitee";
    rtcRole_.clear();
    remoteRoomKey_ = remoteRoomKey.trimmed();
    remoteUserId_ = remoteUserId.trimmed();
    waitingJoinParams_ = false;
    hasJoinParams_ = false;
    joinParams_ = {};
    signalState_ = incomingInviteState();
}

void LinkMicRoomState::handleIncomingAccept(const QString& sessionId,
                                            const QString& requestId,
                                            const QString& remoteRoomKey,
                                            const QString& remoteUserId) {
    if (!sessionId.trimmed().isEmpty()) {
        activeSessionId_ = sessionId.trimmed();
    }
    if (!requestId.trimmed().isEmpty()) {
        activeRequestId_ = requestId.trimmed();
    }
    if (!remoteRoomKey.trimmed().isEmpty()) {
        remoteRoomKey_ = remoteRoomKey.trimmed();
    }
    if (!remoteUserId.trimmed().isEmpty()) {
        remoteUserId_ = remoteUserId.trimmed();
    }

    businessRole_ = "inviter";
    waitingJoinParams_ = true;
    hasJoinParams_ = false;
    joinParams_ = {};
    lastError_.clear();
    signalState_ = waitingJoinParamsState();
}

void LinkMicRoomState::handleJoinParams(const QString& sessionId,
                                        const QString& requestId,
                                        const QString& remoteRoomKey,
                                        const QString& remoteUserId,
                                        const RtcJoinParams& joinParams) {
    if (!sessionId.trimmed().isEmpty()) {
        activeSessionId_ = sessionId.trimmed();
    }
    if (!requestId.trimmed().isEmpty()) {
        activeRequestId_ = requestId.trimmed();
    }
    if (!remoteRoomKey.trimmed().isEmpty()) {
        remoteRoomKey_ = remoteRoomKey.trimmed();
    }
    if (!remoteUserId.trimmed().isEmpty()) {
        remoteUserId_ = remoteUserId.trimmed();
    }

    joinParams_ = joinParams;
    hasJoinParams_ = joinParams.isValid();
    waitingJoinParams_ = false;
    if (!joinParams.businessRole.trimmed().isEmpty()) {
        businessRole_ = joinParams.businessRole.trimmed();
    }
    if (!joinParams.rtcRole.trimmed().isEmpty()) {
        rtcRole_ = joinParams.rtcRole.trimmed();
    }
    lastError_.clear();
    signalState_ = rtcJoiningState();
}

void LinkMicRoomState::handleRtcConnected() {
    waitingJoinParams_ = false;
    lastError_.clear();
    signalState_ = rtcConnectedState();
}

void LinkMicRoomState::handleRejectOrCancel(const QString& sessionId,
                                            const QString& requestId,
                                            const QString& remoteRoomKey,
                                            const QString& remoteUserId) {
    if (!sessionId.trimmed().isEmpty()) {
        activeSessionId_ = sessionId.trimmed();
    }
    if (!requestId.trimmed().isEmpty()) {
        activeRequestId_ = requestId.trimmed();
    }
    if (!remoteRoomKey.trimmed().isEmpty()) {
        remoteRoomKey_ = remoteRoomKey.trimmed();
    }
    if (!remoteUserId.trimmed().isEmpty()) {
        remoteUserId_ = remoteUserId.trimmed();
    }

    signalState_ = idleState();
    lastError_.clear();
    clearRequestState();
}

void LinkMicRoomState::handleHangupOrKick(const QString& sessionId,
                                          const QString& requestId,
                                          const QString& remoteRoomKey,
                                          const QString& remoteUserId) {
    if (!sessionId.trimmed().isEmpty()) {
        activeSessionId_ = sessionId.trimmed();
    }
    if (!requestId.trimmed().isEmpty()) {
        activeRequestId_ = requestId.trimmed();
    }
    if (!remoteRoomKey.trimmed().isEmpty()) {
        remoteRoomKey_ = remoteRoomKey.trimmed();
    }
    if (!remoteUserId.trimmed().isEmpty()) {
        remoteUserId_ = remoteUserId.trimmed();
    }

    signalState_ = idleState();
    lastError_.clear();
    clearRequestState();
}

bool LinkMicRoomState::isRegistered() const {
    return registered_;
}

bool LinkMicRoomState::isWaitingJoinParams() const {
    return waitingJoinParams_;
}

bool LinkMicRoomState::hasJoinParams() const {
    return hasJoinParams_;
}

bool LinkMicRoomState::hasActiveRequest() const {
    return !activeSessionId_.isEmpty() || !activeRequestId_.isEmpty();
}

bool LinkMicRoomState::matchesSession(const QString& sessionId, const QString& requestId) const {
    if (activeSessionId_.isEmpty() && activeRequestId_.isEmpty()) {
        return true;
    }
    if (!activeSessionId_.isEmpty()) {
        return !sessionId.trimmed().isEmpty() && activeSessionId_ == sessionId.trimmed();
    }
    return !requestId.trimmed().isEmpty() && activeRequestId_ == requestId.trimmed();
}

bool LinkMicRoomState::canInvite(const QString& localRoomKey,
                                 const QString& localUserId,
                                 const QString& targetRoomKey,
                                 const QString& targetUserId) const {
    return canStartNewRequest() &&
        !localRoomKey.trimmed().isEmpty() &&
        !localUserId.trimmed().isEmpty() &&
        !targetRoomKey.trimmed().isEmpty() &&
        !targetUserId.trimmed().isEmpty() &&
        !(localRoomKey.trimmed() == targetRoomKey.trimmed() && localUserId.trimmed() == targetUserId.trimmed());
}

bool LinkMicRoomState::canAccept() const {
    return !activeSessionId_.isEmpty() && signalState_ == incomingInviteState();
}

bool LinkMicRoomState::canReject() const {
    return canAccept();
}

bool LinkMicRoomState::canTerminate() const {
    if (activeSessionId_.isEmpty()) {
        return false;
    }

    return signalState_ == waitingResponseState() ||
        signalState_ == waitingJoinParamsState() ||
        signalState_ == rtcJoiningState() ||
        signalState_ == rtcConnectedState() ||
        signalState_ == endingState();
}

QString LinkMicRoomState::signalState() const {
    return signalState_;
}

QString LinkMicRoomState::activeSessionId() const {
    return activeSessionId_;
}

QString LinkMicRoomState::activeRequestId() const {
    return activeRequestId_;
}

QString LinkMicRoomState::businessRole() const {
    return businessRole_;
}

QString LinkMicRoomState::rtcRole() const {
    return rtcRole_;
}

QString LinkMicRoomState::remoteRoomKey() const {
    return remoteRoomKey_;
}

QString LinkMicRoomState::remoteUserId() const {
    return remoteUserId_;
}

QString LinkMicRoomState::lastError() const {
    return lastError_;
}

SignalingConnectionState LinkMicRoomState::connectionState() const {
    return connectionState_;
}

const RtcJoinParams& LinkMicRoomState::joinParams() const {
    return joinParams_;
}

QString LinkMicRoomState::idleState() {
    return "idle";
}

QString LinkMicRoomState::waitingResponseState() {
    return "invite-sent";
}

QString LinkMicRoomState::incomingInviteState() {
    return "invite-received";
}

QString LinkMicRoomState::waitingJoinParamsState() {
    return "waiting-join-params";
}

QString LinkMicRoomState::rtcJoiningState() {
    return "rtc-joining";
}

QString LinkMicRoomState::rtcConnectedState() {
    return "rtc-connected";
}

QString LinkMicRoomState::endingState() {
    return "ending";
}

bool LinkMicRoomState::canStartNewRequest() const {
    return registered_ &&
        connectionState_ == SignalingConnectionState::connected &&
        activeSessionId_.isEmpty() &&
        signalState_ == idleState();
}

QString LinkMicRoomState::nextRequestId(const QString& localRoomKey,
                                        const QString& localUserId,
                                        const QString& targetRoomKey,
                                        const QString& targetUserId) {
    ++requestSequence_;
    return QString("cr-req-%1-%2-to-%3-%4-%5-%6")
        .arg(localRoomKey.trimmed())
        .arg(localUserId.trimmed())
        .arg(targetRoomKey.trimmed())
        .arg(targetUserId.trimmed())
        .arg(QDateTime::currentMSecsSinceEpoch())
        .arg(requestSequence_);
}

QString LinkMicRoomState::nextSessionId(const QString& localRoomKey,
                                        const QString& localUserId,
                                        const QString& targetRoomKey,
                                        const QString& targetUserId) {
    ++requestSequence_;
    return QString("cr-session-%1-%2-to-%3-%4-%5-%6")
        .arg(localRoomKey.trimmed())
        .arg(localUserId.trimmed())
        .arg(targetRoomKey.trimmed())
        .arg(targetUserId.trimmed())
        .arg(QDateTime::currentMSecsSinceEpoch())
        .arg(requestSequence_);
}

void LinkMicRoomState::clearRequestState() {
    activeSessionId_.clear();
    activeRequestId_.clear();
    businessRole_.clear();
    rtcRole_.clear();
    remoteRoomKey_.clear();
    remoteUserId_.clear();
    waitingJoinParams_ = false;
    hasJoinParams_ = false;
    joinParams_ = {};
}

}  // namespace live_studio::linkmic
