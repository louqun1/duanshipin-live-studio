#include "session/LiveSessionControllerInternal.hpp"

namespace live_studio::session {

std::string maybeRecordVideoPerfInput(PublishSession* session,
                                      std::int64_t sampleNs,
                                      std::size_t queueDepthAfterPush) {
    if (session == nullptr || sampleNs <= 0) {
        return {};
    }

    std::lock_guard<std::mutex> lock(session->videoPerfStats.mutex);
    VideoPerfWindow& window = session->videoPerfStats.window;
    if (window.windowStartNs <= 0) {
        window.reset(sampleNs);
    }

    ++window.inputFrames;
    if (!window.hasQueueSample) {
        window.hasQueueSample = true;
        window.queueDepthFirst = queueDepthAfterPush;
    }
    window.queueDepthLast = queueDepthAfterPush;

    const std::int64_t elapsedNs = sampleNs - window.windowStartNs;
    if (elapsedNs < 1000000000LL) {
        return {};
    }

    const double elapsedMs = nsToMs(elapsedNs);
    const double elapsedSeconds = elapsedMs / 1000.0;
    const double inputFps = elapsedSeconds > 0.0 ? static_cast<double>(window.inputFrames) / elapsedSeconds : 0.0;
    const double encodedFps =
        elapsedSeconds > 0.0 ? static_cast<double>(window.encodedFrames) / elapsedSeconds : 0.0;
    const double avgPrepareMs =
        window.encodedFrames > 0 ? window.prepareDurationSumMs / static_cast<double>(window.encodedFrames) : 0.0;
    const double keyAvgMs =
        window.keyFrames > 0 ? window.keyEncodeDurationSumMs / static_cast<double>(window.keyFrames) : 0.0;
    const double pAvgMs =
        window.pFrames > 0 ? window.pEncodeDurationSumMs / static_cast<double>(window.pFrames) : 0.0;
    const double avgFrameCopyMs =
        window.encodedFrames > 0 ? window.frameCopyDurationSumMs / static_cast<double>(window.encodedFrames) : 0.0;
    const double avgCodecSendFrameMs = window.encodedFrames > 0
                                           ? window.codecSendFrameDurationSumMs / static_cast<double>(window.encodedFrames)
                                           : 0.0;
    const double avgCodecReceivePacketMs =
        window.encodedFrames > 0 ? window.codecReceivePacketDurationSumMs / static_cast<double>(window.encodedFrames)
                                 : 0.0;
    const double avgPacketFinalizeMs = window.encodedFrames > 0
                                           ? window.packetFinalizeDurationSumMs / static_cast<double>(window.encodedFrames)
                                           : 0.0;
    const long long queueDelta = static_cast<long long>(window.queueDepthLast) -
                                 static_cast<long long>(window.queueDepthFirst);
    const double queueGrowthPerSecond =
        elapsedSeconds > 0.0 ? static_cast<double>(queueDelta) / elapsedSeconds : 0.0;

    std::string trace =
        "window=" + formatDouble(elapsedSeconds) + "s inputFps=" + formatDouble(inputFps) + " encodeFps=" +
        formatDouble(encodedFps) + " avgPrepare=" + formatDouble(avgPrepareMs) + "ms avgFrameCopy=" +
        formatDouble(avgFrameCopyMs) + "ms avgCodecSend=" + formatDouble(avgCodecSendFrameMs) +
        "ms avgCodecReceive=" + formatDouble(avgCodecReceivePacketMs) + "ms avgPacketFinalize=" +
        formatDouble(avgPacketFinalizeMs) + "ms keyFrames=" + std::to_string(window.keyFrames) +
        " avgKeyEncode=" + formatDouble(keyAvgMs) + "ms maxKeyEncode=" +
        formatDouble(window.keyEncodeDurationMaxMs) + "ms pFrames=" + std::to_string(window.pFrames) +
        " avgPEncode=" + formatDouble(pAvgMs) + "ms maxPEncode=" +
        formatDouble(window.pEncodeDurationMaxMs) + "ms queueDepth=" +
        std::to_string(static_cast<unsigned long long>(window.queueDepthFirst)) + "->" +
        std::to_string(static_cast<unsigned long long>(window.queueDepthLast)) + " queueGrowth=" +
        formatSignedDouble(queueGrowthPerSecond) + "/s";

    window.reset(sampleNs);
    window.hasQueueSample = true;
    window.queueDepthFirst = queueDepthAfterPush;
    window.queueDepthLast = queueDepthAfterPush;
    return trace;
}

std::string maybeRecordVideoPerfEncoded(PublishSession* session,
                                        std::int64_t sampleNs,
                                        bool keyFrame,
                                        double prepareDurationMs,
                                        double encodeDurationMs,
                                        const encode::VideoEncodeTrace& encodeTrace) {
    if (session == nullptr || sampleNs <= 0) {
        return {};
    }

    std::lock_guard<std::mutex> lock(session->videoPerfStats.mutex);
    VideoPerfWindow& window = session->videoPerfStats.window;
    if (window.windowStartNs <= 0) {
        window.reset(sampleNs);
    }

    ++window.encodedFrames;
    window.prepareDurationSumMs += prepareDurationMs;
    window.frameCopyDurationSumMs += encodeTrace.frameCopyDurationMs;
    window.codecSendFrameDurationSumMs += encodeTrace.sendFrameDurationMs;
    window.codecReceivePacketDurationSumMs += encodeTrace.receivePacketDurationMs;
    window.packetFinalizeDurationSumMs += encodeTrace.packetFinalizeDurationMs;
    if (keyFrame) {
        ++window.keyFrames;
        window.keyEncodeDurationSumMs += encodeDurationMs;
        window.keyEncodeDurationMaxMs = std::max(window.keyEncodeDurationMaxMs, encodeDurationMs);
    } else {
        ++window.pFrames;
        window.pEncodeDurationSumMs += encodeDurationMs;
        window.pEncodeDurationMaxMs = std::max(window.pEncodeDurationMaxMs, encodeDurationMs);
    }

    const std::int64_t elapsedNs = sampleNs - window.windowStartNs;
    if (elapsedNs < 1000000000LL) {
        return {};
    }

    const double elapsedMs = nsToMs(elapsedNs);
    const double elapsedSeconds = elapsedMs / 1000.0;
    const double inputFps = elapsedSeconds > 0.0 ? static_cast<double>(window.inputFrames) / elapsedSeconds : 0.0;
    const double encodedFps =
        elapsedSeconds > 0.0 ? static_cast<double>(window.encodedFrames) / elapsedSeconds : 0.0;
    const double avgPrepareMs =
        window.encodedFrames > 0 ? window.prepareDurationSumMs / static_cast<double>(window.encodedFrames) : 0.0;
    const double keyAvgMs =
        window.keyFrames > 0 ? window.keyEncodeDurationSumMs / static_cast<double>(window.keyFrames) : 0.0;
    const double pAvgMs =
        window.pFrames > 0 ? window.pEncodeDurationSumMs / static_cast<double>(window.pFrames) : 0.0;
    const double avgFrameCopyMs =
        window.encodedFrames > 0 ? window.frameCopyDurationSumMs / static_cast<double>(window.encodedFrames) : 0.0;
    const double avgCodecSendFrameMs = window.encodedFrames > 0
                                           ? window.codecSendFrameDurationSumMs / static_cast<double>(window.encodedFrames)
                                           : 0.0;
    const double avgCodecReceivePacketMs =
        window.encodedFrames > 0 ? window.codecReceivePacketDurationSumMs / static_cast<double>(window.encodedFrames)
                                 : 0.0;
    const double avgPacketFinalizeMs = window.encodedFrames > 0
                                           ? window.packetFinalizeDurationSumMs / static_cast<double>(window.encodedFrames)
                                           : 0.0;
    const long long queueDelta = static_cast<long long>(window.queueDepthLast) -
                                 static_cast<long long>(window.queueDepthFirst);
    const double queueGrowthPerSecond =
        elapsedSeconds > 0.0 ? static_cast<double>(queueDelta) / elapsedSeconds : 0.0;
    const std::string queueDepth =
        window.hasQueueSample
            ? std::to_string(static_cast<unsigned long long>(window.queueDepthFirst)) + "->" +
                  std::to_string(static_cast<unsigned long long>(window.queueDepthLast))
            : "n/a";

    std::string trace =
        "window=" + formatDouble(elapsedSeconds) + "s inputFps=" + formatDouble(inputFps) + " encodeFps=" +
        formatDouble(encodedFps) + " avgPrepare=" + formatDouble(avgPrepareMs) + "ms avgFrameCopy=" +
        formatDouble(avgFrameCopyMs) + "ms avgCodecSend=" + formatDouble(avgCodecSendFrameMs) +
        "ms avgCodecReceive=" + formatDouble(avgCodecReceivePacketMs) + "ms avgPacketFinalize=" +
        formatDouble(avgPacketFinalizeMs) + "ms keyFrames=" + std::to_string(window.keyFrames) +
        " avgKeyEncode=" + formatDouble(keyAvgMs) + "ms maxKeyEncode=" +
        formatDouble(window.keyEncodeDurationMaxMs) + "ms pFrames=" + std::to_string(window.pFrames) +
        " avgPEncode=" + formatDouble(pAvgMs) + "ms maxPEncode=" +
        formatDouble(window.pEncodeDurationMaxMs) + "ms queueDepth=" + queueDepth +
        " queueGrowth=" + formatSignedDouble(queueGrowthPerSecond) + "/s";

    window.reset(sampleNs);
    return trace;
}

Status seedPublishEpoch(PublishSession* session, std::int64_t captureWallClockNs) {
    if (session == nullptr) {
        return Status::InvalidArgument("publish session is null");
    }
    if (captureWallClockNs <= 0) {
        return Status::InvalidArgument("captureWallClockNs must be positive");
    }

    std::lock_guard<std::mutex> lock(session->timingMutex);
    if (!session->epochInitialized) {
        session->epochInitialized = true;
        session->epochWallClockNs = captureWallClockNs;
    }
    return Status::Ok();
}

Status prepareVideoFrameForPublish(PublishSession* session,
                                   const media::VideoFrame& frame,
                                   media::VideoFrame* outFrame) {
    if (session == nullptr) {
        return Status::InvalidArgument("publish session is null");
    }
    if (outFrame == nullptr) {
        return Status::InvalidArgument("outFrame is null");
    }
    if (frame.captureWallClockNs <= 0) {
        return Status::InvalidArgument("video frame captureWallClockNs must be positive");
    }

    media::VideoFrame publishFrame = frame;
    {
        std::lock_guard<std::mutex> lock(session->timingMutex);
        if (!session->epochInitialized) {
            session->epochInitialized = true;
            session->epochWallClockNs = frame.captureWallClockNs;
        }
        publishFrame.ptsMs = monotonicTimestampMs(frame.captureWallClockNs,
                                                  session->epochWallClockNs,
                                                  &session->lastVideoTimestampMs);
    }

    *outFrame = std::move(publishFrame);
    return Status::Ok();
}

Status normalizeAudioUnitsForPublish(PublishSession* session,
                                     const media::AudioFrame& sourceFrame,
                                     std::vector<media::EncodedAudioAccessUnit>* outUnits) {
    if (session == nullptr) {
        return Status::InvalidArgument("publish session is null");
    }
    if (outUnits == nullptr) {
        return Status::InvalidArgument("outUnits is null");
    }
    if (sourceFrame.captureWallClockNs <= 0) {
        return Status::InvalidArgument("audio frame captureWallClockNs must be positive");
    }

    std::lock_guard<std::mutex> lock(session->timingMutex);
    if (!session->epochInitialized) {
        session->epochInitialized = true;
        session->epochWallClockNs = sourceFrame.captureWallClockNs;
    }
    if (!session->audioCaptureBaseInitialized) {
        session->audioCaptureBaseInitialized = true;
        session->audioCaptureBaseWallClockNs = sourceFrame.captureWallClockNs;
    }

    const std::int64_t audioBaseOffsetMs =
        relativeTimestampMs(session->audioCaptureBaseWallClockNs, session->epochWallClockNs);

    for (media::EncodedAudioAccessUnit& unit : *outUnits) {
        if (!session->audioEncodedBaseInitialized) {
            session->audioEncodedBaseInitialized = true;
            session->audioEncodedBasePtsMs = unit.ptsMs;
            session->audioEncodedBaseDtsMs = unit.dtsMs;
        }

        std::int64_t normalizedPtsMs =
            audioBaseOffsetMs + std::max<std::int64_t>(0, unit.ptsMs - session->audioEncodedBasePtsMs);
        std::int64_t normalizedDtsMs =
            audioBaseOffsetMs + std::max<std::int64_t>(0, unit.dtsMs - session->audioEncodedBaseDtsMs);
        if (normalizedDtsMs < session->lastAudioTimestampMs) {
            normalizedDtsMs = session->lastAudioTimestampMs;
        }
        if (normalizedPtsMs < normalizedDtsMs) {
            normalizedPtsMs = normalizedDtsMs;
        }

        session->lastAudioTimestampMs = normalizedDtsMs;
        unit.ptsMs = normalizedPtsMs;
        unit.dtsMs = normalizedDtsMs;
    }

    return Status::Ok();
}

void signalPublishFailure(LiveSessionController::ControllerState* state,
                          PublishSession* session,
                          const Status& status,
                          const std::string& detailPrefix) {
    if (state == nullptr || session == nullptr) {
        return;
    }

    bool shouldTrace = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->publishSession.get() != session) {
            return;
        }
        shouldTrace = state->diagnostics.publishing || state->diagnostics.lastError != status.message;
        state->diagnostics.lastError = status.message;
        state->diagnostics.publishing = false;
    }

    session->requestStop();
    if (shouldTrace) {
        pushTrace(state->trace, "publish", detailPrefix + status.message);
    }
}

