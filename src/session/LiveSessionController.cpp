#include "session/LiveSessionController.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "encode/AudioEncoder.hpp"
#include "encode/VideoEncoder.hpp"
#include "mux/FlvMuxer.hpp"
#include "publish/RtmpPublisher.hpp"
#include "spdlog/spdlog.h"

namespace live_studio::session {
namespace {

std::int64_t steadyClockNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

double nsToMs(std::int64_t valueNs) {
    return static_cast<double>(valueNs) / 1000000.0;
}

std::int64_t relativeTimestampMs(std::int64_t captureWallClockNs, std::int64_t epochWallClockNs) {
    if (captureWallClockNs <= 0 || epochWallClockNs <= 0 || captureWallClockNs <= epochWallClockNs) {
        return 0;
    }
    return (captureWallClockNs - epochWallClockNs) / 1000000;
}

std::int64_t monotonicTimestampMs(std::int64_t captureWallClockNs,
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

AudioLevelSnapshot computeAudioLevel(const media::AudioFrame& frame) {
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

void pushTrace(TraceLog* trace, const std::string& stage, const std::string& detail) {
    spdlog::info("[{}] {}", stage, detail);
    if (trace != nullptr) {
        trace->push(stage, detail);
    }
}

}  // namespace

struct LiveSessionController::ControllerState {
    struct PublishSession {
        encode::VideoEncoder videoEncoder;
        encode::AudioEncoder audioEncoder;
        mux::FlvMuxer muxer;
        publish::RtmpPublisher publisher;
        bool audioEnabled = false;
        bool epochInitialized = false;
        std::int64_t epochWallClockNs = 0;
        bool audioCaptureBaseInitialized = false;
        std::int64_t audioCaptureBaseWallClockNs = 0;
        bool audioEncodedBaseInitialized = false;
        std::int64_t audioEncodedBasePtsMs = 0;
        std::int64_t audioEncodedBaseDtsMs = 0;
        std::int64_t lastVideoTimestampMs = 0;
        std::int64_t lastAudioTimestampMs = 0;
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
    TraceLog* trace = nullptr;
    std::unique_ptr<PublishSession> publishSession;
};

namespace {

void stopPublishSessionLocked(LiveSessionController::ControllerState* state) {
    if (state == nullptr || state->publishSession == nullptr) {
        return;
    }

    state->publishSession->publisher.disconnect();
    state->publishSession.reset();
    state->diagnostics.publishing = false;
}

void failPublishSessionLocked(LiveSessionController::ControllerState* state, const Status& status) {
    if (state == nullptr) {
        return;
    }

    state->diagnostics.lastError = status.message;
    stopPublishSessionLocked(state);
}

void updatePreviewStateLocked(LiveSessionController::ControllerState* state, const media::VideoFrame& frame) {
    state->latestPreviewFrame.available = true;
    state->latestPreviewFrame.frameIndex = frame.frameIndex;
    state->latestPreviewFrame.ptsMs = frame.ptsMs;
    state->latestPreviewFrame.frame = frame;
    ++state->diagnostics.previewedVideoFrames;
}

void updateAudioLevelLocked(LiveSessionController::ControllerState* state, const media::AudioFrame& frame) {
    state->latestAudioLevel = computeAudioLevel(frame);
    ++state->diagnostics.previewedAudioFrames;
}

Status ensurePublishEpoch(LiveSessionController::ControllerState::PublishSession* session,
                          std::int64_t captureWallClockNs) {
    if (session == nullptr) {
        return Status::InvalidArgument("publish session is null");
    }

    if (!session->epochInitialized) {
        if (captureWallClockNs <= 0) {
            return Status::InvalidArgument("captureWallClockNs must be positive");
        }
        session->epochInitialized = true;
        session->epochWallClockNs = captureWallClockNs;
    }

    return Status::Ok();
}

void workerMain(LiveSessionController::ControllerState* state) {
    if (state == nullptr) {
        return;
    }

    pushTrace(state->trace, "preview", "preview worker started");

    while (true) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->workerStopRequested) {
                break;
            }
        }

        capture::CaptureEvent event;
        const Status readStatus = state->captureSource.readNext(&event);
        if (!readStatus.ok()) {
            bool expectedStop = false;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                expectedStop = state->workerStopRequested;
                if (!expectedStop) {
                    state->diagnostics.previewRunning = false;
                    state->diagnostics.lastError = readStatus.message;
                    stopPublishSessionLocked(state);
                }
                state->workerThreadRunning = false;
            }

            if (!expectedStop) {
                pushTrace(state->trace, "session", "preview worker stopped unexpectedly: " + readStatus.message);
            }
            return;
        }

        std::string traceStage;
        std::string traceDetail;

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (event.kind == capture::CaptureEventKind::video) {
                const int pendingEvents = state->captureSource.pendingEventCount();
                const double previewAgeMs = event.videoFrame.captureWallClockNs > 0
                                                ? nsToMs(steadyClockNowNs() - event.videoFrame.captureWallClockNs)
                                                : 0.0;
                updatePreviewStateLocked(state, event.videoFrame);
                if ((event.videoFrame.frameIndex % 30) == 0) {
                    pushTrace(state->trace,
                              "preview-frame",
                              "frame#" + std::to_string(event.videoFrame.frameIndex) + " pts=" +
                                  std::to_string(event.videoFrame.ptsMs) + "ms captureAge=" +
                                  std::to_string(previewAgeMs) + "ms queue=" + std::to_string(pendingEvents));
                }

                if (state->publishSession != nullptr) {
                    LiveSessionController::ControllerState::PublishSession* publishSession =
                        state->publishSession.get();
                    Status status = ensurePublishEpoch(publishSession, event.videoFrame.captureWallClockNs);
                    if (!status.ok()) {
                        failPublishSessionLocked(state, status);
                        traceStage = "publish";
                        traceDetail = "video publish stopped: " + status.message;
                    } else {
                        media::VideoFrame publishFrame = event.videoFrame;
                        publishFrame.ptsMs = monotonicTimestampMs(event.videoFrame.captureWallClockNs,
                                                                  publishSession->epochWallClockNs,
                                                                  &publishSession->lastVideoTimestampMs);

                        media::EncodedVideoAccessUnit accessUnit;
                        status = publishSession->videoEncoder.encodeFrame(publishFrame, &accessUnit);
                        if (!status.ok()) {
                            failPublishSessionLocked(state, status);
                            traceStage = "publish";
                            traceDetail = "video publish stopped: " + status.message;
                        } else {
                            media::FlvVideoTagPlan tag;
                            status = publishSession->muxer.buildVideoNaluTag(accessUnit, &tag);
                            if (!status.ok()) {
                                failPublishSessionLocked(state, status);
                                traceStage = "publish";
                                traceDetail = "video publish stopped: " + status.message;
                            } else {
                                media::RtmpMessagePlan message;
                                status = publishSession->publisher.mapVideoTagToMessage(tag, &message);
                                if (!status.ok()) {
                                    failPublishSessionLocked(state, status);
                                    traceStage = "publish";
                                    traceDetail = "video publish stopped: " + status.message;
                                } else {
                                    status = publishSession->publisher.publishVideoTag(message);
                                    if (!status.ok()) {
                                        failPublishSessionLocked(state, status);
                                        traceStage = "publish";
                                        traceDetail = "video publish stopped: " + status.message;
                                    } else {
                                        ++state->diagnostics.publishedVideoFrames;
                                        state->diagnostics.lastPublishedSummary = message.payloadSummary;
                                        if ((accessUnit.frameIndex % 30) == 0) {
                                            pushTrace(state->trace,
                                                      "publish-video",
                                                      "frame#" + std::to_string(accessUnit.frameIndex) + " pts=" +
                                                          std::to_string(accessUnit.ptsMs) + "ms dts=" +
                                                          std::to_string(accessUnit.dtsMs) + "ms queue=" +
                                                          std::to_string(pendingEvents) + " summary=" +
                                                          message.payloadSummary);
                                        }
                                        if (!state->diagnostics.firstMediaPacketSent) {
                                            state->diagnostics.firstMediaPacketSent = true;
                                            state->diagnostics.firstMediaLatencyMs =
                                                nsToMs(steadyClockNowNs() - event.videoFrame.captureWallClockNs);
                                            traceStage = "latency";
                                            traceDetail = "first-media-packet: media=video latency=" +
                                                          std::to_string(state->diagnostics.firstMediaLatencyMs) + "ms";
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            } else {
                const int pendingEvents = state->captureSource.pendingEventCount();
                const double audioAgeMs = event.audioFrame.captureWallClockNs > 0
                                              ? nsToMs(steadyClockNowNs() - event.audioFrame.captureWallClockNs)
                                              : 0.0;
                updateAudioLevelLocked(state, event.audioFrame);
                if ((event.audioFrame.frameIndex % 50) == 0) {
                    pushTrace(state->trace,
                              "preview-audio",
                              "frame#" + std::to_string(event.audioFrame.frameIndex) + " pts=" +
                                  std::to_string(event.audioFrame.ptsMs) + "ms captureAge=" +
                                  std::to_string(audioAgeMs) + "ms queue=" + std::to_string(pendingEvents));
                }

                if (state->publishSession != nullptr) {
                    LiveSessionController::ControllerState::PublishSession* publishSession =
                        state->publishSession.get();
                    Status status = ensurePublishEpoch(publishSession, event.audioFrame.captureWallClockNs);
                    if (!status.ok()) {
                        failPublishSessionLocked(state, status);
                        traceStage = "publish";
                        traceDetail = "audio publish stopped: " + status.message;
                    } else {
                        if (!publishSession->audioCaptureBaseInitialized) {
                            publishSession->audioCaptureBaseInitialized = true;
                            publishSession->audioCaptureBaseWallClockNs = event.audioFrame.captureWallClockNs;
                        }

                        std::vector<media::EncodedAudioAccessUnit> audioUnits;
                        status = publishSession->audioEncoder.encodeFrame(event.audioFrame, &audioUnits);
                        if (!status.ok()) {
                            failPublishSessionLocked(state, status);
                            traceStage = "publish";
                            traceDetail = "audio publish stopped: " + status.message;
                        } else {
                            const std::int64_t audioBaseOffsetMs =
                                relativeTimestampMs(publishSession->audioCaptureBaseWallClockNs,
                                                    publishSession->epochWallClockNs);

                            for (media::EncodedAudioAccessUnit& unit : audioUnits) {
                                if (!publishSession->audioEncodedBaseInitialized) {
                                    publishSession->audioEncodedBaseInitialized = true;
                                    publishSession->audioEncodedBasePtsMs = unit.ptsMs;
                                    publishSession->audioEncodedBaseDtsMs = unit.dtsMs;
                                }

                                std::int64_t normalizedPtsMs =
                                    audioBaseOffsetMs + std::max<std::int64_t>(0, unit.ptsMs - publishSession->audioEncodedBasePtsMs);
                                std::int64_t normalizedDtsMs =
                                    audioBaseOffsetMs + std::max<std::int64_t>(0, unit.dtsMs - publishSession->audioEncodedBaseDtsMs);
                                if (normalizedDtsMs < publishSession->lastAudioTimestampMs) {
                                    normalizedDtsMs = publishSession->lastAudioTimestampMs;
                                }
                                if (normalizedPtsMs < normalizedDtsMs) {
                                    normalizedPtsMs = normalizedDtsMs;
                                }

                                publishSession->lastAudioTimestampMs = normalizedDtsMs;
                                unit.ptsMs = normalizedPtsMs;
                                unit.dtsMs = normalizedDtsMs;

                                media::FlvAudioTagPlan tag;
                                status = publishSession->muxer.buildAudioRawTag(
                                    unit, state->captureSource.config().channels, &tag);
                                if (!status.ok()) {
                                    failPublishSessionLocked(state, status);
                                    traceStage = "publish";
                                    traceDetail = "audio publish stopped: " + status.message;
                                    break;
                                }

                                media::RtmpMessagePlan message;
                                status = publishSession->publisher.mapAudioTagToMessage(tag, &message);
                                if (!status.ok()) {
                                    failPublishSessionLocked(state, status);
                                    traceStage = "publish";
                                    traceDetail = "audio publish stopped: " + status.message;
                                    break;
                                }

                                status = publishSession->publisher.publishAudioTag(message);
                                if (!status.ok()) {
                                    failPublishSessionLocked(state, status);
                                    traceStage = "publish";
                                    traceDetail = "audio publish stopped: " + status.message;
                                    break;
                                }

                                ++state->diagnostics.publishedAudioUnits;
                                state->diagnostics.lastPublishedSummary = message.payloadSummary;
                                if ((unit.frameIndex % 50) == 0) {
                                    pushTrace(state->trace,
                                              "publish-audio",
                                              "frame#" + std::to_string(unit.frameIndex) + " pts=" +
                                                  std::to_string(unit.ptsMs) + "ms dts=" +
                                                  std::to_string(unit.dtsMs) + "ms queue=" +
                                                  std::to_string(pendingEvents) + " summary=" +
                                                  message.payloadSummary);
                                }
                                if (!state->diagnostics.firstMediaPacketSent) {
                                    state->diagnostics.firstMediaPacketSent = true;
                                    state->diagnostics.firstMediaLatencyMs =
                                        nsToMs(steadyClockNowNs() - publishSession->audioCaptureBaseWallClockNs);
                                    traceStage = "latency";
                                    traceDetail = "first-media-packet: media=audio latency=" +
                                                  std::to_string(state->diagnostics.firstMediaLatencyMs) + "ms";
                                }
                            }
                        }
                    }
                }
            }
        }

        if (!traceStage.empty()) {
            pushTrace(state->trace, traceStage, traceDetail);
        }
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->workerThreadRunning = false;
    }
    pushTrace(state->trace, "preview", "preview worker stopped");
}

}  // namespace

LiveSessionController::LiveSessionController() : state_(std::make_unique<ControllerState>()) {}

LiveSessionController::~LiveSessionController() {
    close();
}

Status LiveSessionController::open(const LiveSessionConfig& config) {
    if (config.capture.videoDeviceId.empty()) {
        return Status::InvalidArgument("open requires capture.videoDeviceId");
    }

    close();

    capture::LiveDeviceProbeResult probe;
    Status status = capture::probeLiveDeviceCaptureConfig(config.capture, &probe);
    if (!status.ok()) {
        return status;
    }

    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->config = config;
    state_->config.capture.width = probe.negotiatedVideoWidth > 0 ? probe.negotiatedVideoWidth : config.capture.width;
    state_->config.capture.height = probe.negotiatedVideoHeight > 0 ? probe.negotiatedVideoHeight : config.capture.height;
    state_->config.capture.fps = probe.negotiatedVideoFps > 0 ? probe.negotiatedVideoFps : config.capture.fps;
    state_->config.capture.resolvedVideoDeviceName = probe.selectedVideoDeviceName;
    if (!config.capture.audioDeviceId.empty()) {
        state_->config.capture.sampleRate =
            probe.negotiatedSampleRate > 0 ? probe.negotiatedSampleRate : config.capture.sampleRate;
        state_->config.capture.channels =
            probe.negotiatedChannels > 0 ? probe.negotiatedChannels : config.capture.channels;
        state_->config.capture.resolvedAudioDeviceName = probe.selectedAudioDeviceName;
    } else {
        state_->config.capture.resolvedAudioDeviceName.clear();
    }

    state_->latestPreviewFrame = PreviewFrameSnapshot{};
    state_->latestAudioLevel = AudioLevelSnapshot{};
    state_->diagnostics = SessionDiagnostics{};
    state_->diagnostics.opened = true;
    state_->diagnostics.audioEnabled = !config.capture.audioDeviceId.empty();
    state_->diagnostics.selectedVideoDeviceName = probe.selectedVideoDeviceName;
    state_->diagnostics.selectedAudioDeviceName = probe.selectedAudioDeviceName;
    state_->diagnostics.probe = std::move(probe);

    pushTrace(state_->trace,
              "session",
              "opened: video=\"" + state_->diagnostics.selectedVideoDeviceName + "\" audio=\"" +
                  state_->diagnostics.selectedAudioDeviceName + "\"");
    return Status::Ok();
}

Status LiveSessionController::openSingleVideoStage(const LiveSessionConfig& config) {
    return open(config);
}

Status LiveSessionController::startPreview() {
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->diagnostics.opened) {
            return Status::InvalidArgument("startPreview requires open()");
        }
        if (state_->diagnostics.previewRunning) {
            return Status::Ok();
        }
    }

    Status status = state_->captureSource.configureDeviceSource(state_->config.capture);
    if (!status.ok()) {
        return status;
    }

    status = state_->captureSource.start();
    if (!status.ok()) {
        return status;
    }

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->config.capture = state_->captureSource.config();
        state_->diagnostics.previewRunning = true;
        state_->diagnostics.audioEnabled = state_->captureSource.hasAudio();
        state_->diagnostics.selectedVideoDeviceName = state_->captureSource.config().resolvedVideoDeviceName;
        state_->diagnostics.selectedAudioDeviceName = state_->captureSource.config().resolvedAudioDeviceName;
        state_->workerStopRequested = false;
        state_->workerThreadRunning = true;
        state_->workerThread = std::thread(workerMain, state_.get());
    }

    pushTrace(state_->trace,
              "preview",
              "started: video=\"" + state_->captureSource.config().resolvedVideoDeviceName + "\" audio=\"" +
                  state_->captureSource.config().resolvedAudioDeviceName + "\"");
    return Status::Ok();
}

Status LiveSessionController::startPublish() {
    bool needsPreview = false;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->diagnostics.opened) {
            return Status::InvalidArgument("startPublish requires open()");
        }
        if (state_->publishSession != nullptr) {
            return Status::Ok();
        }
        needsPreview = !state_->diagnostics.previewRunning;
        if (state_->config.rtmpUrl.empty()) {
            return Status::InvalidArgument("startPublish requires a non-empty rtmpUrl");
        }
    }

