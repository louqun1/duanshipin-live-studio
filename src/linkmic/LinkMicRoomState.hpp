#pragma once

#include <cstdint>

#include "linkmic/LinkMicTypes.hpp"

namespace live_studio::linkmic {

class LinkMicRoomState {
public:
    LinkMicRoomState();

    void reset();
    void markRegistered();
    void markDisconnected();
    void setConnectionState(SignalingConnectionState state);
    void failTransport(const QString& error);
    void failSession(const QString& error);
    void recoverFromSignalError(const QString& error);

    [[nodiscard]] LinkMicRequestIds beginInvite(const QString& localRoomKey,
                                                const QString& localUserId,
                                                const QString& targetRoomKey,
                                                const QString& targetUserId);
    void handleOutgoingAccept();
    void handleOutgoingReject();
    void handleOutgoingTerminate();

    void handleIncomingInvite(const QString& sessionId,
                              const QString& requestId,
                              const QString& remoteRoomKey,
                              const QString& remoteUserId);
    void handleIncomingAccept(const QString& sessionId,
                              const QString& requestId,
                              const QString& remoteRoomKey,
                              const QString& remoteUserId);
    void handleJoinParams(const QString& sessionId,
                          const QString& requestId,
                          const QString& remoteRoomKey,
                          const QString& remoteUserId,
                          const RtcJoinParams& joinParams);
    void handleRtcConnected();
    void handleRejectOrCancel(const QString& sessionId,
                              const QString& requestId,
                              const QString& remoteRoomKey,
                              const QString& remoteUserId);
    void handleHangupOrKick(const QString& sessionId,
                            const QString& requestId,
                            const QString& remoteRoomKey,
                            const QString& remoteUserId);

    [[nodiscard]] bool isRegistered() const;
    [[nodiscard]] bool isWaitingJoinParams() const;
    [[nodiscard]] bool hasJoinParams() const;
    [[nodiscard]] bool hasActiveRequest() const;
    [[nodiscard]] bool matchesSession(const QString& sessionId, const QString& requestId) const;
    [[nodiscard]] bool canInvite(const QString& localRoomKey,
                                 const QString& localUserId,
                                 const QString& targetRoomKey,
                                 const QString& targetUserId) const;
    [[nodiscard]] bool canAccept() const;
    [[nodiscard]] bool canReject() const;
    [[nodiscard]] bool canTerminate() const;

    [[nodiscard]] QString signalState() const;
    [[nodiscard]] QString activeSessionId() const;
    [[nodiscard]] QString activeRequestId() const;
    [[nodiscard]] QString businessRole() const;
    [[nodiscard]] QString rtcRole() const;
    [[nodiscard]] QString remoteRoomKey() const;
    [[nodiscard]] QString remoteUserId() const;
    [[nodiscard]] QString lastError() const;
    [[nodiscard]] SignalingConnectionState connectionState() const;
    [[nodiscard]] const RtcJoinParams& joinParams() const;

    [[nodiscard]] static QString idleState();
    [[nodiscard]] static QString waitingResponseState();
    [[nodiscard]] static QString incomingInviteState();
    [[nodiscard]] static QString waitingJoinParamsState();
    [[nodiscard]] static QString rtcJoiningState();
    [[nodiscard]] static QString rtcConnectedState();
    [[nodiscard]] static QString endingState();

private:
    [[nodiscard]] bool canStartNewRequest() const;
    [[nodiscard]] QString nextRequestId(const QString& localRoomKey,
                                        const QString& localUserId,
                                        const QString& targetRoomKey,
                                        const QString& targetUserId);
    [[nodiscard]] QString nextSessionId(const QString& localRoomKey,
                                        const QString& localUserId,
                                        const QString& targetRoomKey,
                                        const QString& targetUserId);
    void clearRequestState();

    QString signalState_;
    QString activeSessionId_;
    QString activeRequestId_;
    QString businessRole_;
    QString rtcRole_;
    QString remoteRoomKey_;
    QString remoteUserId_;
    QString lastError_;
    SignalingConnectionState connectionState_ = SignalingConnectionState::disconnected;
    bool registered_ = false;
    bool waitingJoinParams_ = false;
    bool hasJoinParams_ = false;
    RtcJoinParams joinParams_;
    std::uint64_t requestSequence_ = 0;
};

}  // namespace live_studio::linkmic
