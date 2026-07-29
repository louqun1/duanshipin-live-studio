#include "session/LiveSessionControllerInternal.hpp"

namespace live_studio::session {

void workerMain(LiveSessionController::ControllerState* state) {
    if (state == nullptr) {
        return;
    }

    pushTrace(state->trace, "preview", "preview worker started");

    bool isDesktopMode = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        isDesktopMode = (state->config.captureMode == "desktop");
    }

    while (true) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->workerStopRequested) {
                break;
            }
        }

        capture::CaptureEvent event;
        const Status readStatus = isDesktopMode
            ? state->desktopSource.readNext(&event)
            : state->captureSource.readNext(&event);
        if (!readStatus.ok()) {
            bool expectedStop = false;
            std::unique_ptr<PublishSession> publishSession;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                expectedStop = state->workerStopRequested;
                if (!expectedStop) {
                    state->diagnostics.previewRunning = false;
                    state->diagnostics.lastError = readStatus.message;
                    detachPublishSessionLocked(state, &publishSession);
                }
                state->workerThreadRunning = false;
            }

            finalizePublishSession(std::move(publishSession));
            if (!expectedStop) {
                pushTrace(state->trace, "session", "preview worker stopped unexpectedly: " + readStatus.message);
            }
            return;
        }

        const int pendingEvents = isDesktopMode
            ? state->desktopSource.pendingEventCount()
            : state->captureSource.pendingEventCount();
        if (event.kind == capture::CaptureEventKind::video) {
            LiveSessionController::MediaTapCallbacks mediaTapCallbacks;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                mediaTapCallbacks = state->mediaTapCallbacks;
            }
            if (mediaTapCallbacks.onVideoFrame) {
                mediaTapCallbacks.onVideoFrame(event.videoFrame);
            }

            const int frameIndex = event.videoFrame.frameIndex;
            const std::int64_t ptsMs = event.videoFrame.ptsMs;
            const std::int64_t captureWallClockNs = event.videoFrame.captureWallClockNs;
            const double previewAgeMs =
                captureWallClockNs > 0 ? nsToMs(steadyClockNowNs() - captureWallClockNs) : 0.0;

            std::size_t videoQueueDepthAfterPush = 0;
            std::size_t senderQueueDepthSnapshot = 0;
            std::int64_t videoPerfSampleNs = 0;
            Status dispatchStatus = Status::Ok();
            PublishSession* activeSession = nullptr;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                updatePreviewStateLocked(state, event.videoFrame);
                if (state->publishSession != nullptr && state->diagnostics.publishing) {
                    activeSession = state->publishSession.get();
                    dispatchStatus = seedPublishEpoch(activeSession, captureWallClockNs);
                    if (dispatchStatus.ok()) {
                        senderQueueDepthSnapshot = activeSession->senderQueue.size();
                        VideoEncodeInput input;
                        input.frame = std::move(event.videoFrame);
                        input.pendingCaptureEvents = pendingEvents;
                        input.videoQueueDepthAfterPush = std::min(activeSession->videoInputQueue.size() + 1,
                                                                  activeSession->videoInputQueue.capacity());
                        input.senderQueueDepthSnapshot = senderQueueDepthSnapshot;
                        input.droppedVideoFrames = activeSession->droppedVideoFrames.load();
                        input.enqueuedAtNs = steadyClockNowNs();
                        videoPerfSampleNs = input.enqueuedAtNs;

                        bool droppedFrame = false;
                        const bool queued = activeSession->videoInputQueue.pushDropOldest(
                            std::move(input), &droppedFrame, &videoQueueDepthAfterPush);
                        if (!queued) {
                            dispatchStatus = Status::InternalError("video input queue rejected frame");
                        } else if (droppedFrame) {
                            activeSession->droppedVideoFrames.fetch_add(1);
                        }
                    }
                }
            }

            const std::string videoPerfTrace =
                dispatchStatus.ok() && activeSession != nullptr && videoPerfSampleNs > 0
                    ? maybeRecordVideoPerfInput(activeSession, videoPerfSampleNs, videoQueueDepthAfterPush)
                    : std::string();

            if (!dispatchStatus.ok() && activeSession != nullptr) {
                signalPublishFailure(state, activeSession, dispatchStatus, "video publish stopped: ");
            }

            if ((frameIndex % kVideoTraceInterval) == 0) {
                const std::size_t currentVideoQueueDepth =
                    activeSession != nullptr ? activeSession->videoInputQueue.size() : 0;
                const std::size_t currentSenderQueueDepth =
                    activeSession != nullptr ? activeSession->senderQueue.size() : 0;
                const int currentDroppedVideoFrames =
                    activeSession != nullptr ? activeSession->droppedVideoFrames.load() : 0;
                pushTrace(state->trace,
                          "preview-frame",
                          "captureFrame#" + std::to_string(frameIndex) + " pts=" + std::to_string(ptsMs) +
                              "ms captureAge=" + formatDouble(previewAgeMs) + "ms captureQ=" +
                              std::to_string(pendingEvents) + " videoQ=" +
                              formatQueueDepth(currentVideoQueueDepth, kVideoInputQueueCapacity) +
                              " senderQ=" + formatQueueDepth(currentSenderQueueDepth, kSenderQueueCapacity) +
                              " videoQ(after-dispatch)=" +
                              formatQueueDepth(videoQueueDepthAfterPush, kVideoInputQueueCapacity) +
                              " senderQ(before-dispatch)=" +
                              formatQueueDepth(senderQueueDepthSnapshot, kSenderQueueCapacity) +
                              " videoDrops=" + std::to_string(currentDroppedVideoFrames));
            }

            if (!videoPerfTrace.empty()) {
                pushTrace(state->trace, "publish-video-stats", videoPerfTrace);
            }
        } else {
            LiveSessionController::MediaTapCallbacks mediaTapCallbacks;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                mediaTapCallbacks = state->mediaTapCallbacks;
            }
            if (mediaTapCallbacks.onAudioFrame) {
                mediaTapCallbacks.onAudioFrame(event.audioFrame);
            }

            const int frameIndex = event.audioFrame.frameIndex;
            const std::int64_t ptsMs = event.audioFrame.ptsMs;
            const std::int64_t captureWallClockNs = event.audioFrame.captureWallClockNs;
            const double audioAgeMs =
                captureWallClockNs > 0 ? nsToMs(steadyClockNowNs() - captureWallClockNs) : 0.0;

            std::size_t audioQueueDepthAfterPush = 0;
            std::size_t senderQueueDepthSnapshot = 0;
            Status dispatchStatus = Status::Ok();
            PublishSession* activeSession = nullptr;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                updateAudioLevelLocked(state, event.audioFrame);
                if (state->publishSession != nullptr && state->diagnostics.publishing) {
                    activeSession = state->publishSession.get();
                    dispatchStatus = seedPublishEpoch(activeSession, captureWallClockNs);
                    if (dispatchStatus.ok()) {
                        senderQueueDepthSnapshot = activeSession->senderQueue.size();
                        AudioEncodeInput input;
                        input.frame = std::move(event.audioFrame);
                        input.pendingCaptureEvents = pendingEvents;
                        input.audioQueueDepthAfterPush = activeSession->audioInputQueue.size() + 1;
                        input.senderQueueDepthSnapshot = senderQueueDepthSnapshot;
                        input.droppedAudioFrames = activeSession->droppedAudioFrames.load();
                        input.enqueuedAtNs = steadyClockNowNs();
                        activeSession->audioInputQueue.pushUnbounded(std::move(input), &audioQueueDepthAfterPush);
                    }
                }
            }

            if (!dispatchStatus.ok() && activeSession != nullptr) {
                signalPublishFailure(state, activeSession, dispatchStatus, "audio publish stopped: ");
            }

            if ((frameIndex % kAudioTraceInterval) == 0) {
                const std::size_t currentAudioQueueDepth =
                    activeSession != nullptr ? activeSession->audioInputQueue.size() : 0;
                const std::size_t currentSenderQueueDepth =
                    activeSession != nullptr ? activeSession->senderQueue.size() : 0;
                const int currentDroppedAudioFrames =
                    activeSession != nullptr ? activeSession->droppedAudioFrames.load() : 0;
                pushTrace(state->trace,
                          "preview-audio",
                          "captureFrame#" + std::to_string(frameIndex) + " pts=" + std::to_string(ptsMs) +
                              "ms captureAge=" + formatDouble(audioAgeMs) + "ms captureQ=" +
                              std::to_string(pendingEvents) + " audioQ=" +
                              formatQueueDepth(currentAudioQueueDepth, kAudioInputQueueCapacity) +
                              " senderQ=" + formatQueueDepth(currentSenderQueueDepth, kSenderQueueCapacity) +
                              " audioQ(after-dispatch)=" +
                              formatQueueDepth(audioQueueDepthAfterPush, kAudioInputQueueCapacity) +
                              " senderQ(before-dispatch)=" +
                              formatQueueDepth(senderQueueDepthSnapshot, kSenderQueueCapacity) +
                              " audioDrops=" + std::to_string(currentDroppedAudioFrames));
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->workerThreadRunning = false;
    }
    pushTrace(state->trace, "preview", "preview worker stopped");
}