    if (needsPreview) {
        Status previewStatus = startPreview();
        if (!previewStatus.ok()) {
            return previewStatus;
        }
    }

    auto publishSession = std::make_unique<ControllerState::PublishSession>();
    const media::LiveDeviceCaptureConfig captureConfig = state_->captureSource.config();

    Status status = publishSession->videoEncoder.configureH264(
        captureConfig.width, captureConfig.height, captureConfig.fps, state_->config.videoBitrate);
    if (!status.ok()) {
        return status;
    }

    status = publishSession->publisher.configureVideoStream(captureConfig.width, captureConfig.height, captureConfig.fps);
    if (!status.ok()) {
        return status;
    }

    publishSession->audioEnabled = state_->captureSource.hasAudio();
    if (publishSession->audioEnabled) {
        status = publishSession->audioEncoder.configureAac(
            captureConfig.sampleRate, captureConfig.channels, state_->config.audioBitrate);
        if (!status.ok()) {
            return status;
        }

        status = publishSession->publisher.configureAudioStream(captureConfig.sampleRate, captureConfig.channels);
        if (!status.ok()) {
            return status;
        }
    }

    status = publishSession->publisher.connect(state_->config.rtmpUrl);
    if (!status.ok()) {
        return status;
    }

    media::H264ParameterSets parameterSets;
    status = publishSession->videoEncoder.buildSequenceHeader(&parameterSets);
    if (!status.ok()) {
        publishSession->publisher.disconnect();
        return status;
    }

