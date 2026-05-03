#include "RtcAudioProbe.hpp"

#include <rtc/rtcpnackresponder.hpp>
#include <rtc/rtpdepacketizer.hpp>
#include <rtc/rtcpreceivingsession.hpp>
#include <rtc/rtcpsrreporter.hpp>
#include <rtc/rtppacketizationconfig.hpp>
#include <rtc/rtppacketizer.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <utility>

using json = nlohmann::json;

namespace rtc_probe_common {
namespace {

constexpr char kDefaultRecvAudioWavDir[] = "logs/rtc-audio";
constexpr int kDefaultOpusSampleRate = 48000;
constexpr int kDefaultOpusChannels = 2;
constexpr int kMinimumDecodedAudioSuccessMs = 500;
constexpr char kControllerAudioMid[] = "controller-audio";
constexpr char kParticipantAudioMid[] = "participant-audio";
constexpr char kAudioProbeCompleteMessageType[] = "probe.audio.complete";

void ThrowIfStatusNotOk(const live_studio::Status& status, const std::string& context) {
    if (!status.ok()) {
        throw std::runtime_error(context + ": " + status.message);
    }
}

std::string SanitizeFileComponent(std::string value) {
    for (char& ch : value) {
        const bool keep = std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_';
        if (!keep) {
            ch = '_';
        }
    }
    if (value.empty()) {
        return "audio";
    }
    return value;
}

std::int64_t NowEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

RtcAudioProbe::RtcAudioProbe(RtcAudioProbeConfig config, RtcAudioProbeCallbacks callbacks)
    : config_(std::move(config)), callbacks_(std::move(callbacks)) {}

RtcAudioProbe::~RtcAudioProbe() {
    outbound_.stopRequested.store(true);
    outbound_.captureSource.stop();
    if (outbound_.worker.joinable()) {
        outbound_.worker.join();
    }
    (void)inbound_.wavWriter.close();
}

bool RtcAudioProbe::shouldSendMic() const noexcept {
    return config_.sendMic;
}

bool RtcAudioProbe::shouldExpectRemoteAudio() const noexcept {
    return config_.expectRemoteAudio;
}

bool RtcAudioProbe::hasSatisfiedInboundAudioProbe() const {
    return hasDecodedContinuousAudio();
}

void RtcAudioProbe::bindDataChannel(const std::shared_ptr<rtc::DataChannel>& dc) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    dataChannel_ = dc;
}

void RtcAudioProbe::ensureInboundReady(const std::shared_ptr<rtc::PeerConnection>& pc) {
    if (!shouldExpectRemoteAudio()) {
        return;
    }
    if (inbound_.initialized) {
        return;
    }
    if (!pc) {
        throw std::runtime_error("cannot initialize inbound audio before PeerConnection exists");
    }

    inbound_.wavPath = config_.recvAudioWavPath.empty() ? defaultRecvAudioWavPath() : config_.recvAudioWavPath;
    ThrowIfStatusNotOk(inbound_.decoder.configure(kDefaultOpusSampleRate, kDefaultOpusChannels),
                       "failed to configure inbound Opus decoder");

    rtc::Description::Audio audioDescription(inboundAudioMid(), rtc::Description::Direction::RecvOnly);
    audioDescription.addOpusCodec(outbound_.payloadType);

    inbound_.track = pc->addTrack(audioDescription);
    auto depacketizer = std::make_shared<rtc::RtpDepacketizer>(kDefaultOpusSampleRate);
    auto receivingSession = std::make_shared<rtc::RtcpReceivingSession>();
    inbound_.track->setMediaHandler(depacketizer);
    inbound_.track->chainMediaHandler(receivingSession);
    inbound_.track->onFrame([this](rtc::binary message, rtc::FrameInfo frameInfo) {
        const int packets = inboundAudioPackets_.fetch_add(1) + 1;

        bool expected = false;
        if (inboundAudioFirstPacketLogged_.compare_exchange_strong(expected, true)) {
            logEvent("rtc.audio-packet-recv",
                     "first inbound Opus payload size=" + std::to_string(message.size()) +
                         " ts=" + std::to_string(frameInfo.timestamp));
        } else if (packets % 100 == 0) {
            logEvent("rtc.audio-packet-recv", "packets=" + std::to_string(packets));
        }

        std::string failureMessage;
        bool logFirstDecodedFrame = false;
        bool logContinuousAudio = false;
        bool logProgress = false;
        int totalDecodedFrames = 0;

        {
            std::lock_guard<std::mutex> lock(inbound_.mutex);
            if (!inbound_.firstRtpTimestamp.has_value()) {
                inbound_.firstRtpTimestamp = frameInfo.timestamp;
            }

            const std::int64_t ptsSamplesHint = static_cast<std::int64_t>(
                static_cast<std::uint32_t>(frameInfo.timestamp - *inbound_.firstRtpTimestamp));

            std::vector<live_studio::media::AudioFrame> decodedFrames;
            const live_studio::Status decodeStatus = inbound_.decoder.decodePacket(
                reinterpret_cast<const std::uint8_t*>(message.data()),
                message.size(),
                ptsSamplesHint,
                &decodedFrames);
            if (!decodeStatus.ok()) {
                failureMessage = "Opus decode failed: " + decodeStatus.message;
            } else {
                std::int64_t packetSamples = 0;
                for (const auto& decodedFrame : decodedFrames) {
                    if (!inbound_.wavWriter.isOpen()) {
                        const live_studio::Status openStatus = inbound_.wavWriter.open(
                            inbound_.wavPath,
                            decodedFrame.sampleRate,
                            decodedFrame.channels);
                        if (!openStatus.ok()) {
                            failureMessage = "failed to open inbound WAV file: " + openStatus.message;
                            break;
                        }
                        inbound_.outputSampleRate.store(decodedFrame.sampleRate);
                        inbound_.outputChannels.store(decodedFrame.channels);
                    }

                    const live_studio::Status writeStatus = inbound_.wavWriter.appendFrame(decodedFrame);
                    if (!writeStatus.ok()) {
                        failureMessage = "failed to append decoded PCM to WAV: " + writeStatus.message;
                        break;
                    }

                    packetSamples += decodedFrame.samplesPerChannel;
                }

                if (failureMessage.empty() && !decodedFrames.empty()) {
                    totalDecodedFrames =
                        inbound_.decodedFrames.fetch_add(static_cast<int>(decodedFrames.size())) +
                        static_cast<int>(decodedFrames.size());
                    inbound_.decodedSamples.fetch_add(packetSamples);

                    expected = false;
                    if (inbound_.firstDecodedFrameLogged.compare_exchange_strong(expected, true)) {
                        logFirstDecodedFrame = true;
                    }

                    expected = false;
                    if (hasDecodedContinuousAudio() &&
                        inbound_.continuousAudioLogged.compare_exchange_strong(expected, true)) {
                        logContinuousAudio = true;
                    }

                    logProgress = totalDecodedFrames % 25 == 0;
                }
            }
        }

        if (!failureMessage.empty()) {
            recordFailure(failureMessage);
            return;
        }

        if (logFirstDecodedFrame) {
            logEvent("rtc.audio-decode-start", buildInboundAudioSummary());
        } else if (logContinuousAudio || logProgress) {
            logEvent(logContinuousAudio ? "rtc.audio-decode-ready" : "rtc.audio-decode-progress",
                     buildInboundAudioSummary());
        }

        if (logContinuousAudio) {
            maybeMarkMediaScenarioDone("decoded continuous inbound audio");
        }
    });
    attachInboundTrackHandlers(inbound_.track, config_.targetId);
    inbound_.initialized = true;

    logEvent("rtc.audio-track-recv-ready",
             "prepared recvonly Opus audio track before answer wav=" + inbound_.wavPath);
}

void RtcAudioProbe::ensureOutboundReady(const std::shared_ptr<rtc::PeerConnection>& pc) {
    if (!shouldSendMic()) {
        return;
    }
    if (outbound_.initialized) {
        return;
    }
    if (!pc) {
        throw std::runtime_error("cannot initialize outbound audio before PeerConnection exists");
    }

    ThrowIfStatusNotOk(outbound_.captureSource.configureDeviceSource(config_.micCaptureConfigPath),
                       "failed to load mic capture config");
    ThrowIfStatusNotOk(outbound_.captureSource.start(), "failed to start microphone capture");
    if (!outbound_.captureSource.hasAudio()) {
        throw std::runtime_error("mic capture config did not enable audio_device");
    }

    const auto& captureConfig = outbound_.captureSource.config();
    ThrowIfStatusNotOk(outbound_.encoder.configure(captureConfig.sampleRate,
                                                   captureConfig.channels,
                                                   config_.opusBitrate),
                       "failed to configure Opus encoder");

    const std::uint32_t ssrc = stableSsrc();
    const std::string cname = config_.peerId + "-audio";
    const std::string msid = config_.peerId + "-stream";

    rtc::Description::Audio audioDescription(outboundAudioMid());
    audioDescription.addOpusCodec(outbound_.payloadType,
                                  buildOpusProfile(captureConfig.channels, config_.opusBitrate));
    audioDescription.addSSRC(ssrc, cname, msid, cname);

    outbound_.track = pc->addTrack(audioDescription);
    auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(ssrc,
                                                                   cname,
                                                                   static_cast<std::uint8_t>(outbound_.payloadType),
                                                                   kDefaultOpusSampleRate);
    auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(rtpConfig);
    auto senderReport = std::make_shared<rtc::RtcpSrReporter>(rtpConfig);
    packetizer->addToChain(senderReport);
    packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
    outbound_.track->setMediaHandler(packetizer);

    attachOutboundTrackHandlers(outbound_.track, config_.targetId);

    outbound_.worker = std::thread(&RtcAudioProbe::outboundAudioWorkerMain, this);
    outbound_.initialized = true;

    logEvent("rtc.audio-track-added",
             "configured " + captureConfig.resolvedAudioDeviceName + " " +
                 std::to_string(captureConfig.sampleRate) + "Hz/" +
                 std::to_string(captureConfig.channels) + "ch bitrate=" +
                 std::to_string(config_.opusBitrate));
}

void RtcAudioProbe::attachInboundTrackHandlers(const std::shared_ptr<rtc::Track>& track,
                                               const std::string& peerId) {
    const std::string mid = track->mid();
    const std::string kind = track->description().type();

    track->onOpen([this, peerId, mid, kind]() {
        logEvent("rtc.track-open", "opened mid=" + mid + " kind=" + kind, peerId);
    });

    track->onClosed([this, peerId, mid, kind]() {
        const bool shutdownRequested = isShutdownRequested();
        const bool shouldTreatAsRemoteComplete =
            !shutdownRequested && shouldExpectRemoteAudio() && kind == "audio" && hasSatisfiedInboundAudioProbe();
        const bool shouldFail =
            !shutdownRequested && shouldExpectRemoteAudio() && kind == "audio" && !hasSatisfiedInboundAudioProbe();

        logEvent("rtc.track-closed", "closed mid=" + mid + " kind=" + kind, peerId);

        if (shouldTreatAsRemoteComplete) {
            remoteOutboundAudioCompleted_.store(true);
            logEvent("rtc.audio-recv-complete", buildInboundAudioSummary(), peerId);
            maybeMarkMediaScenarioDone("remote inbound audio track closed after successful decode");
        } else if (shouldFail && !isDone()) {
            recordFailure("inbound audio track closed before decoding continuous audio, " +
                          buildInboundAudioSummary());
        }
    });

    track->onError([this, peerId, mid](const std::string& error) {
        recordFailure("track " + mid + " error: " + error + " peer=" + peerId);
    });
}

void RtcAudioProbe::onPeerConnectionStateChange(rtc::PeerConnection::State state) {
    if (shouldSendMic()) {
        if (state == rtc::PeerConnection::State::Connected) {
            outbound_.trackOpen.store(true);
            logEvent("rtc.audio-send-ready", "peer connection connected, outbound audio may flow");
        } else if (state == rtc::PeerConnection::State::Disconnected ||
                   state == rtc::PeerConnection::State::Failed ||
                   state == rtc::PeerConnection::State::Closed) {
            outbound_.trackOpen.store(false);
        }
    }
}

void RtcAudioProbe::onDataChannelOpen() {
    if (!shouldSendMic() && !shouldExpectRemoteAudio()) {
        markDone();
        return;
    }
    maybeSendOutboundCompletionSignal();
    maybeMarkMediaScenarioDone("data channel opened after media prerequisites were satisfied");
}

bool RtcAudioProbe::handleDataChannelStringMessage(const std::string& payload, const std::string& peerId) {
    json message = json::parse(payload, nullptr, false);
    if (!message.is_object() || message.value("type", "") != kAudioProbeCompleteMessageType) {
        return false;
    }

    remoteOutboundAudioCompleted_.store(true);
    logEvent("rtc.audio-peer-complete", payload, peerId);
    maybeMarkMediaScenarioDone("received remote outbound audio completion signal");
    return true;
}

void RtcAudioProbe::checkMediaTimers() {
    if (!shouldSendMic()) {
        return;
    }

    bool shouldComplete = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        shouldComplete = outbound_.completionDeadline.has_value() &&
            std::chrono::steady_clock::now() >= *outbound_.completionDeadline;
        if (shouldComplete) {
            outbound_.completionDeadline.reset();
        }
    }