void senderThreadMain(LiveSessionController::ControllerState* state, PublishSession* session) {
    if (state == nullptr || session == nullptr) {
        return;
    }

    while (!session->stopRequested.load()) {
        SenderWorkItem item;
        if (!session->senderQueue.pop(&item)) {
            break;
        }

        std::string latencyTrace;
        const std::int64_t sendStartNs = steadyClockNowNs();
        const double senderQueueWaitMs =
            item.senderEnqueuedAtNs > 0 ? nsToMs(sendStartNs - item.senderEnqueuedAtNs) : 0.0;
        if (item.kind == SenderWorkKind::video) {
            media::FlvVideoTagPlan tag;
            Status status = session->muxer.buildVideoNaluTag(item.videoUnit, &tag);
            if (!status.ok()) {
                signalPublishFailure(state, session, status, "video publish stopped: ");
                return;
            }

            media::RtmpMessagePlan message;
            status = session->publisher.mapVideoTagToMessage(tag, &message);
            if (!status.ok()) {
                signalPublishFailure(state, session, status, "video publish stopped: ");
                return;
            }

            status = session->publisher.publishVideoTag(message);
            if (!status.ok()) {
                signalPublishFailure(state, session, status, "video publish stopped: ");
                return;
            }

            const std::int64_t sendDoneNs = steadyClockNowNs();
            const double sendDurationMs = nsToMs(sendDoneNs - sendStartNs);
            const double captureToSendMs =
                item.captureWallClockNs > 0 ? nsToMs(sendDoneNs - item.captureWallClockNs) : 0.0;

            if ((item.encodedSequence % kVideoTraceInterval) == 0) {
                pushTrace(state->trace,
                          "publish-video-send",
                          "captureFrame#" + std::to_string(item.captureFrameIndex) + " encodeSeq#" +
                              std::to_string(item.encodedSequence) + " capturePts=" +
                              std::to_string(item.capturePtsMs) + "ms outPts=" +
                              std::to_string(item.videoUnit.ptsMs) + "ms outDts=" +
                              std::to_string(item.videoUnit.dtsMs) + "ms captureQ=" +
                              std::to_string(item.pendingCaptureEvents) + " videoQ(at-push)=" +
                              formatQueueDepth(item.stageInputQueueDepthAfterPush, kVideoInputQueueCapacity) +
                              " senderQ(before-push)=" +
                              formatQueueDepth(item.senderQueueDepthSnapshot, kSenderQueueCapacity) +
                              " senderQ(after-push)=" +
                              formatQueueDepth(item.senderQueueDepthAfterPush, kSenderQueueCapacity) +
                              " qWait=" + formatDouble(item.stageQueueWaitMs) + "ms prepare=" +
                              formatDouble(item.prepareDurationMs) + "ms encode=" +
                              formatDouble(item.encodeDurationMs) + "ms frameCopy=" +
                              formatDouble(item.frameCopyDurationMs) + "ms codecSend=" +
                              formatDouble(item.codecSendFrameDurationMs) + "ms codecReceive=" +
                              formatDouble(item.codecReceivePacketDurationMs) + "ms packetFinalize=" +
                              formatDouble(item.packetFinalizeDurationMs) + "ms senderWait=" +
                              formatDouble(senderQueueWaitMs) + "ms send=" + formatDouble(sendDurationMs) +
                              "ms total=" + formatDouble(captureToSendMs) + "ms videoDrops=" +
                              std::to_string(item.droppedFrames) + " summary=" +
                              message.payloadSummary);
            }

            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->publishSession.get() == session) {
                    ++state->diagnostics.publishedVideoFrames;
                    state->diagnostics.lastPublishedSummary = message.payloadSummary;
                    if (!state->diagnostics.firstMediaPacketSent && item.captureWallClockNs > 0) {
                        state->diagnostics.firstMediaPacketSent = true;
                        state->diagnostics.firstMediaLatencyMs = nsToMs(sendDoneNs - item.captureWallClockNs);
                        latencyTrace = "first-media-packet: media=video latency=" +
                                       std::to_string(state->diagnostics.firstMediaLatencyMs) + "ms";
                    }
                }
            }
        } else {
            media::FlvAudioTagPlan tag;
            Status status =
                session->muxer.buildAudioRawTag(item.audioUnit, state->captureSource.config().channels, &tag);
            if (!status.ok()) {
                signalPublishFailure(state, session, status, "audio publish stopped: ");
                return;
            }

            media::RtmpMessagePlan message;
            status = session->publisher.mapAudioTagToMessage(tag, &message);
            if (!status.ok()) {
                signalPublishFailure(state, session, status, "audio publish stopped: ");
                return;
            }

            status = session->publisher.publishAudioTag(message);
            if (!status.ok()) {
                signalPublishFailure(state, session, status, "audio publish stopped: ");
                return;
            }

            const std::int64_t sendDoneNs = steadyClockNowNs();
            const double sendDurationMs = nsToMs(sendDoneNs - sendStartNs);
            const double captureToSendMs =
                item.captureWallClockNs > 0 ? nsToMs(sendDoneNs - item.captureWallClockNs) : 0.0;

            if ((item.encodedSequence % kAudioTraceInterval) == 0) {
                pushTrace(state->trace,
                          "publish-audio-send",
                          "captureFrame#" + std::to_string(item.captureFrameIndex) + " encodeSeq#" +
                              std::to_string(item.encodedSequence) + " capturePts=" +
                              std::to_string(item.capturePtsMs) + "ms outPts=" +
                              std::to_string(item.audioUnit.ptsMs) + "ms outDts=" +
                              std::to_string(item.audioUnit.dtsMs) + "ms captureQ=" +
                              std::to_string(item.pendingCaptureEvents) + " audioQ(at-push)=" +
                              formatQueueDepth(item.stageInputQueueDepthAfterPush, kAudioInputQueueCapacity) +
                              " senderQ(before-push)=" +
                              formatQueueDepth(item.senderQueueDepthSnapshot, kSenderQueueCapacity) +
                              " senderQ(after-push)=" +
                              formatQueueDepth(item.senderQueueDepthAfterPush, kSenderQueueCapacity) +
                              " qWait=" + formatDouble(item.stageQueueWaitMs) + "ms encode=" +
                              formatDouble(item.encodeDurationMs) + "ms senderWait=" +
                              formatDouble(senderQueueWaitMs) + "ms send=" + formatDouble(sendDurationMs) +
                              "ms total=" + formatDouble(captureToSendMs) + "ms audioDrops=" +
                              std::to_string(item.droppedFrames) + " summary=" +
                              message.payloadSummary);
            }

            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->publishSession.get() == session) {
                    ++state->diagnostics.publishedAudioUnits;
                    state->diagnostics.lastPublishedSummary = message.payloadSummary;
                    if (!state->diagnostics.firstMediaPacketSent && item.captureWallClockNs > 0) {
                        state->diagnostics.firstMediaPacketSent = true;
                        state->diagnostics.firstMediaLatencyMs = nsToMs(sendDoneNs - item.captureWallClockNs);
                        latencyTrace = "first-media-packet: media=audio latency=" +
                                       std::to_string(state->diagnostics.firstMediaLatencyMs) + "ms";
                    }
                }
            }
        }

        if (!latencyTrace.empty()) {
            pushTrace(state->trace, "latency", latencyTrace);
        }
    }
}