    media::FlvVideoTagPlan videoSequenceHeaderTag;
    status = publishSession->muxer.buildVideoSequenceHeader(parameterSets, &videoSequenceHeaderTag);
    if (!status.ok()) {
        publishSession->publisher.disconnect();
        return status;
    }

    media::RtmpMessagePlan videoSequenceHeaderMessage;
    status = publishSession->publisher.mapVideoTagToMessage(videoSequenceHeaderTag, &videoSequenceHeaderMessage);
    if (!status.ok()) {
        publishSession->publisher.disconnect();
        return status;
    }

    status = publishSession->publisher.publishVideoTag(videoSequenceHeaderMessage);
    if (!status.ok()) {
        publishSession->publisher.disconnect();
        return status;
    }

    if (publishSession->audioEnabled) {
        media::AacDecoderConfig decoderConfig;
        status = publishSession->audioEncoder.buildSequenceHeader(&decoderConfig);
        if (!status.ok()) {
            publishSession->publisher.disconnect();
            return status;
        }

        media::FlvAudioTagPlan audioSequenceHeaderTag;
        status = publishSession->muxer.buildAudioSequenceHeader(decoderConfig, &audioSequenceHeaderTag);
        if (!status.ok()) {
            publishSession->publisher.disconnect();
            return status;
        }

        media::RtmpMessagePlan audioSequenceHeaderMessage;
        status = publishSession->publisher.mapAudioTagToMessage(audioSequenceHeaderTag, &audioSequenceHeaderMessage);
        if (!status.ok()) {
            publishSession->publisher.disconnect();
            return status;
        }

        status = publishSession->publisher.publishAudioTag(audioSequenceHeaderMessage);
        if (!status.ok()) {
            publishSession->publisher.disconnect();
            return status;
        }
    }

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->publishSession = std::move(publishSession);
        state_->diagnostics.publishing = true;
        state_->diagnostics.lastError.clear();
        state_->diagnostics.lastPublishedSummary.clear();
        state_->diagnostics.firstMediaPacketSent = false;
        state_->diagnostics.firstMediaLatencyMs = 0.0;
        state_->diagnostics.publishedVideoFrames = 0;
        state_->diagnostics.publishedAudioUnits = 0;
    }