    if (!shouldComplete) {
        return;
    }

    logEvent("rtc.media-run-complete", buildOutboundAudioSummary());
    localOutboundAudioCompleted_.store(true);
    maybeSendOutboundCompletionSignal();
    maybeMarkMediaScenarioDone("local outbound audio media run completed");
}

std::string RtcAudioProbe::currentRequestId() const {
    return callbacks_.currentRequestId ? callbacks_.currentRequestId() : std::string();
}

bool RtcAudioProbe::isDone() const {
    return callbacks_.isDone ? callbacks_.isDone() : false;
}

bool RtcAudioProbe::isShutdownRequested() const {
    return callbacks_.isShutdownRequested ? callbacks_.isShutdownRequested() : false;
}

void RtcAudioProbe::logEvent(const std::string& event,
                             const std::string& detail,
                             const std::string& remoteUserId) const {
    if (callbacks_.logEvent) {
        callbacks_.logEvent(event, detail, remoteUserId.empty() ? config_.targetId : remoteUserId);
    }
}

void RtcAudioProbe::recordFailure(const std::string& message) const {
    if (callbacks_.recordFailure) {
        callbacks_.recordFailure(message);
    }
}

void RtcAudioProbe::markDone() const {
    if (callbacks_.markDone) {
        callbacks_.markDone();
    }
}

