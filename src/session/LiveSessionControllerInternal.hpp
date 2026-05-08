#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "encode/AudioEncoder.hpp"
#include "encode/VideoEncoder.hpp"
#include "common/TraceLog.hpp"
#include "mux/FlvMuxer.hpp"
#include "publish/RtmpPublisher.hpp"
#include "session/LiveSessionController.hpp"
#include "spdlog/spdlog.h"

namespace live_studio::session {

inline constexpr std::size_t kVideoInputQueueCapacity = 8;
inline constexpr std::size_t kAudioInputQueueCapacity = 64;
inline constexpr std::size_t kSenderQueueCapacity = 256;
inline constexpr int kVideoTraceInterval = 30;
inline constexpr int kAudioTraceInterval = 50;
inline constexpr std::int64_t kPreviewFrameCopyIntervalNs = 100000000LL;
inline constexpr std::size_t kVideoQueueCoalesceThreshold = 2;

inline std::int64_t steadyClockNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline double nsToMs(std::int64_t valueNs) {
    return static_cast<double>(valueNs) / 1000000.0;
}

inline std::string formatDouble(double value, int precision = 3) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(precision) << value;
    return stream.str();
}

inline std::string formatSignedDouble(double value, int precision = 3) {
    std::ostringstream stream;
    stream << std::showpos << std::fixed << std::setprecision(precision) << value;
    return stream.str();
}

inline std::string formatQueueDepth(std::size_t depth, std::size_t capacity) {
    return std::to_string(static_cast<unsigned long long>(depth)) + "/" +
           std::to_string(static_cast<unsigned long long>(capacity));
}

inline std::int64_t relativeTimestampMs(std::int64_t captureWallClockNs, std::int64_t epochWallClockNs) {
    if (captureWallClockNs <= 0 || epochWallClockNs <= 0 || captureWallClockNs <= epochWallClockNs) {
        return 0;
    }
    return (captureWallClockNs - epochWallClockNs) / 1000000;
}

inline std::int64_t monotonicTimestampMs(std::int64_t captureWallClockNs,
                                         std::int64_t epochWallClockNs,
                                         std::int64_t* inOutLastTimestampMs) {
    if (inOutLastTimestampMs == nullptr) {
        return 0;
    }

    const std::int64_t candidate = relativeTimestampMs(captureWallClockNs, epochWallClockNs);
    if (candidate < *inOutLastTimestampMs) {
        return *inOutLastTimestampMs;
    }

    *inOutLastTimestampMs = candidate;
    return candidate;
}

inline AudioLevelSnapshot computeAudioLevel(const media::AudioFrame& frame) {
    AudioLevelSnapshot snapshot;
    snapshot.available = frame.hasSamplePayload && !frame.samplePayload.empty();
    snapshot.frameIndex = frame.frameIndex;
    snapshot.ptsMs = frame.ptsMs;
    if (!snapshot.available) {
        return snapshot;
    }

    const std::size_t sampleCount = frame.samplePayload.size() / sizeof(std::int16_t);
    if (sampleCount == 0) {
        snapshot.available = false;
        return snapshot;
    }

    const auto* samples = reinterpret_cast<const std::int16_t*>(frame.samplePayload.data());
    double sumSquares = 0.0;
    double peak = 0.0;
    for (std::size_t index = 0; index < sampleCount; ++index) {
        const double normalized = static_cast<double>(samples[index]) / 32768.0;
        sumSquares += normalized * normalized;
        peak = std::max(peak, std::abs(normalized));
    }

    snapshot.peak = peak;
    snapshot.rms = std::sqrt(sumSquares / static_cast<double>(sampleCount));
    return snapshot;
}

