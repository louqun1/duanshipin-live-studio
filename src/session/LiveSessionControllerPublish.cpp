#include "session/LiveSessionControllerInternal.hpp"

namespace live_studio::session {

Status LiveSessionController::startPublish() {
    bool needsPreview = false;
    std::unique_ptr<ControllerState::PublishSession> staleSession;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->diagnostics.opened) {
            return Status::InvalidArgument("startPublish requires open()");
        }
        if (state_->diagnostics.publishing) {
            return Status::Ok();
        }
        if (state_->publishSession != nullptr) {
            staleSession = std::move(state_->publishSession);
        }
        needsPreview = !state_->diagnostics.previewRunning;
        if (state_->config.rtmpUrl.empty()) {
            return Status::InvalidArgument("startPublish requires a non-empty rtmpUrl");
        }
    }

    finalizePublishSession(std::move(staleSession));

    if (needsPreview) {
        Status previewStatus = startPreview();
        if (!previewStatus.ok()) {
            return previewStatus;
        }
    }

    auto publishSession = std::make_unique<ControllerState::PublishSession>();
    const bool isDesktop = (state_->config.captureMode == "desktop");
    const int videoWidth = isDesktop ? state_->config.capture.width : state_->captureSource.config().width;
    const int videoHeight = isDesktop ? state_->config.capture.height : state_->captureSource.config().height;
    const int videoFps = isDesktop ? state_->config.capture.fps : state_->captureSource.config().fps;

    Status status = publishSession->videoEncoder.configureH264(
        videoWidth, videoHeight, videoFps, state_->config.videoBitrate);
    if (!status.ok()) {
        return status;
    }

    status = publishSession->publisher.configureVideoStream(videoWidth, videoHeight, videoFps);
    if (!status.ok()) {
        return status;
    }

    publishSession->audioEnabled = isDesktop ? false : state_->captureSource.hasAudio();
    if (publishSession->audioEnabled) {
        const media::LiveDeviceCaptureConfig audioConfig = state_->captureSource.config();
        status = publishSession->audioEncoder.configureAac(
            audioConfig.sampleRate, audioConfig.channels, state_->config.audioBitrate);
        if (!status.ok()) {
            return status;
        }

        status = publishSession->publisher.configureAudioStream(audioConfig.sampleRate, audioConfig.channels);
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

    publishSession->senderThread = std::thread(senderThreadMain, state_.get(), publishSession.get());
    publishSession->videoThread = std::thread(videoEncodeThreadMain, state_.get(), publishSession.get());
    if (publishSession->audioEnabled) {
        publishSession->audioThread = std::thread(audioEncodeThreadMain, state_.get(), publishSession.get());
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
    std::unique_ptr<ControllerState::PublishSession> publishSession;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->publishSession == nullptr) {
            return Status::Ok();
        }
        detachPublishSessionLocked(state_.get(), &publishSession);
    }

    finalizePublishSession(std::move(publishSession));
    pushTrace(state_->trace, "publish", "publish session stopped and reset");
    return Status::Ok();
}

}  // namespace live_studio::session
