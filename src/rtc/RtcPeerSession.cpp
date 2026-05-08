#include "rtc/RtcPeerSession.hpp"

#include <rtc/rtc.hpp>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <utility>

namespace live_studio::rtc {
namespace {

QString toQString(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<int>(value.size()));
}

std::string toStdString(const QString& value) {
    const QByteArray utf8 = value.toUtf8();
    return std::string(utf8.constData(), static_cast<std::size_t>(utf8.size()));
}

std::string buildIceServerUrl(const QJsonObject& object) {
    QString url = object.value("url").toString().trimmed();
    if (url.isEmpty()) {
        url = object.value("urls").toString().trimmed();
    }
    if (url.isEmpty() && object.contains("urls") && object.value("urls").isArray()) {
        const QJsonArray urls = object.value("urls").toArray();
        if (!urls.isEmpty()) {
            url = urls.first().toString().trimmed();
        }
    }
    if (url.isEmpty()) {
        return {};
    }

    const QString username = object.value("username").toString().trimmed();
    const QString credential = object.value("credential").toString().trimmed();
    if (username.isEmpty() || credential.isEmpty() || url.contains('@')) {
        return toStdString(url);
    }

    const int schemeSeparator = url.indexOf("://");
    if (schemeSeparator < 0) {
        return toStdString(url);
    }

    const QString decorated = url.left(schemeSeparator + 3) + username + ":" + credential + "@" +
        url.mid(schemeSeparator + 3);
    return toStdString(decorated);
}

QJsonObject buildPingPayload(const RtcSessionConfig& config) {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    return QJsonObject{
        {"type", "ping"},
        {"sessionId", config.sessionId},
        {"fromUserId", config.selfUserId},
        {"toUserId", config.peerUserId},
        {"sentAtMs", nowMs}
    };
}

QJsonObject buildPongPayload(const RtcSessionConfig& config) {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    return QJsonObject{
        {"type", "pong"},
        {"sessionId", config.sessionId},
        {"fromUserId", config.selfUserId},
        {"toUserId", config.peerUserId},
        {"sentAtMs", nowMs}
    };
}

}  // namespace

RtcPeerSession::RtcPeerSession() = default;

RtcPeerSession::~RtcPeerSession() {
    stop();
}

void RtcPeerSession::setCallbacks(Callbacks callbacks) {
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks_ = std::move(callbacks);
}

bool RtcPeerSession::start(const RtcSessionConfig& config) {
    if (!config.isValid()) {
        return false;
    }

    stop();

    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    snapshot_ = {};
    snapshot_.sessionId = config.sessionId;
    snapshot_.selfUserId = config.selfUserId;
    snapshot_.peerUserId = config.peerUserId;
    snapshot_.rtcRole = config.rtcRole;
    snapshot_.transportState = RtcTransportState::starting;
    started_ = true;

    createPeerConnectionLocked();
    maybeCreateOutboundDataChannelLocked();
    return true;
}

void RtcPeerSession::stop(const QString& reason) {
    std::shared_ptr<::rtc::DataChannel> dataChannel;
    std::shared_ptr<::rtc::PeerConnection> peerConnection;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dataChannel = std::exchange(dataChannel_, nullptr);
        peerConnection = std::exchange(peerConnection_, nullptr);
        if (!started_ && snapshot_.transportState == RtcTransportState::idle) {
            return;
        }
        started_ = false;
        snapshot_.transportState = RtcTransportState::closed;
        snapshot_.dataChannelState = RtcDataChannelState::closed;
        if (!reason.trimmed().isEmpty()) {
            snapshot_.lastError = reason.trimmed();
        }
    }

    if (dataChannel) {
        try {
            dataChannel->close();
        } catch (...) {
        }
    }
    if (peerConnection) {
        try {
            peerConnection->close();
        } catch (...) {
        }
    }
}

bool RtcPeerSession::applyRemoteDescription(const QString& sdp, const QString& descriptionType) {
    std::shared_ptr<::rtc::PeerConnection> peerConnection;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || !peerConnection_) {
            return false;
        }
        peerConnection = peerConnection_;
        snapshot_.transportState = RtcTransportState::connecting;
    }

    try {
        peerConnection->setRemoteDescription(
            ::rtc::Description(toStdString(sdp), toStdString(descriptionType)));
        return true;
    } catch (const std::exception& exception) {
        updateSnapshot([&](RtcPeerSessionSnapshot& snapshot) {
            snapshot.transportState = RtcTransportState::failed;
            snapshot.lastError = toQString(exception.what());
        });
        return false;
    }
}

bool RtcPeerSession::addRemoteCandidate(const QString& candidate, const QString& mid) {
    std::shared_ptr<::rtc::PeerConnection> peerConnection;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || !peerConnection_) {
            return false;
        }
        peerConnection = peerConnection_;
    }

    try {
        peerConnection->addRemoteCandidate(::rtc::Candidate(toStdString(candidate), toStdString(mid)));
        return true;
    } catch (const std::exception& exception) {
        updateSnapshot([&](RtcPeerSessionSnapshot& snapshot) {
            snapshot.transportState = RtcTransportState::failed;
            snapshot.lastError = toQString(exception.what());
        });
        return false;
    }
}

RtcPeerSessionSnapshot RtcPeerSession::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void RtcPeerSession::updateSnapshot(const std::function<void(RtcPeerSessionSnapshot&)>& updater) {
    std::lock_guard<std::mutex> lock(mutex_);
    updater(snapshot_);
}

