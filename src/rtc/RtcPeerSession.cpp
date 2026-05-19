#include "rtc/RtcPeerSession.hpp"

#include <rtc/h264rtpdepacketizer.hpp>
#include <rtc/h264rtppacketizer.hpp>
#include <rtc/rtcpnackresponder.hpp>
#include <rtc/rtc.hpp>
#include <rtc/rtcpreceivingsession.hpp>
#include <rtc/rtcpsrreporter.hpp>
#include <rtc/rtpdepacketizer.hpp>
#include <rtc/rtppacketizationconfig.hpp>

#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

#include "capture/LiveDeviceCaptureSource.hpp"
#include "encode/OpusAudioEncoder.hpp"
#include "encode/VideoEncoder.hpp"
#include "rtc/RtcRemoteAudioSink.hpp"
#include "rtc/RtcRemoteVideoSink.hpp"

namespace live_studio::rtc {
namespace {

constexpr int kOpusPayloadType = 111;
constexpr int kH264PayloadType = 102;
constexpr int kDefaultOpusSampleRate = 48000;
constexpr int kDefaultOpusChannels = 2;
constexpr std::size_t kMaxQueuedLocalVideoFrames = 8;
constexpr std::size_t kMaxQueuedLocalAudioFrames = 128;

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

QString sanitizeFileComponent(QString value) {
    value = value.trimmed();
    if (value.isEmpty()) {
        return "rtc";
    }

    for (int index = 0; index < value.size(); ++index) {
        const QChar ch = value.at(index);
        if (!ch.isLetterOrNumber() && ch != '-' && ch != '_') {
            value[index] = '_';
        }
    }
    return value;
}

QString ensureDebugDumpPath(const RtcSessionConfig& config, const QString& kind, const QString& extension) {
    const QString baseDir = config.debugDumpDirectory.trimmed().isEmpty() ? "logs/rtc-media" : config.debugDumpDirectory;
    QDir directory(baseDir);
    directory.mkpath(".");
    return directory.filePath(QString("%1-%2-%3.%4")
                                  .arg(sanitizeFileComponent(config.sessionId),
                                       sanitizeFileComponent(config.selfUserId),
                                       kind,
                                       extension));
}

QString localMidPrefix(const RtcSessionConfig& config) {
    return config.rtcRole.compare("offerer", Qt::CaseInsensitive) == 0 ? "offerer" : "answerer";
}

QString remoteMidPrefix(const RtcSessionConfig& config) {
    return config.rtcRole.compare("offerer", Qt::CaseInsensitive) == 0 ? "answerer" : "offerer";
}

QString outboundAudioMid(const RtcSessionConfig& config) {
    return localMidPrefix(config) + "-audio";
}

QString outboundVideoMid(const RtcSessionConfig& config) {
    return localMidPrefix(config) + "-video";
}

QString inboundAudioMid(const RtcSessionConfig& config) {
    return remoteMidPrefix(config) + "-audio";
}

QString inboundVideoMid(const RtcSessionConfig& config) {
    return remoteMidPrefix(config) + "-video";
}

std::uint32_t stableSsrc(const RtcSessionConfig& config, const char* kind) {
    const std::string seed = toStdString(config.sessionId) + "|" + toStdString(config.selfUserId) + "|" + kind;
    const auto hashed = static_cast<std::uint32_t>(std::hash<std::string>{}(seed));
    return hashed == 0 ? 1u : hashed;
}

std::string buildOpusProfile(int channels, int bitrate) {
    const bool stereo = channels > 1;
    return "minptime=10;maxaveragebitrate=" + std::to_string(std::max(6000, bitrate)) +
        ";stereo=" + (stereo ? "1" : "0") +
        ";sprop-stereo=" + (stereo ? "1" : "0") +
        ";useinbandfec=1";
}

bool startsWithStartCode(const std::uint8_t* data, std::size_t size, std::size_t offset, std::size_t* codeSize) {
    if ((offset + 3) <= size && data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 1) {
        *codeSize = 3;
        return true;
    }
    if ((offset + 4) <= size && data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 0 &&
        data[offset + 3] == 1) {
        *codeSize = 4;
        return true;
    }
    return false;
}

bool payloadContainsNalType(const std::vector<std::uint8_t>& payload, std::uint8_t nalType) {
    std::size_t cursor = 0;
    while (cursor < payload.size()) {
        std::size_t startCodeSize = 0;
        if (!startsWithStartCode(payload.data(), payload.size(), cursor, &startCodeSize)) {
            ++cursor;
            continue;
        }

        const std::size_t naluStart = cursor + startCodeSize;
        if (naluStart < payload.size() && (payload[naluStart] & 0x1f) == nalType) {
            return true;
        }

        cursor = naluStart;
    }
    return false;
}

std::vector<std::uint8_t> prependParameterSetsIfNeeded(const media::H264ParameterSets& parameterSets,
                                                       const media::EncodedVideoAccessUnit& unit) {
    if (!unit.keyFrame || parameterSets.spsBytes.empty() || parameterSets.ppsBytes.empty()) {
        return unit.annexBPayload;
    }
    if (payloadContainsNalType(unit.annexBPayload, 7) && payloadContainsNalType(unit.annexBPayload, 8)) {
        return unit.annexBPayload;
    }

    std::vector<std::uint8_t> payload;
    payload.reserve(parameterSets.spsBytes.size() + parameterSets.ppsBytes.size() + unit.annexBPayload.size() + 12);
    payload.insert(payload.end(), {0, 0, 0, 1});
    payload.insert(payload.end(), parameterSets.spsBytes.begin(), parameterSets.spsBytes.end());
    payload.insert(payload.end(), {0, 0, 0, 1});
    payload.insert(payload.end(), parameterSets.ppsBytes.begin(), parameterSets.ppsBytes.end());
    payload.insert(payload.end(), unit.annexBPayload.begin(), unit.annexBPayload.end());
    return payload;
}

void pushBoundedVideoFrame(std::deque<media::VideoFrame>* queue, media::VideoFrame frame) {
    if (queue == nullptr) {
        return;
    }
    if (queue->size() >= kMaxQueuedLocalVideoFrames) {
        queue->pop_front();
    }
    queue->push_back(std::move(frame));
}

void pushBoundedAudioFrame(std::deque<media::AudioFrame>* queue, media::AudioFrame frame) {
    if (queue == nullptr) {
        return;
    }
    if (queue->size() >= kMaxQueuedLocalAudioFrames) {
        queue->pop_front();
    }
    queue->push_back(std::move(frame));
}

}  // namespace

struct RtcPeerSession::Impl {
    std::shared_ptr<::rtc::PeerConnection> peerConnection;
    std::shared_ptr<::rtc::DataChannel> dataChannel;
    std::shared_ptr<::rtc::Track> outboundAudioTrack;
    std::shared_ptr<::rtc::Track> outboundVideoTrack;
    std::shared_ptr<::rtc::Track> inboundAudioTrack;
    std::shared_ptr<::rtc::Track> inboundVideoTrack;
    std::shared_ptr<::rtc::RtpPacketizationConfig> outboundAudioRtpConfig;
    std::shared_ptr<::rtc::RtpPacketizationConfig> outboundVideoRtpConfig;