std::string RtcAudioProbe::outboundAudioMid() const {
    return config_.role == "controller" ? kControllerAudioMid : kParticipantAudioMid;
}

std::string RtcAudioProbe::inboundAudioMid() const {
    return config_.role == "controller" ? kParticipantAudioMid : kControllerAudioMid;
}

std::uint32_t RtcAudioProbe::stableSsrc() const {
    const std::string seed = config_.peerId + "|" + config_.targetId + "|" + config_.roomId + "|audio";
    const auto hashed = static_cast<std::uint32_t>(std::hash<std::string>{}(seed));
    return hashed == 0 ? 1u : hashed;
}

std::string RtcAudioProbe::buildOpusProfile(int channels, int bitrate) const {
    const bool stereo = channels > 1;
    return "minptime=10;maxaveragebitrate=" + std::to_string(std::max(6000, bitrate)) +
           ";stereo=" + (stereo ? "1" : "0") +
           ";sprop-stereo=" + (stereo ? "1" : "0") +
           ";useinbandfec=1";
}

std::string RtcAudioProbe::defaultRecvAudioWavPath() const {
    const std::string fileName =
        SanitizeFileComponent(config_.roomId) + "-" +
        SanitizeFileComponent(config_.peerId) + "-from-" +
        SanitizeFileComponent(config_.targetId) + "-" +
        std::to_string(NowEpochMs()) + ".wav";
    return (std::filesystem::path(kDefaultRecvAudioWavDir) / fileName).string();
}