Status LiveSessionController::startPreview() {
    std::thread staleWorkerThread;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->diagnostics.opened) {
            return Status::InvalidArgument("startPreview requires open()");
        }
        if (state_->diagnostics.previewRunning) {
            return Status::Ok();
        }
        if (!state_->workerThreadRunning && state_->workerThread.joinable()) {
            staleWorkerThread = std::move(state_->workerThread);
        }
    }

    if (staleWorkerThread.joinable()) {
        staleWorkerThread.join();
    }

    const bool isDesktopMode = (state_->config.captureMode == "desktop");
    Status status = Status::Ok();

    if (isDesktopMode) {
        media::DesktopCaptureConfig desktopConfig;
        desktopConfig.sourceKind = "desktop";
        desktopConfig.sourceLabel = "desktop-source";
        desktopConfig.width = state_->config.capture.width;
        desktopConfig.height = state_->config.capture.height;
        desktopConfig.fps = state_->config.capture.fps;
        desktopConfig.frameCount = state_->config.capture.frameCount;

        status = state_->desktopSource.configure(desktopConfig);
        if (!status.ok()) {
            return status;
        }
        status = state_->desktopSource.start();
    } else {
        status = state_->captureSource.configureDeviceSource(state_->config.capture);
        if (!status.ok()) {
            return status;
        }
        status = state_->captureSource.start();
    }
    if (!status.ok()) {
        return status;
    }

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (isDesktopMode) {
            state_->config.capture.width = state_->desktopSource.config().width;
            state_->config.capture.height = state_->desktopSource.config().height;
            state_->config.capture.resolvedVideoDeviceName = "Desktop Screen";
            state_->diagnostics.previewRunning = true;
            state_->diagnostics.audioEnabled = false;
            state_->diagnostics.selectedVideoDeviceName = "Desktop Screen";
            state_->diagnostics.selectedAudioDeviceName.clear();
        } else {
            state_->config.capture = state_->captureSource.config();
            state_->diagnostics.previewRunning = true;
            state_->diagnostics.audioEnabled = state_->captureSource.hasAudio();
            state_->diagnostics.selectedVideoDeviceName = state_->captureSource.config().resolvedVideoDeviceName;
            state_->diagnostics.selectedAudioDeviceName = state_->captureSource.config().resolvedAudioDeviceName;
        }
        state_->workerStopRequested = false;
        state_->workerThreadRunning = true;
        state_->workerThread = std::thread(workerMain, state_.get());
    }

    pushTrace(state_->trace,
              "preview",
              "started: captureMode=" + (isDesktopMode ? std::string("desktop") : std::string("device")) +
                  " video=\"" + state_->diagnostics.selectedVideoDeviceName + "\" audio=\"" +
                  state_->diagnostics.selectedAudioDeviceName + "\"");
    return Status::Ok();
}

}  // namespace live_studio::session