    pushTrace(state_->trace, "publish", "publisher entered publishing state for " + state_->config.rtmpUrl);
    return Status::Ok();
}

Status LiveSessionController::stopPublish() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->publishSession == nullptr) {
        return Status::Ok();
    }

    stopPublishSessionLocked(state_.get());
    pushTrace(state_->trace, "publish", "publish session stopped and reset");
    return Status::Ok();
}

void LiveSessionController::close() noexcept {
    std::thread workerThread;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        stopPublishSessionLocked(state_.get());
        state_->workerStopRequested = true;
        workerThread = std::move(state_->workerThread);
    }

    state_->captureSource.stop();
    if (workerThread.joinable()) {
        workerThread.join();
    }

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->latestPreviewFrame = PreviewFrameSnapshot{};
        state_->latestAudioLevel = AudioLevelSnapshot{};
        state_->diagnostics = SessionDiagnostics{};
        state_->config = LiveSessionConfig{};
        state_->workerStopRequested = false;
        state_->workerThreadRunning = false;
    }
}

PreviewFrameSnapshot LiveSessionController::latestPreviewFrame() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->latestPreviewFrame;
}

AudioLevelSnapshot LiveSessionController::latestAudioLevel() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->latestAudioLevel;
}

SessionDiagnostics LiveSessionController::diagnostics() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->diagnostics;
}

