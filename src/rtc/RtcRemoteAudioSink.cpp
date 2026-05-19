#include "rtc/RtcRemoteAudioSink.hpp"

#include <QByteArray>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "audio/SdlAudioOutput.hpp"
#include "common/WavFileWriter.hpp"
#include "decode/OpusAudioDecoder.hpp"

namespace live_studio::rtc {
namespace {

constexpr std::size_t kMaxQueuedAudioPackets = 256;

struct QueuedAudioPacket {
    std::vector<std::uint8_t> payload;
    std::uint32_t rtpTimestamp = 0;
};

}  // namespace

struct RtcRemoteAudioSink::State {
    Config config;
    decode::OpusAudioDecoder decoder;
    audio::SdlAudioOutput audioOutput;
    common::WavFileWriter wavWriter;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<QueuedAudioPacket> queue;
    bool stopRequested = false;
    bool started = false;
    bool firstTimestampInitialized = false;
    std::uint32_t firstRtpTimestamp = 0;
};

RtcRemoteAudioSink::RtcRemoteAudioSink() = default;

RtcRemoteAudioSink::~RtcRemoteAudioSink() {
    stop();
}

void RtcRemoteAudioSink::setLogCallback(LogCallback callback) {
    logCallback_ = std::move(callback);
    if (state_ != nullptr) {
        state_->audioOutput.setLogCallback(logCallback_);
    }
}

Status RtcRemoteAudioSink::start(const Config& config) {
    stop();

    auto nextState = std::make_unique<State>();
    nextState->config = config;
    nextState->audioOutput.setLogCallback(logCallback_);

    Status status = nextState->decoder.configure(config.expectedSampleRate, config.expectedChannels);
    if (!status.ok()) {
        return status;
    }

    nextState->started = true;
    nextState->worker = std::thread([this, rawState = nextState.get()]() {
        while (true) {
            QueuedAudioPacket packet;
            {
                std::unique_lock<std::mutex> lock(rawState->mutex);
                rawState->condition.wait(lock, [&]() {
                    return rawState->stopRequested || !rawState->queue.empty();
                });
                if (rawState->stopRequested && rawState->queue.empty()) {
                    break;
                }
                packet = std::move(rawState->queue.front());
                rawState->queue.pop_front();
            }

            if (!rawState->firstTimestampInitialized) {
                rawState->firstTimestampInitialized = true;
                rawState->firstRtpTimestamp = packet.rtpTimestamp;
            }
            const std::int64_t ptsSamplesHint =
                static_cast<std::int64_t>(static_cast<std::uint32_t>(packet.rtpTimestamp - rawState->firstRtpTimestamp));

            std::vector<media::AudioFrame> decodedFrames;
            const Status decodeStatus = rawState->decoder.decodePacket(
                packet.payload.data(), packet.payload.size(), ptsSamplesHint, &decodedFrames);
            if (!decodeStatus.ok()) {
                emitLog(QString("remote Opus decode failed: %1").arg(QString::fromStdString(decodeStatus.message)),
                        true);
                continue;
            }

            for (const auto& decodedFrame : decodedFrames) {
                if (rawState->config.enablePcmDump) {
                    if (!rawState->wavWriter.isOpen()) {
                        const Status openStatus = rawState->wavWriter.open(rawState->config.pcmDumpPath.toStdString(),
                                                                          decodedFrame.sampleRate,
                                                                          decodedFrame.channels);
                        if (!openStatus.ok()) {
                            emitLog(QString("failed to open RTC audio dump: %1")
                                        .arg(QString::fromStdString(openStatus.message)),
                                    true);
                            rawState->config.enablePcmDump = false;
                        }
                    }
                    if (rawState->config.enablePcmDump) {
                        const Status writeStatus = rawState->wavWriter.appendFrame(decodedFrame);
                        if (!writeStatus.ok()) {
                            emitLog(QString("failed to write RTC audio dump: %1")
                                        .arg(QString::fromStdString(writeStatus.message)),
                                    true);
                            rawState->config.enablePcmDump = false;
                            rawState->wavWriter.close();
                        }
                    }
                }

                const QByteArray pcm(reinterpret_cast<const char*>(decodedFrame.samplePayload.data()),
                                     static_cast<int>(decodedFrame.samplePayload.size()));
                if (!rawState->audioOutput.enqueuePcm(pcm,
                                                      decodedFrame.ptsMs,
                                                      decodedFrame.sampleRate,
                                                      decodedFrame.channels,
                                                      decodedFrame.samplesPerChannel)) {
                    emitLog("failed to enqueue remote PCM into SDL audio output", true);
                }
            }
        }
    });

    state_ = std::move(nextState);
    return Status::Ok();
}

void RtcRemoteAudioSink::stop() {
    if (state_ == nullptr) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->stopRequested = true;
        state_->condition.notify_all();
    }
    if (state_->worker.joinable()) {
        state_->worker.join();
    }
    state_->audioOutput.stop();
    state_->wavWriter.close();
    state_.reset();
}

void RtcRemoteAudioSink::submitEncodedFrame(const std::uint8_t* data,
                                            std::size_t size,
                                            std::uint32_t rtpTimestamp) {
    if (state_ == nullptr || data == nullptr || size == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->stopRequested) {
        return;
    }
    if (state_->queue.size() >= kMaxQueuedAudioPackets) {
        state_->queue.pop_front();
    }
    QueuedAudioPacket packet;
    packet.payload.assign(data, data + size);
    packet.rtpTimestamp = rtpTimestamp;
    state_->queue.push_back(std::move(packet));
    state_->condition.notify_one();
}

void RtcRemoteAudioSink::emitLog(const QString& message, bool warning) const {
    if (logCallback_) {
        logCallback_(message, warning);
    }
}

}  // namespace live_studio::rtc