std::string RtcAudioProbe::buildInboundAudioSummary() const {
    int sampleRate = inbound_.outputSampleRate.load();
    if (sampleRate <= 0) {
        sampleRate = kDefaultOpusSampleRate;
    }

    std::ostringstream detail;
    detail << "decoded_packets=" << inboundAudioPackets_.load()
           << " decoded_frames=" << inbound_.decodedFrames.load()
           << " decoded_ms=" << ((inbound_.decodedSamples.load() * 1000) / std::max(1, sampleRate))
           << " sample_rate=" << sampleRate
           << " channels=" << inbound_.outputChannels.load()
           << " wav=" << inbound_.wavPath;
    return detail.str();
}

std::string RtcAudioProbe::buildOutboundAudioSummary() const {
    std::ostringstream detail;
    detail << "sent_packets=" << outbound_.sentPackets.load()
           << " captured_frames=" << outbound_.capturedFrames.load()
           << " dropped_before_track_open=" << outbound_.droppedFramesBeforeTrackOpen.load()
           << " sent_samples=" << outbound_.sentSamples.load();
    return detail.str();
}

std::int64_t RtcAudioProbe::minimumDecodedSamplesForSuccess() const {
    int sampleRate = inbound_.outputSampleRate.load();
    if (sampleRate <= 0) {
        sampleRate = kDefaultOpusSampleRate;
    }
    return (static_cast<std::int64_t>(sampleRate) * kMinimumDecodedAudioSuccessMs) / 1000;
}

