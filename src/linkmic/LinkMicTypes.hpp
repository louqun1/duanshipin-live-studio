#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace live_studio::linkmic {

enum class SignalingConnectionState {
    disconnected,
    connecting,
    connected,
    failed,
};

struct LinkMicSessionConfig {
    QUrl signalingUrl;
    QString roomKey;
    QString userId;
    QString displayName;
    int requestTimeoutMs = 10000;

    [[nodiscard]] bool isValid() const {
        return signalingUrl.isValid() &&
            !roomKey.trimmed().isEmpty() &&
            !userId.trimmed().isEmpty() &&
            !displayName.trimmed().isEmpty();
    }
};

struct CrossRoomInviteTarget {
    QString roomKey;
    QString userId;

    [[nodiscard]] bool isValid() const {
        return !roomKey.trimmed().isEmpty() && !userId.trimmed().isEmpty();
    }
};

struct LinkMicRequestIds {
    QString sessionId;
    QString requestId;

    [[nodiscard]] bool isValid() const {
        return !sessionId.trimmed().isEmpty() && !requestId.trimmed().isEmpty();
    }
};

struct RoomMember {
    QString userId;
    QString displayName;
    QString avatarUrl;
    QString role;
    bool online = true;
    bool inLinkMic = false;
    QJsonObject raw;
};

struct RtcJoinParams {
    QString sessionId;
    QString requestId;
    QString roomAKey;
    QString roomBKey;
    QString selfRoomKey;
    QString peerRoomKey;
    QString selfUserId;
    QString peerUserId;
    QString businessRole;
    QString rtcRole;
    QString signalingUrl;
    QJsonArray iceServers;
    QString rtcBackend;
    QString mediaMode;
    QJsonObject raw;

    [[nodiscard]] bool isValid() const {
        return !sessionId.trimmed().isEmpty() &&
            !selfRoomKey.trimmed().isEmpty() &&
            !peerRoomKey.trimmed().isEmpty() &&
            !selfUserId.trimmed().isEmpty() &&
            !peerUserId.trimmed().isEmpty() &&
            !rtcRole.trimmed().isEmpty();
    }
};

struct LinkMicEventLogEntry {
    QDateTime timestampUtc;
    QString direction;
    QString type;
    QString detail;
    QString state;
    QString sessionId;
    QString requestId;
    QString remoteRoomKey;
    QString remoteUserId;
};

[[nodiscard]] inline QString connectionStateText(SignalingConnectionState state) {
    switch (state) {
    case SignalingConnectionState::disconnected:
        return "disconnected";
    case SignalingConnectionState::connecting:
        return "connecting";
    case SignalingConnectionState::connected:
        return "connected";
    case SignalingConnectionState::failed:
        return "failed";
    }
    return "disconnected";
}

[[nodiscard]] inline QString displayNameForMember(const RoomMember& member) {
    return member.displayName.trimmed().isEmpty() ? member.userId : member.displayName;
}

[[nodiscard]] inline QString joinParamsSummary(const RtcJoinParams& params) {
    if (!params.isValid()) {
        return "pending";
    }

    QStringList parts;
    if (!params.sessionId.trimmed().isEmpty()) {
        parts.push_back("session=" + params.sessionId);
    }
    if (!params.selfRoomKey.trimmed().isEmpty() && !params.peerRoomKey.trimmed().isEmpty()) {
        parts.push_back("rooms=" + params.selfRoomKey + " <-> " + params.peerRoomKey);
    }
    if (!params.businessRole.trimmed().isEmpty()) {
        parts.push_back("business=" + params.businessRole);
    }
    if (!params.rtcRole.trimmed().isEmpty()) {
        parts.push_back("rtc=" + params.rtcRole);
    }
    if (!params.rtcBackend.trimmed().isEmpty()) {
        parts.push_back("backend=" + params.rtcBackend);
    }
    if (!params.mediaMode.trimmed().isEmpty()) {
        parts.push_back("media=" + params.mediaMode);
    }
    return parts.join(" | ");
}

}  // namespace live_studio::linkmic
