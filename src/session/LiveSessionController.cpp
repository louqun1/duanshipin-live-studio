#include "session/LiveSessionControllerInternal.hpp"

namespace live_studio::session {

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
    state_->config.capture.height =
        probe.negotiatedVideoHeight > 0 ? probe.negotiatedVideoHeight : config.capture.height;
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
    state_->lastPreviewFrameStoredNs = 0;
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

void LiveSessionController::close() noexcept {
    std::thread workerThread;
    std::unique_ptr<PublishSession> publishSession;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        detachPublishSessionLocked(state_.get(), &publishSession);
        state_->workerStopRequested = true;
        workerThread = std::move(state_->workerThread);
    }

    finalizePublishSession(std::move(publishSession));
    state_->captureSource.stop();
    if (workerThread.joinable()) {
        workerThread.join();
    }

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->latestPreviewFrame = PreviewFrameSnapshot{};
        state_->latestAudioLevel = AudioLevelSnapshot{};
        state_->lastPreviewFrameStoredNs = 0;
        state_->diagnostics = SessionDiagnostics{};
        state_->config = LiveSessionConfig{};
        state_->workerStopRequested = false;
        state_->workerThreadRunning = false;
    }
}

void LiveSessionController::setMediaTapCallbacks(MediaTapCallbacks callbacks) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->mediaTapCallbacks = std::move(callbacks);
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

}  // namespace live_studio::session