bool RtcAudioProbe::hasDecodedContinuousAudio() const {
    return inbound_.decodedSamples.load() >= minimumDecodedSamplesForSuccess();
}

std::string RtcAudioProbe::buildAudioProbeCompleteMessage() const {
    json message = {
        {"type", kAudioProbeCompleteMessageType},
        {"from_user_id", config_.peerId},
        {"sent_packets", outbound_.sentPackets.load()},
        {"sent_samples", outbound_.sentSamples.load()},
        {"captured_frames", outbound_.capturedFrames.load()},
        {"dropped_before_track_open", outbound_.droppedFramesBeforeTrackOpen.load()}
    };
    return message.dump();
}

void RtcAudioProbe::attachOutboundTrackHandlers(const std::shared_ptr<rtc::Track>& track,
                                                const std::string& peerId) {
    const std::string mid = track->mid();

    track->onOpen([this, peerId, mid]() {
        outbound_.trackOpen.store(true);
        logEvent("rtc.audio-track-open", "opened mid=" + mid, peerId);
    });

    track->onClosed([this, peerId, mid]() {
        outbound_.trackOpen.store(false);

        logEvent("rtc.audio-track-closed", "closed mid=" + mid, peerId);

        if (!isShutdownRequested() && shouldSendMic() && !isDone()) {
            recordFailure("outbound audio track closed before the media run completed");
        }
    });

    track->onError([this, peerId, mid](const std::string& error) {
        recordFailure("outbound audio track " + mid + " error: " + error + " peer=" + peerId);
    });
}