    capture::LiveDeviceCaptureSource captureSource;
    std::thread captureThread;
    std::thread audioThread;
    std::thread videoThread;

    std::mutex audioQueueMutex;
    std::condition_variable audioQueueCondition;
    std::deque<media::AudioFrame> audioQueue;

    std::mutex videoQueueMutex;
    std::condition_variable videoQueueCondition;
    std::deque<media::VideoFrame> videoQueue;

    std::atomic_bool stopRequested{false};
    std::atomic_bool outboundAudioTrackOpen{false};
    std::atomic_bool outboundVideoTrackOpen{false};

    encode::OpusAudioEncoder audioEncoder;
    encode::VideoEncoder videoEncoder;
    bool audioEncoderConfigured = false;
    bool videoEncoderConfigured = false;
    media::H264ParameterSets h264ParameterSets;

    RtcRemoteAudioSink remoteAudioSink;
    RtcRemoteVideoSink remoteVideoSink;
};

RtcPeerSession::Callbacks RtcPeerSession::currentCallbacks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return callbacks_;
}

RtcSessionConfig RtcPeerSession::currentConfig() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

void RtcPeerSession::emitLog(const QString& message, bool warning) const {
    const auto callbacks = currentCallbacks();
    if (callbacks.onLog) {
        callbacks.onLog(message, warning);
    }
}

