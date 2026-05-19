#include "rtc/RtcRemoteVideoSink.hpp"

#include <fstream>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "decode/H264VideoDecoder.hpp"

namespace live_studio::rtc {
namespace {

constexpr std::size_t kMaxQueuedVideoPackets = 64;
constexpr std::int64_t kVideoClockRate = 90000;

struct QueuedVideoPacket {
    std::vector<std::uint8_t> payload;
    std::uint32_t rtpTimestamp = 0;
};

}  // namespace

struct RtcRemoteVideoSink::State {
    Config config;
    decode::H264VideoDecoder decoder;
    std::ofstream h264Dump;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<QueuedVideoPacket> queue;
    bool stopRequested = false;
    bool firstTimestampInitialized = false;
    std::uint32_t firstRtpTimestamp = 0;
};

RtcRemoteVideoSink::RtcRemoteVideoSink() = default;

RtcRemoteVideoSink::~RtcRemoteVideoSink() {
    stop();
}

void RtcRemoteVideoSink::setFrameCallback(FrameCallback callback) {
    frameCallback_ = std::move(callback);
}

void RtcRemoteVideoSink::setClearCallback(ClearCallback callback) {
    clearCallback_ = std::move(callback);
}

void RtcRemoteVideoSink::setLogCallback(LogCallback callback) {
    logCallback_ = std::move(callback);
}

Status RtcRemoteVideoSink::start(const Config& config) {
    stop();

    auto nextState = std::make_unique<State>();
    nextState->config = config;
    Status status = nextState->decoder.configure();
    if (!status.ok()) {
        return status;
    }

    if (config.enableH264Dump && !config.h264DumpPath.trimmed().isEmpty()) {
        nextState->h264Dump.open(config.h264DumpPath.toStdString(), std::ios::binary | std::ios::out);
        if (!nextState->h264Dump.is_open()) {
            emitLog(QString("failed to open RTC H.264 dump: %1").arg(config.h264DumpPath), true);
            nextState->config.enableH264Dump = false;
        }
    }

    nextState->worker = std::thread([this, rawState = nextState.get()]() {
        while (true) {
            QueuedVideoPacket packet;
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
            const std::uint32_t timestampDelta = packet.rtpTimestamp - rawState->firstRtpTimestamp;
            const std::int64_t ptsMsHint =
                (static_cast<std::int64_t>(timestampDelta) * 1000) / kVideoClockRate;

            if (rawState->config.enableH264Dump && rawState->h264Dump.is_open()) {
                rawState->h264Dump.write(reinterpret_cast<const char*>(packet.payload.data()),
                                         static_cast<std::streamsize>(packet.payload.size()));
            }

            std::vector<media::VideoFrame> decodedFrames;
            const Status decodeStatus = rawState->decoder.decodeAccessUnit(
                packet.payload.data(), packet.payload.size(), ptsMsHint, &decodedFrames);
            if (!decodeStatus.ok()) {
                emitLog(QString("remote H.264 decode failed: %1").arg(QString::fromStdString(decodeStatus.message)),
                        true);
                continue;
            }

            for (const auto& frame : decodedFrames) {
                if (frameCallback_) {
                    frameCallback_(frame);
                }
            }
        }
    });

    state_ = std::move(nextState);
    return Status::Ok();
}

void RtcRemoteVideoSink::stop() {
    if (state_ == nullptr) {
        if (clearCallback_) {
            clearCallback_();
        }
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
    if (state_->h264Dump.is_open()) {
        state_->h264Dump.close();
    }
    state_.reset();
    if (clearCallback_) {
        clearCallback_();
    }
}

void RtcRemoteVideoSink::submitEncodedFrame(const std::uint8_t* data,
                                            std::size_t size,
                                            std::uint32_t rtpTimestamp) {
    if (state_ == nullptr || data == nullptr || size == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->stopRequested) {
        return;
    }
    if (state_->queue.size() >= kMaxQueuedVideoPackets) {
        state_->queue.pop_front();
    }
    QueuedVideoPacket packet;
    packet.payload.assign(data, data + size);
    packet.rtpTimestamp = rtpTimestamp;
    state_->queue.push_back(std::move(packet));
    state_->condition.notify_one();
}

void RtcRemoteVideoSink::emitLog(const QString& message, bool warning) const {
    if (logCallback_) {
        logCallback_(message, warning);
    }
}

}  // namespace live_studio::rtc