void RtcAudioProbe::outboundAudioWorkerMain() {
    try {
        while (!outbound_.stopRequested.load() && !isDone()) {
            live_studio::capture::CaptureEvent event;
            const live_studio::Status readStatus = outbound_.captureSource.readNext(&event);
            if (!readStatus.ok()) {
                if (!outbound_.stopRequested.load() && !isDone()) {
                    recordFailure("microphone capture failed: " + readStatus.message);
                }
                break;
            }

            if (event.kind != live_studio::capture::CaptureEventKind::audio) {
                continue;
            }

            outbound_.capturedFrames.fetch_add(1);
            if (!outbound_.trackOpen.load()) {
                outbound_.droppedFramesBeforeTrackOpen.fetch_add(1);
                continue;
            }

            std::vector<live_studio::encode::EncodedOpusFrame> encodedFrames;
            const live_studio::Status encodeStatus = outbound_.encoder.encodeFrame(event.audioFrame, &encodedFrames);
            if (!encodeStatus.ok()) {
                recordFailure("Opus encode failed: " + encodeStatus.message);
                break;
            }

            for (const auto& encodedFrame : encodedFrames) {
                if (outbound_.stopRequested.load() || isDone()) {
                    break;
                }

                try {
                    outbound_.track->sendFrame(
                        reinterpret_cast<const rtc::byte*>(encodedFrame.payload.data()),
                        encodedFrame.payload.size(),
                        std::chrono::duration<double>(
                            static_cast<double>(encodedFrame.ptsSamples) /
                            static_cast<double>(std::max(1, encodedFrame.sampleRate))));
                } catch (const std::exception& exception) {
                    recordFailure(std::string("failed to send Opus frame: ") + exception.what());
                    break;
                }

                const int sentPackets = outbound_.sentPackets.fetch_add(1) + 1;
                outbound_.sentSamples.fetch_add(encodedFrame.samplesPerChannel);

                bool firstPacket = false;
                bool expected = false;
                if (outbound_.firstPacketSent.compare_exchange_strong(expected, true)) {
                    firstPacket = true;
                    {
                        std::lock_guard<std::mutex> lock(stateMutex_);
                        outbound_.completionDeadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(config_.mediaRunMs);
                    }
                    logEvent("rtc.audio-send-start",
                             "first packet sent " + encodedFrame.payloadSummary +
                                 " run_ms=" + std::to_string(config_.mediaRunMs));
                }

                if (firstPacket || sentPackets % 100 == 0) {
                    logEvent("rtc.audio-packet-sent",
                             encodedFrame.payloadSummary + " packets=" + std::to_string(sentPackets));
                }
            }
        }
    } catch (const std::exception& exception) {
        if (!outbound_.stopRequested.load() && !isDone()) {
            recordFailure(std::string("outbound audio worker failed: ") + exception.what());
        }
    }
}

void RtcAudioProbe::maybeMarkMediaScenarioDone(const std::string& reason) {
    if (isDone()) {
        return;
    }

    const bool outboundSatisfied = !shouldSendMic() || localOutboundAudioCompleted_.load();
    const bool inboundSatisfied = !shouldExpectRemoteAudio() || hasSatisfiedInboundAudioProbe();
    const bool remoteCompletionSatisfied = !shouldExpectRemoteAudio() || remoteOutboundAudioCompleted_.load();
    if (!outboundSatisfied || !inboundSatisfied || !remoteCompletionSatisfied) {
        return;
    }

    bool expected = false;
    if (mediaCompletionLogged_.compare_exchange_strong(expected, true)) {
        std::ostringstream detail;
        detail << reason;
        if (shouldSendMic()) {
            detail << " outbound{" << buildOutboundAudioSummary() << "}";
        }
        if (shouldExpectRemoteAudio()) {
            detail << " inbound{" << buildInboundAudioSummary() << "}";
        }
        logEvent("rtc.media-probe-complete", detail.str());
    }

    markDone();
}

void RtcAudioProbe::maybeSendOutboundCompletionSignal() {
    if (!shouldSendMic() || !localOutboundAudioCompleted_.load()) {
        return;
    }

    std::shared_ptr<rtc::DataChannel> dc;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        dc = dataChannel_.lock();
    }
    if (!dc || !dc->isOpen()) {
        return;
    }

    bool expected = false;
    if (!outboundCompletionSignalSent_.compare_exchange_strong(expected, true)) {
        return;
    }

    try {
        dc->send(buildAudioProbeCompleteMessage());
        logEvent("rtc.audio-send-complete-signal", buildOutboundAudioSummary());
    } catch (const std::exception& exception) {
        outboundCompletionSignalSent_.store(false);
        recordFailure(std::string("failed to send outbound audio completion signal: ") + exception.what());
    }
}

}  // namespace rtc_probe_common