void RtcPeerSession::signalMediaFailure(const QString& message) {
    updateSnapshot([&](RtcPeerSessionSnapshot& snapshot) {
        snapshot.transportState = RtcTransportState::failed;
        snapshot.lastError = message;
    });
    emitLog(message, true);
}

void RtcPeerSession::updateSnapshot(const std::function<void(RtcPeerSessionSnapshot&)>& updater) {
    std::lock_guard<std::mutex> lock(mutex_);
    updater(snapshot_);
}

namespace {

void videoSendThreadMain(RtcPeerSession* session, const std::shared_ptr<RtcPeerSession::Impl>& impl) {
    while (!impl->stopRequested.load()) {
        media::VideoFrame frame;
        {
            std::unique_lock<std::mutex> lock(impl->videoQueueMutex);
            impl->videoQueueCondition.wait(lock, [&]() {
                return impl->stopRequested.load() || !impl->videoQueue.empty();
            });
            if (impl->stopRequested.load() && impl->videoQueue.empty()) {
                return;
            }
            frame = std::move(impl->videoQueue.front());
            impl->videoQueue.pop_front();
        }

        if (!impl->outboundVideoTrackOpen.load() || impl->outboundVideoTrack == nullptr) {
            continue;
        }

        if (!impl->videoEncoderConfigured) {
            const auto config = session->currentConfig();
            const Status configureStatus = impl->videoEncoder.configureH264(
                frame.width, frame.height, std::max(frame.fps, 1), std::max(config.localVideoBitrate, 256000));
            if (!configureStatus.ok()) {
                session->signalMediaFailure(QString("RTC H.264 encoder configure failed: %1")
                                                .arg(QString::fromStdString(configureStatus.message)));
                continue;
            }
            Status headerStatus = impl->videoEncoder.buildSequenceHeader(&impl->h264ParameterSets);
            if (!headerStatus.ok()) {
                session->signalMediaFailure(QString("RTC H.264 sequence header failed: %1")
                                                .arg(QString::fromStdString(headerStatus.message)));
                continue;
            }
            impl->videoEncoderConfigured = true;
        }

        media::EncodedVideoAccessUnit accessUnit;
        Status encodeStatus = impl->videoEncoder.encodeFrame(frame, &accessUnit);
        if (!encodeStatus.ok()) {
            session->signalMediaFailure(
                QString("RTC H.264 encode failed: %1").arg(QString::fromStdString(encodeStatus.message)));
            continue;
        }

        const std::vector<std::uint8_t> payload = prependParameterSetsIfNeeded(impl->h264ParameterSets, accessUnit);
        try {
            impl->outboundVideoTrack->sendFrame(reinterpret_cast<const ::rtc::byte*>(payload.data()),
                                                payload.size(),
                                                ::rtc::FrameInfo(
                                                    std::chrono::duration<double>(
                                                        static_cast<double>(accessUnit.ptsMs) / 1000.0)));
        } catch (const std::exception& exception) {
            session->signalMediaFailure(QString("failed to send RTC H.264 frame: %1").arg(exception.what()));
        }
    }
}

void audioSendThreadMain(RtcPeerSession* session, const std::shared_ptr<RtcPeerSession::Impl>& impl) {
    while (!impl->stopRequested.load()) {
        media::AudioFrame frame;
        {
            std::unique_lock<std::mutex> lock(impl->audioQueueMutex);
            impl->audioQueueCondition.wait(lock, [&]() {
                return impl->stopRequested.load() || !impl->audioQueue.empty();
            });
            if (impl->stopRequested.load() && impl->audioQueue.empty()) {
                return;
            }
            frame = std::move(impl->audioQueue.front());
            impl->audioQueue.pop_front();
        }

        if (!impl->outboundAudioTrackOpen.load() || impl->outboundAudioTrack == nullptr) {
            continue;
        }

        if (!impl->audioEncoderConfigured) {
            const auto config = session->currentConfig();
            const Status configureStatus = impl->audioEncoder.configure(
                frame.sampleRate, frame.channels, std::max(config.localAudioBitrate, 12000));
            if (!configureStatus.ok()) {
                session->signalMediaFailure(QString("RTC Opus encoder configure failed: %1")
                                                .arg(QString::fromStdString(configureStatus.message)));
                continue;
            }
            impl->audioEncoderConfigured = true;
        }

        std::vector<encode::EncodedOpusFrame> encodedFrames;
        Status encodeStatus = impl->audioEncoder.encodeFrame(frame, &encodedFrames);
        if (!encodeStatus.ok()) {
            session->signalMediaFailure(
                QString("RTC Opus encode failed: %1").arg(QString::fromStdString(encodeStatus.message)));
            continue;
        }

        for (const auto& encodedFrame : encodedFrames) {
            try {
                impl->outboundAudioTrack->sendFrame(
                    reinterpret_cast<const ::rtc::byte*>(encodedFrame.payload.data()),
                    encodedFrame.payload.size(),
                    ::rtc::FrameInfo(
                        std::chrono::duration<double>(
                            static_cast<double>(encodedFrame.ptsSamples) /
                            static_cast<double>(std::max(encodedFrame.sampleRate, 1)))));
            } catch (const std::exception& exception) {
                session->signalMediaFailure(QString("failed to send RTC Opus frame: %1").arg(exception.what()));
                break;
            }
        }
    }
}

void captureThreadMain(RtcPeerSession* session, const std::shared_ptr<RtcPeerSession::Impl>& impl) {
    while (!impl->stopRequested.load()) {
        capture::CaptureEvent event;
        const Status readStatus = impl->captureSource.readNext(&event);
        if (!readStatus.ok()) {
            if (!impl->stopRequested.load()) {
                session->signalMediaFailure(QString("RTC fallback capture failed: %1")
                                                .arg(QString::fromStdString(readStatus.message)));
            }
            return;
        }

        if (event.kind == capture::CaptureEventKind::video) {
            session->pushLocalVideoFrame(event.videoFrame);
        } else {
            session->pushLocalAudioFrame(event.audioFrame);
        }
    }
}

void stopMediaRuntime(const std::shared_ptr<RtcPeerSession::Impl>& impl) {
    if (!impl) {
        return;
    }

    impl->stopRequested.store(true);
    {
        std::lock_guard<std::mutex> lock(impl->audioQueueMutex);
        impl->audioQueue.clear();
        impl->audioQueueCondition.notify_all();
    }
    {
        std::lock_guard<std::mutex> lock(impl->videoQueueMutex);
        impl->videoQueue.clear();
        impl->videoQueueCondition.notify_all();
    }

    impl->captureSource.stop();
    impl->remoteAudioSink.stop();
    impl->remoteVideoSink.stop();

    if (impl->captureThread.joinable()) {
        impl->captureThread.join();
    }
    if (impl->audioThread.joinable()) {
        impl->audioThread.join();
    }
    if (impl->videoThread.joinable()) {
        impl->videoThread.join();
    }

    impl->outboundAudioTrackOpen.store(false);
    impl->outboundVideoTrackOpen.store(false);
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

    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = config;
        snapshot_ = {};
        snapshot_.sessionId = config.sessionId;
        snapshot_.selfUserId = config.selfUserId;
        snapshot_.peerUserId = config.peerUserId;
        snapshot_.rtcRole = config.rtcRole;
        snapshot_.transportState = RtcTransportState::starting;
        started_ = true;
        impl_ = std::make_shared<Impl>();
    }