inline void pushTrace(TraceLog* trace, const std::string& stage, const std::string& detail) {
    spdlog::info("[{}] {}", stage, detail);
    if (trace != nullptr) {
        trace->push(stage, detail);
    }
}

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(std::max<std::size_t>(capacity, 1)) {}

    bool pushDropOldest(T item, bool* outDropped = nullptr, std::size_t* outSizeAfterPush = nullptr) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (closed_) {
            if (outDropped != nullptr) {
                *outDropped = false;
            }
            if (outSizeAfterPush != nullptr) {
                *outSizeAfterPush = items_.size();
            }
            return false;
        }

        bool dropped = false;
        if (items_.size() >= capacity_) {
            items_.pop_front();
            dropped = true;
        }
        items_.push_back(std::move(item));
        if (outDropped != nullptr) {
            *outDropped = dropped;
        }
        if (outSizeAfterPush != nullptr) {
            *outSizeAfterPush = items_.size();
        }
        notEmpty_.notify_one();
        return true;
    }

    bool pushWait(T item, std::size_t* outSizeAfterPush = nullptr) {
        std::unique_lock<std::mutex> lock(mutex_);
        notFull_.wait(lock, [&]() {
            return closed_ || items_.size() < capacity_;
        });
        if (closed_) {
            if (outSizeAfterPush != nullptr) {
                *outSizeAfterPush = items_.size();
            }
            return false;
        }

        items_.push_back(std::move(item));
        if (outSizeAfterPush != nullptr) {
            *outSizeAfterPush = items_.size();
        }
        notEmpty_.notify_one();
        return true;
    }

    bool pushUnbounded(T item, std::size_t* outSizeAfterPush = nullptr) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (closed_) {
            if (outSizeAfterPush != nullptr) {
                *outSizeAfterPush = items_.size();
            }
            return false;
        }

        items_.push_back(std::move(item));
        if (outSizeAfterPush != nullptr) {
            *outSizeAfterPush = items_.size();
        }
        notEmpty_.notify_one();
        return true;
    }

    bool coalesceToNewest(T* inOutItem,
                          std::size_t minPendingItems,
                          std::size_t* outDroppedCount = nullptr,
                          std::size_t* outSizeAfterCoalesce = nullptr) {
        if (inOutItem == nullptr) {
            return false;
        }

        std::unique_lock<std::mutex> lock(mutex_);
        if (outDroppedCount != nullptr) {
            *outDroppedCount = 0;
        }
        if (items_.size() < minPendingItems) {
            if (outSizeAfterCoalesce != nullptr) {
                *outSizeAfterCoalesce = items_.size();
            }
            return true;
        }

        const std::size_t droppedCount = items_.size();
        *inOutItem = std::move(items_.back());
        items_.clear();
        if (outDroppedCount != nullptr) {
            *outDroppedCount = droppedCount;
        }
        if (outSizeAfterCoalesce != nullptr) {
            *outSizeAfterCoalesce = items_.size();
        }
        notFull_.notify_all();
        return true;
    }

    bool pop(T* outItem) {
        if (outItem == nullptr) {
            return false;
        }

        std::unique_lock<std::mutex> lock(mutex_);
        notEmpty_.wait(lock, [&]() {
            return closed_ || !items_.empty();
        });
        if (items_.empty()) {
            return false;
        }

        *outItem = std::move(items_.front());
        items_.pop_front();
        notFull_.notify_one();
        return true;
    }

    void abort() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        items_.clear();
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
    std::deque<T> items_;
    bool closed_ = false;
};

struct VideoEncodeInput {
    media::VideoFrame frame;
    int pendingCaptureEvents = 0;
    std::size_t videoQueueDepthAfterPush = 0;
    std::size_t senderQueueDepthSnapshot = 0;
    int droppedVideoFrames = 0;
    std::int64_t enqueuedAtNs = 0;
};

struct AudioEncodeInput {
    media::AudioFrame frame;
    int pendingCaptureEvents = 0;
    std::size_t audioQueueDepthAfterPush = 0;
    std::size_t senderQueueDepthSnapshot = 0;
    int droppedAudioFrames = 0;
    std::int64_t enqueuedAtNs = 0;
};

enum class SenderWorkKind {
    video,
    audio,
};

