#include "rtc/RtcSignalingBridge.hpp"

#include <QDateTime>
#include <QJsonObject>

namespace live_studio::rtc {
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

QJsonObject extractPayload(const QJsonObject& message) {
    if (message.contains("payload") && message.value("payload").isObject()) {
        return message.value("payload").toObject();
    }
    return {};
}

}  // namespace

void RtcSignalingBridge::setCallbacks(Callbacks callbacks) {
    callbacks_ = std::move(callbacks);
}

bool RtcSignalingBridge::start(const RtcSessionConfig& config, SendEnvelope sender) {
    if (!config.isValid() || !sender) {
        return false;
    }

    config_ = config;
    sendEnvelope_ = std::move(sender);
    active_ = true;
    return true;
}

void RtcSignalingBridge::stop() {
    active_ = false;
    sendEnvelope_ = {};
    config_ = {};
}

bool RtcSignalingBridge::isActive() const {
    return active_;
}

const RtcSessionConfig& RtcSignalingBridge::config() const {
    return config_;
}

QString RtcSignalingBridge::sessionId() const {
    return config_.sessionId;
}

bool RtcSignalingBridge::handleIncomingMessage(const QJsonObject& message) {
    if (!active_ || !messageTargetsSession(message)) {
        return false;
    }

    const QString type = message.value("type").toString().trimmed();
    const QJsonObject payload = extractPayload(message);

    if (type == "rtc.offer" || type == "rtc.answer") {
        if (callbacks_.onRemoteDescription) {
            callbacks_.onRemoteDescription(firstString(payload, {"sdp"}),
                                           firstString(payload, {"descriptionType", "description_type"}));
        }
        return true;
    }

    if (type == "rtc.ice-candidate") {
        if (callbacks_.onRemoteIceCandidate) {
            callbacks_.onRemoteIceCandidate(firstString(payload, {"candidate"}),
                                            firstString(payload, {"mid"}));
        }
        return true;
    }

    if (type == "signal.error" && callbacks_.onWarning) {
        callbacks_.onWarning(firstString(message, {"message"}));
        return true;
    }

    return false;
}

bool RtcSignalingBridge::sendLocalDescription(const QString& sdp, const QString& descriptionType) {
    if (!active_ || !sendEnvelope_) {
        return false;
    }

    const QString loweredType = descriptionType.trimmed().toLower();
    const QString messageType = loweredType == "offer" ? "rtc.offer" : "rtc.answer";
    return sendEnvelope_(buildEnvelope(messageType,
                                       {
                                           {"sdp", sdp},
                                           {"descriptionType", loweredType},
                                           {"description_type", loweredType}
                                       }));
}

bool RtcSignalingBridge::sendLocalIceCandidate(const QString& candidate, const QString& mid) {
    if (!active_ || !sendEnvelope_) {
        return false;
    }

    return sendEnvelope_(buildEnvelope("rtc.ice-candidate",
                                       {
                                           {"candidate", candidate},
                                           {"mid", mid}
                                       }));
}

bool RtcSignalingBridge::sendConnected() {
    if (!active_ || !sendEnvelope_) {
        return false;
    }

    return sendEnvelope_(buildEnvelope("linkmic.connected",
                                       {
                                           {"state", "connected"},
                                           {"rtcRole", config_.rtcRole},
                                           {"rtc_role", config_.rtcRole},
                                           {"source", "RtcPeerSession"}
                                       }));
}

bool RtcSignalingBridge::messageTargetsSession(const QJsonObject& message) const {
    if (!active_) {
        return false;
    }

    const QString sessionId = firstString(message, {"sessionId", "session_id"});
    if (sessionId != config_.sessionId) {
        return false;
    }

    const QString toRoomKey = firstString(message, {"toRoomKey", "to_room_key"});
    if (!toRoomKey.isEmpty() && toRoomKey != config_.selfRoomKey) {
        return false;
    }

    const QString toUserId = firstString(message, {"toUserId", "to_user_id"});
    if (!toUserId.isEmpty() && toUserId != config_.selfUserId) {
        return false;
    }

    const QString fromRoomKey = firstString(message, {"fromRoomKey", "from_room_key"});
    if (!fromRoomKey.isEmpty() && fromRoomKey != config_.peerRoomKey) {
        return false;
    }

    const QString fromUserId = firstString(message, {"fromUserId", "from_user_id"});
    if (!fromUserId.isEmpty() && fromUserId != config_.peerUserId) {
        return false;
    }

    return true;
}

QJsonObject RtcSignalingBridge::buildEnvelope(const QString& type, const QJsonObject& payload) const {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    return QJsonObject{
        {"type", type},
        {"sessionId", config_.sessionId},
        {"session_id", config_.sessionId},
        {"fromRoomKey", config_.selfRoomKey},
        {"from_room_key", config_.selfRoomKey},
        {"toRoomKey", config_.peerRoomKey},
        {"to_room_key", config_.peerRoomKey},
        {"fromUserId", config_.selfUserId},
        {"from_user_id", config_.selfUserId},
        {"toUserId", config_.peerUserId},
        {"to_user_id", config_.peerUserId},
        {"requestId", config_.requestId},
        {"request_id", config_.requestId},
        {"payload", payload},
        {"tsMs", nowMs},
        {"ts_ms", nowMs}
    };
}

}  // namespace live_studio::rtc