    if (!createPeerConnection()) {
        std::shared_ptr<Impl> failedImpl;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            failedImpl = std::exchange(impl_, nullptr);
            started_ = false;
            snapshot_.transportState = RtcTransportState::failed;
        }
        if (failedImpl->dataChannel) {
            try {
                failedImpl->dataChannel->close();
            } catch (...) {
            }
        }
        if (failedImpl->peerConnection) {
            try {
                failedImpl->peerConnection->close();
            } catch (...) {
            }
        }
        stopMediaRuntime(failedImpl);
        return false;
    }

    maybeCreateOutboundDataChannel();
    return true;
}

void RtcPeerSession::stop(const QString& reason) {
    std::shared_ptr<Impl> impl;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        impl = std::exchange(impl_, nullptr);
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

    if (impl) {
        if (impl->dataChannel) {
            try {
                impl->dataChannel->close();
            } catch (...) {
            }
        }
        if (impl->peerConnection) {
            try {
                impl->peerConnection->close();
            } catch (...) {
            }
        }
    }

    stopMediaRuntime(impl);
}

bool RtcPeerSession::applyRemoteDescription(const QString& sdp, const QString& descriptionType) {
    std::shared_ptr<::rtc::PeerConnection> peerConnection;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || !impl_ || !impl_->peerConnection) {
            return false;
        }
        peerConnection = impl_->peerConnection;
        snapshot_.transportState = RtcTransportState::connecting;
    }

    try {
        peerConnection->setRemoteDescription(::rtc::Description(toStdString(sdp), toStdString(descriptionType)));
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
        if (!started_ || !impl_ || !impl_->peerConnection) {
            return false;
        }
        peerConnection = impl_->peerConnection;
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

void RtcPeerSession::pushLocalVideoFrame(const media::VideoFrame& frame) {
    std::shared_ptr<Impl> impl;
    bool enabled = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        impl = impl_;
        enabled = started_ && config_.enableVideoSend;
    }
    if (!enabled || !impl || !frame.hasPixelPayload) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(impl->videoQueueMutex);
        pushBoundedVideoFrame(&impl->videoQueue, frame);
    }
    impl->videoQueueCondition.notify_one();
}