void RtcPeerSession::createPeerConnectionLocked() {
    ::rtc::Configuration configuration;
    for (const QJsonValue& value : config_.iceServers) {
        if (value.isString()) {
            const QString url = value.toString().trimmed();
            if (!url.isEmpty()) {
                configuration.iceServers.emplace_back(toStdString(url));
            }
            continue;
        }
        if (!value.isObject()) {
            continue;
        }
        const std::string iceServerUrl = buildIceServerUrl(value.toObject());
        if (!iceServerUrl.empty()) {
            configuration.iceServers.emplace_back(iceServerUrl);
        }
    }

    peerConnection_ = std::make_shared<::rtc::PeerConnection>(configuration);
    attachPeerConnectionHandlersLocked();
    snapshot_.transportState = RtcTransportState::connecting;
}

void RtcPeerSession::attachPeerConnectionHandlersLocked() {
    auto peerConnection = peerConnection_;
    peerConnection->onLocalDescription([this](::rtc::Description description) {
        Callbacks callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callbacks = callbacks_;
        }
        if (callbacks.onLocalDescription) {
            callbacks.onLocalDescription(toQString(std::string(description)),
                                         toQString(description.typeString()));
        }
    });

    peerConnection->onLocalCandidate([this](::rtc::Candidate candidate) {
        Callbacks callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callbacks = callbacks_;
        }
        if (callbacks.onLocalIceCandidate) {
            callbacks.onLocalIceCandidate(toQString(std::string(candidate)),
                                          toQString(candidate.mid()));
        }
    });

    peerConnection->onStateChange([this](::rtc::PeerConnection::State state) {
        bool notifyConnected = false;
        Callbacks callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callbacks = callbacks_;
            switch (state) {
            case ::rtc::PeerConnection::State::New:
                snapshot_.transportState = RtcTransportState::starting;
                break;
            case ::rtc::PeerConnection::State::Connecting:
                snapshot_.transportState = RtcTransportState::connecting;
                break;
            case ::rtc::PeerConnection::State::Connected:
                snapshot_.transportState = RtcTransportState::connected;
                notifyConnected = true;
                break;
            case ::rtc::PeerConnection::State::Disconnected:
                snapshot_.transportState = RtcTransportState::connecting;
                break;
            case ::rtc::PeerConnection::State::Failed:
                snapshot_.transportState = RtcTransportState::failed;
                snapshot_.lastError = "PeerConnection entered failed state";
                break;
            case ::rtc::PeerConnection::State::Closed:
                snapshot_.transportState = RtcTransportState::closed;
                break;
            }
        }

        if (notifyConnected && callbacks.onPeerConnectionConnected) {
            callbacks.onPeerConnectionConnected();
        }
    });

    peerConnection->onDataChannel([this](std::shared_ptr<::rtc::DataChannel> channel) {
        std::lock_guard<std::mutex> lock(mutex_);
        dataChannel_ = channel;
        attachDataChannelHandlersLocked(channel);
    });
}

void RtcPeerSession::attachDataChannelHandlersLocked(const std::shared_ptr<::rtc::DataChannel>& channel) {
    snapshot_.dataChannelState = RtcDataChannelState::opening;

    channel->onOpen([this, channel]() {
        updateSnapshot([&](RtcPeerSessionSnapshot& snapshot) {
            snapshot.dataChannelState = RtcDataChannelState::open;
            snapshot.pingSentCount += 1;
        });
        channel->send(QJsonDocument(buildPingPayload(config_)).toJson(QJsonDocument::Compact).toStdString());
    });

    channel->onClosed([this]() {
        updateSnapshot([&](RtcPeerSessionSnapshot& snapshot) {
            snapshot.dataChannelState = RtcDataChannelState::closed;
        });
    });

    channel->onMessage([this, channel](::rtc::message_variant data) {
        if (!std::holds_alternative<std::string>(data)) {
            return;
        }
        handleDataChannelStringMessage(toQString(std::get<std::string>(data)), channel);
    });
}

void RtcPeerSession::maybeCreateOutboundDataChannelLocked() {
    if (config_.rtcRole.compare("offerer", Qt::CaseInsensitive) != 0 || !peerConnection_) {
        return;
    }

    dataChannel_ = peerConnection_->createDataChannel("cross-room-linkmic");
    attachDataChannelHandlersLocked(dataChannel_);
}

void RtcPeerSession::handleDataChannelStringMessage(const QString& payload,
                                                    const std::shared_ptr<::rtc::DataChannel>& channel) {
    updateSnapshot([&](RtcPeerSessionSnapshot& snapshot) {
        snapshot.lastDataChannelMessage = payload;
    });

    const QJsonDocument document = QJsonDocument::fromJson(payload.toUtf8());
    if (!document.isObject()) {
        return;
    }

    const QJsonObject object = document.object();
    const QString type = object.value("type").toString().trimmed();
    if (type == "ping") {
        updateSnapshot([&](RtcPeerSessionSnapshot& snapshot) {
            snapshot.pingReceivedCount += 1;
            snapshot.pongSentCount += 1;
        });
        channel->send(QJsonDocument(buildPongPayload(config_)).toJson(QJsonDocument::Compact).toStdString());
        return;
    }

    if (type == "pong") {
        updateSnapshot([&](RtcPeerSessionSnapshot& snapshot) {
            snapshot.pongReceivedCount += 1;
        });
    }
}

}  // namespace live_studio::rtc
