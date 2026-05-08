#pragma once

#include <QtGlobal>

#include <QList>
#include <QString>

namespace live_studio::business {

enum class BusinessLiveState {
    idle,
    logging_in,
    logged_in,
    creating_room,
    room_created,
    ws_connecting,
    ws_connected,
    anchor_online,
    reconnecting,
    stopping,
    error,
};

struct BusinessStartConfig {
    QString apiBaseUrl;
    QString username;
    QString password;
    QString roomKey;
    QString title;

    [[nodiscard]] bool isValid() const {
        return !apiBaseUrl.trimmed().isEmpty() && !username.trimmed().isEmpty() && !password.isEmpty();
    }
};

struct BusinessRoomMember {
    qint64 userId = 0;
    QString username;
    QString role;
    bool online = false;
};

struct BusinessLiveSnapshot {
    BusinessLiveState state = BusinessLiveState::idle;
    QString apiBaseUrl;
    QString accessToken;
    qint64 userId = 0;
    QString username;
    QString roomKey;
    QString title;
    QString signalingUrl;
    bool businessWsConnected = false;
    bool anchorJoined = false;
    QString lastError;
    QString lastWarning;
    QList<BusinessRoomMember> members;
};

[[nodiscard]] inline QString businessStateText(BusinessLiveState state) {
    switch (state) {
    case BusinessLiveState::idle:
        return "Not logged in";
    case BusinessLiveState::logging_in:
        return "Logging in";
    case BusinessLiveState::logged_in:
        return "Logged in";
    case BusinessLiveState::creating_room:
        return "Creating room";
    case BusinessLiveState::room_created:
        return "Room created";
    case BusinessLiveState::ws_connecting:
        return "Connecting WebSocket";
    case BusinessLiveState::ws_connected:
        return "WebSocket connected";
    case BusinessLiveState::anchor_online:
        return "Anchor online";
    case BusinessLiveState::reconnecting:
        return "Business presence disconnected";
    case BusinessLiveState::stopping:
        return "Stopping";
    case BusinessLiveState::error:
        return "Error";
    }
    return "Not logged in";
}

[[nodiscard]] inline bool businessStateIsBusy(BusinessLiveState state) {
    switch (state) {
    case BusinessLiveState::logging_in:
    case BusinessLiveState::creating_room:
    case BusinessLiveState::ws_connecting:
    case BusinessLiveState::stopping:
        return true;
    case BusinessLiveState::idle:
    case BusinessLiveState::logged_in:
    case BusinessLiveState::room_created:
    case BusinessLiveState::ws_connected:
    case BusinessLiveState::anchor_online:
    case BusinessLiveState::reconnecting:
    case BusinessLiveState::error:
        return false;
    }
    return false;
}

}  // namespace live_studio::business