void RtcPeerSession::pushLocalAudioFrame(const media::AudioFrame& frame) {
    std::shared_ptr<Impl> impl;
    bool enabled = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        impl = impl_;
        enabled = started_ && config_.enableAudioSend;
    }
    if (!enabled || !impl || !frame.hasSamplePayload) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(impl->audioQueueMutex);
        pushBoundedAudioFrame(&impl->audioQueue, frame);
    }
    impl->audioQueueCondition.notify_one();
}

RtcPeerSessionSnapshot RtcPeerSession::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

bool RtcPeerSession::createPeerConnection() {
    std::shared_ptr<Impl> impl;
    RtcSessionConfig config;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        impl = impl_;
        config = config_;
    }
    if (!impl) {
        return false;
    }

    try {
        ::rtc::Configuration configuration;
        for (const QJsonValue& value : config.iceServers) {
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

        impl->remoteAudioSink.setLogCallback([this](const QString& message, bool warning) {
            emitLog(message, warning);
        });
        impl->remoteVideoSink.setLogCallback([this](const QString& message, bool warning) {
            emitLog(message, warning);
        });
        impl->remoteVideoSink.setFrameCallback([this](const media::VideoFrame& frame) {
            const auto callbacks = currentCallbacks();
            if (callbacks.onRemoteVideoFrame) {
                callbacks.onRemoteVideoFrame(frame);
            }
        });
        impl->remoteVideoSink.setClearCallback([this]() {
            const auto callbacks = currentCallbacks();
            if (callbacks.onRemoteVideoCleared) {
                callbacks.onRemoteVideoCleared();
            }
        });

        RtcRemoteAudioSink::Config audioSinkConfig;
        audioSinkConfig.expectedSampleRate = kDefaultOpusSampleRate;
        audioSinkConfig.expectedChannels = kDefaultOpusChannels;
        audioSinkConfig.enablePcmDump = config.enableDebugAudioDump;
        if (audioSinkConfig.enablePcmDump) {
            audioSinkConfig.pcmDumpPath = ensureDebugDumpPath(config, "remote-audio", "wav");
        }
        Status audioSinkStatus = impl->remoteAudioSink.start(audioSinkConfig);
        if (!audioSinkStatus.ok()) {
            updateSnapshot([&](RtcPeerSessionSnapshot& snapshot) {
                snapshot.lastError = QString::fromStdString(audioSinkStatus.message);
            });
            return false;
        }

        RtcRemoteVideoSink::Config videoSinkConfig;
        videoSinkConfig.enableH264Dump = config.enableDebugVideoDump;
        if (videoSinkConfig.enableH264Dump) {
            videoSinkConfig.h264DumpPath = ensureDebugDumpPath(config, "remote-video", "h264");
        }
        Status videoSinkStatus = impl->remoteVideoSink.start(videoSinkConfig);
        if (!videoSinkStatus.ok()) {
            updateSnapshot([&](RtcPeerSessionSnapshot& snapshot) {
                snapshot.lastError = QString::fromStdString(videoSinkStatus.message);
            });
            impl->remoteAudioSink.stop();
            return false;
        }

        impl->peerConnection = std::make_shared<::rtc::PeerConnection>(configuration);
        attachPeerConnectionHandlers(impl);

        ::rtc::Description::Audio inboundAudioDescription(
            toStdString(inboundAudioMid(config)), ::rtc::Description::Direction::RecvOnly);
        inboundAudioDescription.addOpusCodec(kOpusPayloadType);
        impl->inboundAudioTrack = impl->peerConnection->addTrack(inboundAudioDescription);
        auto audioDepacketizer = std::make_shared<::rtc::RtpDepacketizer>(kDefaultOpusSampleRate);
        auto audioReceivingSession = std::make_shared<::rtc::RtcpReceivingSession>();
        impl->inboundAudioTrack->setMediaHandler(audioDepacketizer);
        impl->inboundAudioTrack->chainMediaHandler(audioReceivingSession);
        impl->inboundAudioTrack->onFrame([impl](::rtc::binary message, ::rtc::FrameInfo frameInfo) {
            impl->remoteAudioSink.submitEncodedFrame(reinterpret_cast<const std::uint8_t*>(message.data()),
                                                    message.size(),
                                                    frameInfo.timestamp);
        });

        ::rtc::Description::Video inboundVideoDescription(
            toStdString(inboundVideoMid(config)), ::rtc::Description::Direction::RecvOnly);
        inboundVideoDescription.addH264Codec(kH264PayloadType);
        impl->inboundVideoTrack = impl->peerConnection->addTrack(inboundVideoDescription);
        auto videoDepacketizer =
            std::make_shared<::rtc::H264RtpDepacketizer>(::rtc::NalUnit::Separator::StartSequence);
        auto videoReceivingSession = std::make_shared<::rtc::RtcpReceivingSession>();
        impl->inboundVideoTrack->setMediaHandler(videoDepacketizer);
        impl->inboundVideoTrack->chainMediaHandler(videoReceivingSession);
        impl->inboundVideoTrack->onFrame([impl](::rtc::binary message, ::rtc::FrameInfo frameInfo) {
            impl->remoteVideoSink.submitEncodedFrame(reinterpret_cast<const std::uint8_t*>(message.data()),
                                                    message.size(),
                                                    frameInfo.timestamp);
        });
        impl->inboundVideoTrack->onOpen([track = impl->inboundVideoTrack]() {
            if (track) {
                track->requestKeyframe();
            }
        });

        if (config.enableAudioSend) {
            const std::uint32_t ssrc = stableSsrc(config, "audio");
            const std::string cname = toStdString(config.selfUserId) + "-audio";
            const std::string msid = toStdString(config.selfUserId) + "-stream";
            ::rtc::Description::Audio outboundAudioDescription(
                toStdString(outboundAudioMid(config)), ::rtc::Description::Direction::SendOnly);
            outboundAudioDescription.addOpusCodec(
                kOpusPayloadType,
                buildOpusProfile(std::max(config.localCapture.channels, 1), config.localAudioBitrate));
            outboundAudioDescription.addSSRC(ssrc, cname, msid, cname);
            impl->outboundAudioTrack = impl->peerConnection->addTrack(outboundAudioDescription);
            impl->outboundAudioRtpConfig = std::make_shared<::rtc::RtpPacketizationConfig>(
                ssrc, cname, static_cast<std::uint8_t>(kOpusPayloadType), kDefaultOpusSampleRate);
            auto packetizer = std::make_shared<::rtc::OpusRtpPacketizer>(impl->outboundAudioRtpConfig);
            auto senderReport = std::make_shared<::rtc::RtcpSrReporter>(impl->outboundAudioRtpConfig);
            packetizer->addToChain(senderReport);
            packetizer->addToChain(std::make_shared<::rtc::RtcpNackResponder>());
            impl->outboundAudioTrack->setMediaHandler(packetizer);
            impl->outboundAudioTrack->onOpen([impl]() {
                impl->outboundAudioTrackOpen.store(true);
            });
            impl->outboundAudioTrack->onClosed([impl]() {
                impl->outboundAudioTrackOpen.store(false);
            });
            impl->audioThread = std::thread(audioSendThreadMain, this, impl);
        }

        if (config.enableVideoSend) {
            const std::uint32_t ssrc = stableSsrc(config, "video");
            const std::string cname = toStdString(config.selfUserId) + "-video";
            const std::string msid = toStdString(config.selfUserId) + "-stream";
            ::rtc::Description::Video outboundVideoDescription(
                toStdString(outboundVideoMid(config)), ::rtc::Description::Direction::SendOnly);
            outboundVideoDescription.addH264Codec(kH264PayloadType);
            outboundVideoDescription.addSSRC(ssrc, cname, msid, cname);
            impl->outboundVideoTrack = impl->peerConnection->addTrack(outboundVideoDescription);
            impl->outboundVideoRtpConfig = std::make_shared<::rtc::RtpPacketizationConfig>(
                ssrc, cname, static_cast<std::uint8_t>(kH264PayloadType), ::rtc::H264RtpPacketizer::ClockRate);
            auto packetizer = std::make_shared<::rtc::H264RtpPacketizer>(
                ::rtc::NalUnit::Separator::StartSequence, impl->outboundVideoRtpConfig);
            auto senderReport = std::make_shared<::rtc::RtcpSrReporter>(impl->outboundVideoRtpConfig);
            packetizer->addToChain(senderReport);
            packetizer->addToChain(std::make_shared<::rtc::RtcpNackResponder>());
            impl->outboundVideoTrack->setMediaHandler(packetizer);
            impl->outboundVideoTrack->onOpen([impl]() {
                impl->outboundVideoTrackOpen.store(true);
            });
            impl->outboundVideoTrack->onClosed([impl]() {
                impl->outboundVideoTrackOpen.store(false);
            });
            impl->videoThread = std::thread(videoSendThreadMain, this, impl);
        }

        const bool shouldUseFallbackCapture =
            !config.preferExternalCapture &&
            ((config.enableVideoSend && !config.localCapture.videoDeviceId.empty()) ||
             (config.enableAudioSend && !config.localCapture.audioDeviceId.empty()));
        if (shouldUseFallbackCapture) {
            Status captureConfigStatus = impl->captureSource.configureDeviceSource(config.localCapture);
            if (!captureConfigStatus.ok()) {
                updateSnapshot([&](RtcPeerSessionSnapshot& snapshot) {
                    snapshot.lastError = QString::fromStdString(captureConfigStatus.message);
                });
                return false;
            }
            Status captureStartStatus = impl->captureSource.start();
            if (!captureStartStatus.ok()) {
                updateSnapshot([&](RtcPeerSessionSnapshot& snapshot) {
                    snapshot.lastError = QString::fromStdString(captureStartStatus.message);
                });
                return false;
            }
            impl->captureThread = std::thread(captureThreadMain, this, impl);
        }

        updateSnapshot([&](RtcPeerSessionSnapshot& snapshot) {
            snapshot.transportState = RtcTransportState::connecting;
        });
        return true;
    } catch (const std::exception& exception) {
        updateSnapshot([&](RtcPeerSessionSnapshot& snapshot) {
            snapshot.lastError = exception.what();
        });
        return false;
    }
}

