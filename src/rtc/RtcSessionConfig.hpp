#pragma once

#include <QJsonArray>
#include <QString>
#include <QUrl>

#include "common/MediaTypes.hpp"

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
    media::LiveDeviceCaptureConfig localCapture;
    int localVideoBitrate = 1500000;
    int localAudioBitrate = 64000;
    bool enableVideoSend = true;
    bool enableAudioSend = true;
    bool preferExternalCapture = false;
    bool enableDebugAudioDump = false;
    bool enableDebugVideoDump = false;
    QString debugDumpDirectory = "logs/rtc-media";

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