void videoEncodeThreadMain(LiveSessionController::ControllerState* state, PublishSession* session) {
    if (state == nullptr || session == nullptr) {
        return;
    }

    while (!session->stopRequested.load()) {
        VideoEncodeInput input;
        if (!session->videoInputQueue.pop(&input)) {
            break;
        }

        const std::int64_t stageStartNs = steadyClockNowNs();
        const double stageQueueWaitMs =
            input.enqueuedAtNs > 0 ? nsToMs(stageStartNs - input.enqueuedAtNs) : 0.0;

        media::VideoFrame publishFrame;
        const std::int64_t prepareStartNs = stageStartNs;
        Status status = prepareVideoFrameForPublish(session, input.frame, &publishFrame);
        if (!status.ok()) {
            signalPublishFailure(state, session, status, "video publish stopped: ");
            return;
        }
        const std::int64_t prepareDoneNs = steadyClockNowNs();
        const double prepareDurationMs = nsToMs(prepareDoneNs - prepareStartNs);

        media::EncodedVideoAccessUnit accessUnit;
        encode::VideoEncodeTrace encodeTrace;
        const std::int64_t encodeCallStartNs = prepareDoneNs;
        status = session->videoEncoder.encodeFrame(publishFrame, &accessUnit, &encodeTrace);
        if (!status.ok()) {
            signalPublishFailure(state, session, status, "video publish stopped: ");
            return;
        }

        const std::int64_t encodeDoneNs = steadyClockNowNs();
        const double encodeDurationMs = nsToMs(encodeDoneNs - encodeCallStartNs);
        const double captureToEncodeDoneMs =
            input.frame.captureWallClockNs > 0 ? nsToMs(encodeDoneNs - input.frame.captureWallClockNs) : 0.0;
        const std::int64_t encodedPtsMs = accessUnit.ptsMs;
        const std::int64_t encodedDtsMs = accessUnit.dtsMs;
        const std::string encodedNalSummary = accessUnit.nalSummary;
        const bool keyFrame = accessUnit.keyFrame;

        SenderWorkItem item;
        item.kind = SenderWorkKind::video;
        item.captureFrameIndex = input.frame.frameIndex;
        item.capturePtsMs = input.frame.ptsMs;
        item.captureWallClockNs = input.frame.captureWallClockNs;
        item.pendingCaptureEvents = input.pendingCaptureEvents;
        item.encodedSequence = session->nextVideoEncodedSequence.fetch_add(1);
        item.droppedFrames = input.droppedVideoFrames;
        item.stageInputQueueDepthAfterPush = input.videoQueueDepthAfterPush;
        item.senderQueueDepthSnapshot = input.senderQueueDepthSnapshot;
        item.stageQueueWaitMs = stageQueueWaitMs;
        item.prepareDurationMs = prepareDurationMs;
        item.encodeDurationMs = encodeDurationMs;
        item.frameCopyDurationMs = encodeTrace.frameCopyDurationMs;
        item.codecSendFrameDurationMs = encodeTrace.sendFrameDurationMs;
        item.codecReceivePacketDurationMs = encodeTrace.receivePacketDurationMs;
        item.packetFinalizeDurationMs = encodeTrace.packetFinalizeDurationMs;
        item.captureToEncodeDoneMs = captureToEncodeDoneMs;
        item.senderEnqueuedAtNs = encodeDoneNs;
        item.senderQueueDepthAfterPush = input.senderQueueDepthSnapshot + 1;
        item.videoUnit = std::move(accessUnit);

        std::size_t senderQueueDepthAfterPush = 0;
        if (!session->senderQueue.pushWait(std::move(item), &senderQueueDepthAfterPush)) {
            break;
        }

        const std::string videoPerfTrace =
            maybeRecordVideoPerfEncoded(session, encodeDoneNs, keyFrame, prepareDurationMs, encodeDurationMs, encodeTrace);

        if ((item.encodedSequence % kVideoTraceInterval) == 0) {
            pushTrace(state->trace,
                      "publish-video-encode",
                      "captureFrame#" + std::to_string(input.frame.frameIndex) + " encodeSeq#" +
                          std::to_string(item.encodedSequence) + " capturePts=" +
                          std::to_string(input.frame.ptsMs) + "ms outPts=" +
                          std::to_string(encodedPtsMs) + "ms outDts=" +
                          std::to_string(encodedDtsMs) + "ms captureQ=" +
                          std::to_string(input.pendingCaptureEvents) + " videoQ(at-push)=" +
                          formatQueueDepth(input.videoQueueDepthAfterPush, kVideoInputQueueCapacity) +
                          " senderQ(before-push)=" +
                          formatQueueDepth(input.senderQueueDepthSnapshot, kSenderQueueCapacity) +
                          " senderQ(after-push)=" +
                          formatQueueDepth(senderQueueDepthAfterPush, kSenderQueueCapacity) +
                          " qWait=" + formatDouble(stageQueueWaitMs) + "ms prepare=" +
                          formatDouble(prepareDurationMs) + "ms encode=" +
                          formatDouble(encodeDurationMs) + "ms frameCopy=" +
                          formatDouble(encodeTrace.frameCopyDurationMs) + "ms codecSend=" +
                          formatDouble(encodeTrace.sendFrameDurationMs) + "ms codecReceive=" +
                          formatDouble(encodeTrace.receivePacketDurationMs) + "ms packetFinalize=" +
                          formatDouble(encodeTrace.packetFinalizeDurationMs) + "ms captureToEncode=" +
                          formatDouble(captureToEncodeDoneMs) + "ms videoDrops=" +
                          std::to_string(input.droppedVideoFrames) + " summary=" + encodedNalSummary);
        }

        if (!videoPerfTrace.empty()) {
            pushTrace(state->trace, "publish-video-stats", videoPerfTrace);
        }
    }
}