void RtcPeerSession::attachPeerConnectionHandlers(const std::shared_ptr<Impl>& impl) {
    auto peerConnection = impl->peerConnection;

    peerConnection->onLocalDescription([this](::rtc::Description description) {
        const auto callbacks = currentCallbacks();
        if (callbacks.onLocalDescription) {
            callbacks.onLocalDescription(toQString(std::string(description)),
                                         toQString(description.typeString()));
        }
    });

    peerConnection->onLocalCandidate([this](::rtc::Candidate candidate) {
        const auto callbacks = currentCallbacks();
        if (callbacks.onLocalIceCandidate) {
            callbacks.onLocalIceCandidate(toQString(std::string(candidate)),
                                          toQString(candidate.mid()));
        }
    });

    peerConnection->onStateChange([this](::rtc::PeerConnection::State state) {
        bool notifyConnected = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
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

        const auto callbacks = currentCallbacks();
        if (notifyConnected && callbacks.onPeerConnectionConnected) {
            callbacks.onPeerConnectionConnected();
        }
    });

    peerConnection->onDataChannel([this](std::shared_ptr<::rtc::DataChannel> channel) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!impl_) {
                return;
            }
            impl_->dataChannel = channel;
        }
        attachDataChannelHandlers(channel);
    });

    peerConnection->onTrack([this, impl](std::shared_ptr<::rtc::Track> track) {
        if (!track) {
            return;
        }

        const std::string kind = track->description().type();
        if (kind == "audio" && (!impl->inboundAudioTrack || impl->inboundAudioTrack->mid() == track->mid())) {
            impl->inboundAudioTrack = track;
            auto depacketizer = std::make_shared<::rtc::RtpDepacketizer>(kDefaultOpusSampleRate);
            auto receivingSession = std::make_shared<::rtc::RtcpReceivingSession>();
            track->setMediaHandler(depacketizer);
            track->chainMediaHandler(receivingSession);
            track->onFrame([impl](::rtc::binary message, ::rtc::FrameInfo frameInfo) {
                impl->remoteAudioSink.submitEncodedFrame(reinterpret_cast<const std::uint8_t*>(message.data()),
                                                        message.size(),
                                                        frameInfo.timestamp);
            });
            return;
        }

        if (kind == "video" && (!impl->inboundVideoTrack || impl->inboundVideoTrack->mid() == track->mid())) {
            impl->inboundVideoTrack = track;
            auto depacketizer =
                std::make_shared<::rtc::H264RtpDepacketizer>(::rtc::NalUnit::Separator::StartSequence);
            auto receivingSession = std::make_shared<::rtc::RtcpReceivingSession>();
            track->setMediaHandler(depacketizer);
            track->chainMediaHandler(receivingSession);
            track->onFrame([impl](::rtc::binary message, ::rtc::FrameInfo frameInfo) {
                impl->remoteVideoSink.submitEncodedFrame(reinterpret_cast<const std::uint8_t*>(message.data()),
                                                        message.size(),
                                                        frameInfo.timestamp);
            });
            track->onOpen([track]() {
                if (track) {
                    track->requestKeyframe();
                }
            });
        }
    });
}

void RtcPeerSession::attachDataChannelHandlers(const std::shared_ptr<::rtc::DataChannel>& channel) {
    updateSnapshot([&](RtcPeerSessionSnapshot& snapshot) {
        snapshot.dataChannelState = RtcDataChannelState::opening;
    });

    channel->onOpen([this, channel]() {
        const auto config = currentConfig();
        updateSnapshot([&](RtcPeerSessionSnapshot& snapshot) {
            snapshot.dataChannelState = RtcDataChannelState::open;
            snapshot.pingSentCount += 1;
        });
        channel->send(QJsonDocument(buildPingPayload(config)).toJson(QJsonDocument::Compact).toStdString());
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

void RtcPeerSession::maybeCreateOutboundDataChannel() {
    std::shared_ptr<Impl> impl;
    QString rtcRole;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        impl = impl_;
        rtcRole = config_.rtcRole;
    }
    if (rtcRole.compare("offerer", Qt::CaseInsensitive) != 0 || !impl || !impl->peerConnection) {
        return;
    }

    const auto channel = impl->peerConnection->createDataChannel("cross-room-linkmic");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (impl_ != impl) {
            return;
        }
        impl_->dataChannel = channel;
    }
    attachDataChannelHandlers(channel);
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