Status LiveSessionController::runStage1VideoTrace(const std::string& rtmpUrl,
                                                  const std::string& captureConfigPath,
                                                  int frameCountOverride,
                                                  TraceLog* trace) {
    if (trace == nullptr) {
        return Status::InvalidArgument("trace is null");
    }
    if (captureConfigPath.empty()) {
        return Status::InvalidArgument("captureConfigPath is empty");
    }
    if (frameCountOverride < 0) {
        return Status::InvalidArgument("frameCountOverride must be >= 0");
    }

    media::LiveDeviceCaptureConfig captureConfig;
    Status status = capture::loadLiveDeviceCaptureConfig(captureConfigPath, &captureConfig);
    if (!status.ok()) {
        return status;
    }

    const int targetVideoFrameCount = frameCountOverride > 0 ? frameCountOverride : captureConfig.frameCount;
    LiveSessionConfig config;
    config.capture = captureConfig;
    config.rtmpUrl = rtmpUrl;

    state_->trace = trace;
    pushTrace(trace, "session", "stage1 trace begins");
    pushTrace(trace,
              "latency",
              "push-side latency probe armed: measuring first real media packet from capture wall clock to send completion");

    status = open(config);
    if (!status.ok()) {
        state_->trace = nullptr;
        return status;
    }

    status = startPreview();
    if (!status.ok()) {
        close();
        state_->trace = nullptr;
        return status;
    }

    status = startPublish();
    if (!status.ok()) {
        close();
        state_->trace = nullptr;
        return status;
    }

    while (true) {
        const SessionDiagnostics current = diagnostics();
        if (!current.lastError.empty() && !current.publishing) {
            close();
            state_->trace = nullptr;
            return Status::InternalError(current.lastError);
        }
        if (current.publishedVideoFrames >= targetVideoFrameCount) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    stopPublish();
    close();
    pushTrace(trace, "session", "stage1 trace finished cleanly");
    state_->trace = nullptr;
    return Status::Ok();
}

}  // namespace live_studio::session