void audioEncodeThreadMain(LiveSessionController::ControllerState* state, PublishSession* session) {
    if (state == nullptr || session == nullptr) {
        return;
    }

    while (!session->stopRequested.load()) {
        AudioEncodeInput input;
        if (!session->audioInputQueue.pop(&input)) {
            break;
        }

        const std::int64_t encodeStartNs = steadyClockNowNs();
        const double stageQueueWaitMs =
            input.enqueuedAtNs > 0 ? nsToMs(encodeStartNs - input.enqueuedAtNs) : 0.0;

        std::vector<media::EncodedAudioAccessUnit> units;
        Status status = session->audioEncoder.encodeFrame(input.frame, &units);
        if (!status.ok()) {
            signalPublishFailure(state, session, status, "audio publish stopped: ");
            return;
        }

        status = normalizeAudioUnitsForPublish(session, input.frame, &units);
        if (!status.ok()) {
            signalPublishFailure(state, session, status, "audio publish stopped: ");
            return;
        }

        const std::int64_t encodeDoneNs = steadyClockNowNs();
        const double encodeDurationMs = nsToMs(encodeDoneNs - encodeStartNs);
        const double captureToEncodeDoneMs =
            input.frame.captureWallClockNs > 0 ? nsToMs(encodeDoneNs - input.frame.captureWallClockNs) : 0.0;

        std::size_t senderQueueDepthAfterPush = 0;
        int lastEncodedSequence = -1;
        for (media::EncodedAudioAccessUnit& unit : units) {
            SenderWorkItem item;
            item.kind = SenderWorkKind::audio;
            item.captureFrameIndex = input.frame.frameIndex;
            item.capturePtsMs = input.frame.ptsMs;
            item.captureWallClockNs = input.frame.captureWallClockNs;
            item.pendingCaptureEvents = input.pendingCaptureEvents;
            item.encodedSequence = session->nextAudioEncodedSequence.fetch_add(1);
            item.droppedFrames = input.droppedAudioFrames;
            item.stageInputQueueDepthAfterPush = input.audioQueueDepthAfterPush;
            item.senderQueueDepthSnapshot = input.senderQueueDepthSnapshot;
            item.stageQueueWaitMs = stageQueueWaitMs;
            item.encodeDurationMs = encodeDurationMs;
            item.captureToEncodeDoneMs = captureToEncodeDoneMs;
            item.senderEnqueuedAtNs = encodeDoneNs;
            item.senderQueueDepthAfterPush = input.senderQueueDepthSnapshot + 1;
            item.audioUnit = std::move(unit);
            lastEncodedSequence = item.encodedSequence;
            if (!session->senderQueue.pushWait(std::move(item), &senderQueueDepthAfterPush)) {
                return;
            }
        }

        if (!units.empty() && (lastEncodedSequence % kAudioTraceInterval) == 0) {
            pushTrace(state->trace,
                      "publish-audio-encode",
                      "captureFrame#" + std::to_string(input.frame.frameIndex) + " encodeSeq#" +
                          std::to_string(lastEncodedSequence) + " capturePts=" +
                          std::to_string(input.frame.ptsMs) + "ms units=" +
                          std::to_string(static_cast<unsigned long long>(units.size())) + " captureQ=" +
                          std::to_string(input.pendingCaptureEvents) + " audioQ(at-push)=" +
                          formatQueueDepth(input.audioQueueDepthAfterPush, kAudioInputQueueCapacity) +
                          " senderQ(before-push)=" +
                          formatQueueDepth(input.senderQueueDepthSnapshot, kSenderQueueCapacity) +
                          " senderQ(after-push)=" +
                          formatQueueDepth(senderQueueDepthAfterPush, kSenderQueueCapacity) +
                          " qWait=" + formatDouble(stageQueueWaitMs) + "ms encode=" +
                          formatDouble(encodeDurationMs) + "ms captureToEncode=" +
                          formatDouble(captureToEncodeDoneMs) + "ms audioDrops=" +
                          std::to_string(input.droppedAudioFrames));
        }
    }
}

}  // namespace live_studio::session
