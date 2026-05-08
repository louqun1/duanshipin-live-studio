#pragma once

#include <QJsonArray>
#include <QString>
#include <QUrl>

namespace live_studio::rtc {

struct RtcSessionConfig {
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
    QUrl signalingUrl;
    QJsonArray iceServers;
    QString rtcBackend = "libdatachannel";
    QString mediaMode = "audio-first";

    [[nodiscard]] bool isValid() const {
        return !sessionId.trimmed().isEmpty() &&
            !selfRoomKey.trimmed().isEmpty() &&
            !peerRoomKey.trimmed().isEmpty() &&
            !selfUserId.trimmed().isEmpty() &&
            !peerUserId.trimmed().isEmpty() &&
            !rtcRole.trimmed().isEmpty() &&
            signalingUrl.isValid();
    }
};

}  // namespace live_studio::rtc