struct SenderWorkItem {
    SenderWorkKind kind = SenderWorkKind::video;
    int captureFrameIndex = 0;
    std::int64_t capturePtsMs = 0;
    std::int64_t captureWallClockNs = 0;
    int pendingCaptureEvents = 0;
    int encodedSequence = 0;
    int droppedFrames = 0;
    std::size_t stageInputQueueDepthAfterPush = 0;
    std::size_t senderQueueDepthSnapshot = 0;
    std::size_t senderQueueDepthAfterPush = 0;
    double stageQueueWaitMs = 0.0;
    double prepareDurationMs = 0.0;
    double encodeDurationMs = 0.0;
    double frameCopyDurationMs = 0.0;
    double codecSendFrameDurationMs = 0.0;
    double codecReceivePacketDurationMs = 0.0;
    double packetFinalizeDurationMs = 0.0;
    double captureToEncodeDoneMs = 0.0;
    std::int64_t senderEnqueuedAtNs = 0;
    media::EncodedVideoAccessUnit videoUnit;
    media::EncodedAudioAccessUnit audioUnit;
};

struct VideoPerfWindow {
    std::int64_t windowStartNs = 0;
    int inputFrames = 0;
    int encodedFrames = 0;
    int keyFrames = 0;
    int pFrames = 0;
    double prepareDurationSumMs = 0.0;
    double keyEncodeDurationSumMs = 0.0;
    double pEncodeDurationSumMs = 0.0;
    double keyEncodeDurationMaxMs = 0.0;
    double pEncodeDurationMaxMs = 0.0;
    double frameCopyDurationSumMs = 0.0;
    double codecSendFrameDurationSumMs = 0.0;
    double codecReceivePacketDurationSumMs = 0.0;
    double packetFinalizeDurationSumMs = 0.0;
    bool hasQueueSample = false;
    std::size_t queueDepthFirst = 0;
    std::size_t queueDepthLast = 0;

    void reset(std::int64_t nextWindowStartNs) {
        windowStartNs = nextWindowStartNs;
        inputFrames = 0;
        encodedFrames = 0;
        keyFrames = 0;
        pFrames = 0;
        prepareDurationSumMs = 0.0;
        keyEncodeDurationSumMs = 0.0;
        pEncodeDurationSumMs = 0.0;
        keyEncodeDurationMaxMs = 0.0;
        pEncodeDurationMaxMs = 0.0;
        frameCopyDurationSumMs = 0.0;
        codecSendFrameDurationSumMs = 0.0;
        codecReceivePacketDurationSumMs = 0.0;
        packetFinalizeDurationSumMs = 0.0;
        hasQueueSample = false;
        queueDepthFirst = 0;
        queueDepthLast = 0;
    }
};

struct VideoPerfStats {
    std::mutex mutex;
    VideoPerfWindow window;
};

struct LiveSessionController::ControllerState {
    struct PublishSession {
        encode::VideoEncoder videoEncoder;
        encode::AudioEncoder audioEncoder;
        mux::FlvMuxer muxer;
        publish::RtmpPublisher publisher;
        bool audioEnabled = false;

        std::mutex timingMutex;
        bool epochInitialized = false;
        std::int64_t epochWallClockNs = 0;
        bool audioCaptureBaseInitialized = false;
        std::int64_t audioCaptureBaseWallClockNs = 0;
        bool audioEncodedBaseInitialized = false;
        std::int64_t audioEncodedBasePtsMs = 0;
        std::int64_t audioEncodedBaseDtsMs = 0;
        std::int64_t lastVideoTimestampMs = 0;
        std::int64_t lastAudioTimestampMs = 0;

        BoundedQueue<VideoEncodeInput> videoInputQueue{kVideoInputQueueCapacity};
        BoundedQueue<AudioEncodeInput> audioInputQueue{kAudioInputQueueCapacity};
        BoundedQueue<SenderWorkItem> senderQueue{kSenderQueueCapacity};

        std::thread videoThread;
        std::thread audioThread;
        std::thread senderThread;
        std::atomic_bool stopRequested{false};
        std::atomic_int droppedVideoFrames{0};
        std::atomic_int droppedAudioFrames{0};
        std::atomic_int nextVideoEncodedSequence{0};
        std::atomic_int nextAudioEncodedSequence{0};
        VideoPerfStats videoPerfStats;

