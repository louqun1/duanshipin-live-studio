#pragma once

#include <QString>

namespace live_studio::rtc {

enum class RtcTransportState {
    idle,
    starting,
    connecting,
    connected,
    failed,
    closed,
};

enum class RtcDataChannelState {
    idle,
    opening,
    open,
    closed,
};

struct RtcPeerSessionSnapshot {
    QString sessionId;
    QString selfUserId;
    QString peerUserId;
    QString rtcRole;
    RtcTransportState transportState = RtcTransportState::idle;
    RtcDataChannelState dataChannelState = RtcDataChannelState::idle;
    int pingSentCount = 0;
    int pingReceivedCount = 0;
    int pongSentCount = 0;
    int pongReceivedCount = 0;
    QString lastDataChannelMessage;
    QString lastError;
};

[[nodiscard]] inline QString rtcTransportStateText(RtcTransportState state) {
    switch (state) {
    case RtcTransportState::idle:
        return "idle";
    case RtcTransportState::starting:
        return "starting";
    case RtcTransportState::connecting:
        return "connecting";
    case RtcTransportState::connected:
        return "connected";
    case RtcTransportState::failed:
        return "failed";
    case RtcTransportState::closed:
        return "closed";
    }
    return "idle";
}

[[nodiscard]] inline QString rtcDataChannelStateText(RtcDataChannelState state) {
    switch (state) {
    case RtcDataChannelState::idle:
        return "idle";
    case RtcDataChannelState::opening:
        return "opening";
    case RtcDataChannelState::open:
        return "open";
    case RtcDataChannelState::closed:
        return "closed";
    }
    return "idle";
}

}  // namespace live_studio::rtc