        void requestStop() {
            stopRequested.store(true);
            videoInputQueue.abort();
            audioInputQueue.abort();
            senderQueue.abort();
        }
    };

    mutable std::mutex mutex;
    LiveSessionConfig config;
    PreviewFrameSnapshot latestPreviewFrame;
    AudioLevelSnapshot latestAudioLevel;
    SessionDiagnostics diagnostics;
    capture::LiveDeviceCaptureSource captureSource;
    std::thread workerThread;
    bool workerStopRequested = false;
    bool workerThreadRunning = false;
    std::int64_t lastPreviewFrameStoredNs = 0;
    TraceLog* trace = nullptr;
    std::unique_ptr<PublishSession> publishSession;
};

using PublishSession = LiveSessionController::ControllerState::PublishSession;

inline void updatePreviewStateLocked(LiveSessionController::ControllerState* state, const media::VideoFrame& frame) {
    ++state->diagnostics.previewedVideoFrames;

    const std::int64_t previewSampleNs = frame.captureWallClockNs > 0 ? frame.captureWallClockNs : steadyClockNowNs();
    const bool shouldStoreFrame =
        !state->latestPreviewFrame.available || state->lastPreviewFrameStoredNs <= 0 ||
        previewSampleNs <= state->lastPreviewFrameStoredNs ||
        (previewSampleNs - state->lastPreviewFrameStoredNs) >= kPreviewFrameCopyIntervalNs;
    if (!shouldStoreFrame) {
        return;
    }

    state->latestPreviewFrame.available = true;
    state->latestPreviewFrame.frameIndex = frame.frameIndex;
    state->latestPreviewFrame.ptsMs = frame.ptsMs;
    state->latestPreviewFrame.frame = frame;
    state->lastPreviewFrameStoredNs = previewSampleNs;
}

inline void updateAudioLevelLocked(LiveSessionController::ControllerState* state, const media::AudioFrame& frame) {
    state->latestAudioLevel = computeAudioLevel(frame);
    ++state->diagnostics.previewedAudioFrames;
}

inline void detachPublishSessionLocked(LiveSessionController::ControllerState* state,
                                       std::unique_ptr<PublishSession>* outSession) {
    if (state == nullptr || outSession == nullptr) {
        return;
    }

    *outSession = std::move(state->publishSession);
    state->diagnostics.publishing = false;
}

inline void finalizePublishSession(std::unique_ptr<PublishSession> session) {
    if (session == nullptr) {
        return;
    }

    session->requestStop();
    if (session->videoThread.joinable()) {
        session->videoThread.join();
    }
    if (session->audioThread.joinable()) {
        session->audioThread.join();
    }
    if (session->senderThread.joinable()) {
        session->senderThread.join();
    }
    session->publisher.disconnect();
}

std::string maybeRecordVideoPerfInput(PublishSession* session,
                                      std::int64_t sampleNs,
                                      std::size_t queueDepthAfterPush);

std::string maybeRecordVideoPerfEncoded(PublishSession* session,
                                        std::int64_t sampleNs,
                                        bool keyFrame,
                                        double prepareDurationMs,
                                        double encodeDurationMs,
                                        const encode::VideoEncodeTrace& encodeTrace);

Status seedPublishEpoch(PublishSession* session, std::int64_t captureWallClockNs);

Status prepareVideoFrameForPublish(PublishSession* session, media::VideoFrame frame, media::VideoFrame* outFrame);

Status normalizeAudioUnitsForPublish(PublishSession* session,
                                     const media::AudioFrame& sourceFrame,
                                     std::vector<media::EncodedAudioAccessUnit>* outUnits);

void signalPublishFailure(LiveSessionController::ControllerState* state,
                          PublishSession* session,
                          const Status& status,
                          const std::string& detailPrefix);

void senderThreadMain(LiveSessionController::ControllerState* state, PublishSession* session);
void videoEncodeThreadMain(LiveSessionController::ControllerState* state, PublishSession* session);
void audioEncodeThreadMain(LiveSessionController::ControllerState* state, PublishSession* session);
void workerMain(LiveSessionController::ControllerState* state);

}  // namespace live_studio::session
