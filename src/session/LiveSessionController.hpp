#pragma once

#include <string>
#include <vector>

#include "capture/LiveDeviceCaptureSource.hpp"
#include "common/Status.hpp"
#include "common/TraceLog.hpp"
#include "encode/AudioEncoder.hpp"
#include "encode/VideoEncoder.hpp"
#include "mux/FlvMuxer.hpp"
#include "publish/RtmpPublisher.hpp"

namespace live_studio::session {

class LiveSessionController {
public:
    [[nodiscard]] std::string moduleName() const {
        return "LiveSessionController";
    }

    Status openSingleVideoStage(const std::string& rtmpUrl) {
        if (rtmpUrl.empty()) {
            return Status::InvalidArgument("rtmpUrl is empty");
        }
        return Status::NotImplemented(
            "TODO(user): in stage 1, orchestrate live camera/microphone capture beyond the trace entry");
    }

    Status runStage1VideoTrace(const std::string& rtmpUrl,
                               const std::string& captureConfigPath,
                               int frameCountOverride,
                               TraceLog* trace) {
        if (trace == nullptr) {
            return Status::InvalidArgument("trace is null");
        }
        if (rtmpUrl.empty()) {
            return Status::InvalidArgument("rtmpUrl is empty");
        }
        if (captureConfigPath.empty()) {
            return Status::InvalidArgument("captureConfigPath is empty");
        }
        if (frameCountOverride < 0) {
            return Status::InvalidArgument("frameCountOverride must be >= 0");
        }

        capture::LiveDeviceCaptureSource capture;
        encode::VideoEncoder videoEncoder;
        encode::AudioEncoder audioEncoder;
        mux::FlvMuxer muxer;
        publish::RtmpPublisher publisher;

        bool captureStarted = false;
        bool publisherConnected = false;
        const auto fail = [&](Status status) {
            if (captureStarted) {
                capture.stop();
            }
            if (publisherConnected) {
                publisher.disconnect();
            }
            return status;
        };

        trace->push("session", "stage1 trace begins");

        Status status = capture.configureDeviceSource(captureConfigPath);
        if (!status.ok()) {
            return status;
        }

        const int targetVideoFrameCount = frameCountOverride > 0 ? frameCountOverride : capture.config().frameCount;
        trace->push("capture",
                    "live devices configured: label=" + capture.config().sourceLabel + " requested_video=\"" +
                        capture.config().videoDeviceId + "\" requested_audio=\"" + capture.config().audioDeviceId +
                        "\" " + std::to_string(capture.config().width) + "x" +
                        std::to_string(capture.config().height) + " @" + std::to_string(capture.config().fps) +
                        "fps target_video_frames=" +
                        std::to_string(targetVideoFrameCount));

        status = capture.start();
        if (!status.ok()) {
            return status;
        }
        captureStarted = true;
        trace->push("capture",
                    "live device source started: selected_video=\"" + capture.config().resolvedVideoDeviceName +
                        "\" selected_audio=\"" + capture.config().resolvedAudioDeviceName + "\" negotiated_video=" +
                        std::to_string(capture.config().width) + "x" + std::to_string(capture.config().height) +
                        " @" + std::to_string(capture.config().fps) + "fps negotiated_audio=" +
                        std::to_string(capture.config().sampleRate) + "Hz " +
                        std::to_string(capture.config().channels) + "ch");

        status = videoEncoder.configureH264(capture.config().width, capture.config().height, capture.config().fps, 2000000);
        if (!status.ok()) {
            return fail(status);
        }
        trace->push("encode",
                    "FFmpeg encoder configured: H.264 " + std::to_string(capture.config().width) + "x" +
                        std::to_string(capture.config().height) + " bitrate=2000000");

        status = publisher.configureVideoStream(capture.config().width, capture.config().height, capture.config().fps);
        if (!status.ok()) {
            return fail(status);
        }
        trace->push("publish",
                    "publisher video stream configured: " + std::to_string(capture.config().width) + "x" +
                        std::to_string(capture.config().height) + " @" + std::to_string(capture.config().fps) + "fps");

        const bool audioEnabled = capture.hasAudio();
        if (audioEnabled) {
            status = audioEncoder.configureAac(capture.config().sampleRate, capture.config().channels, 128000);
            if (!status.ok()) {
                return fail(status);
            }
            trace->push("encode",
                        "FFmpeg encoder configured: AAC " + std::to_string(capture.config().sampleRate) + "Hz " +
                            std::to_string(capture.config().channels) + "ch bitrate=128000");

            status = publisher.configureAudioStream(capture.config().sampleRate, capture.config().channels);
            if (!status.ok()) {
                return fail(status);
            }
            trace->push("publish",
                        "publisher audio stream configured: " + std::to_string(capture.config().sampleRate) + "Hz " +
                            std::to_string(capture.config().channels) + "ch");
        } else {
            trace->push("capture", "audio capture disabled because audio_device is empty in the live device profile");
        }

        status = publisher.connect(rtmpUrl);
        if (!status.ok()) {
            return fail(status);
        }
        publisherConnected = true;
        trace->push("publish", "publisher entered publishing state for " + publisher.url());

        media::H264ParameterSets parameterSets;
        status = videoEncoder.buildSequenceHeader(&parameterSets);
        if (!status.ok()) {
            return fail(status);
        }
        trace->push("encode",
                    "video sequence header ready: profile=" + parameterSets.profile + " " + parameterSets.spsLabel +
                        " / " + parameterSets.ppsLabel);

        media::FlvVideoTagPlan videoSequenceHeaderTag;
        status = muxer.buildVideoSequenceHeader(parameterSets, &videoSequenceHeaderTag);
        if (!status.ok()) {
            return fail(status);
        }
        trace->push("mux",
                    "built FLV video sequence header: " + videoSequenceHeaderTag.payloadSummary + " payload=" +
                        std::to_string(videoSequenceHeaderTag.payloadBytes.size()) + "B tag=" +
                        std::to_string(videoSequenceHeaderTag.tagBytes.size()) + "B");

        media::RtmpMessagePlan videoSequenceHeaderMessage;
        status = publisher.mapVideoTagToMessage(videoSequenceHeaderTag, &videoSequenceHeaderMessage);
        if (!status.ok()) {
            return fail(status);
        }
        status = publisher.publishVideoTag(videoSequenceHeaderMessage);
        if (!status.ok()) {
            return fail(status);
        }
        trace->push("publish",
                    "planned RTMP video sequence header message: " + videoSequenceHeaderMessage.payloadSummary +
                        " payload=" + std::to_string(videoSequenceHeaderMessage.payloadBytes.size()) + "B");

        if (audioEnabled) {
            media::AacDecoderConfig decoderConfig;
            status = audioEncoder.buildSequenceHeader(&decoderConfig);
            if (!status.ok()) {
                return fail(status);
            }
            trace->push("encode", "audio sequence header ready: " + decoderConfig.configLabel);

            media::FlvAudioTagPlan audioSequenceHeaderTag;
            status = muxer.buildAudioSequenceHeader(decoderConfig, &audioSequenceHeaderTag);
            if (!status.ok()) {
                return fail(status);
            }
            trace->push("mux",
                        "built FLV audio sequence header: " + audioSequenceHeaderTag.payloadSummary + " payload=" +
                            std::to_string(audioSequenceHeaderTag.payloadBytes.size()) + "B tag=" +
                            std::to_string(audioSequenceHeaderTag.tagBytes.size()) + "B");

            media::RtmpMessagePlan audioSequenceHeaderMessage;
            status = publisher.mapAudioTagToMessage(audioSequenceHeaderTag, &audioSequenceHeaderMessage);
            if (!status.ok()) {
                return fail(status);
            }
            status = publisher.publishAudioTag(audioSequenceHeaderMessage);
            if (!status.ok()) {
                return fail(status);
            }
            trace->push("publish",
                        "planned RTMP audio sequence header message: " + audioSequenceHeaderMessage.payloadSummary +
                            " payload=" + std::to_string(audioSequenceHeaderMessage.payloadBytes.size()) + "B");
        }

        int publishedVideoFrames = 0;
        while (publishedVideoFrames < targetVideoFrameCount) {
            capture::CaptureEvent event;
            status = capture.readNext(&event);
            if (!status.ok()) {
                return fail(status);
            }

            if (event.kind == capture::CaptureEventKind::video) {
                trace->push("capture",
                            "video frame#" + std::to_string(event.videoFrame.frameIndex) + " pts=" +
                                std::to_string(event.videoFrame.ptsMs) + "ms source=" + event.videoFrame.sourceLabel +
                                " payload=" + std::to_string(event.videoFrame.pixelPayload.size()) + "B");

                media::EncodedVideoAccessUnit accessUnit;
                status = videoEncoder.encodeFrame(event.videoFrame, &accessUnit);
                if (!status.ok()) {
                    return fail(status);
                }
                trace->push("encode",
                            "video frame#" + std::to_string(accessUnit.frameIndex) +
                                (accessUnit.keyFrame ? " keyframe " : " interframe ") + "pts=" +
                                std::to_string(accessUnit.ptsMs) + "ms " + accessUnit.nalSummary);

                media::FlvVideoTagPlan tag;
                status = muxer.buildVideoNaluTag(accessUnit, &tag);
                if (!status.ok()) {
                    return fail(status);
                }
                trace->push("mux",
                            "video frame#" + std::to_string(tag.frameIndex) + " tag ts=" +
                                std::to_string(tag.timestampMs) + "ms " + tag.payloadSummary + " payload=" +
                                std::to_string(tag.payloadBytes.size()) + "B tag=" +
                                std::to_string(tag.tagBytes.size()) + "B");

                media::RtmpMessagePlan message;
                status = publisher.mapVideoTagToMessage(tag, &message);
                if (!status.ok()) {
                    return fail(status);
                }
                status = publisher.publishVideoTag(message);
                if (!status.ok()) {
                    return fail(status);
                }
                trace->push("publish",
                            "video frame#" + std::to_string(message.frameIndex) + " message ts=" +
                                std::to_string(message.timestampMs) + "ms " + message.payloadSummary + " payload=" +
                                std::to_string(message.payloadBytes.size()) + "B");

                ++publishedVideoFrames;
                continue;
            }

            trace->push("capture",
                        "audio frame#" + std::to_string(event.audioFrame.frameIndex) + " pts=" +
                            std::to_string(event.audioFrame.ptsMs) + "ms samples=" +
                            std::to_string(event.audioFrame.samplesPerChannel) + " payload=" +
                            std::to_string(event.audioFrame.samplePayload.size()) + "B");

            std::vector<media::EncodedAudioAccessUnit> audioUnits;
            status = audioEncoder.encodeFrame(event.audioFrame, &audioUnits);
            if (!status.ok()) {
                return fail(status);
            }

            for (const media::EncodedAudioAccessUnit& unit : audioUnits) {
                trace->push("encode",
                            "audio frame#" + std::to_string(unit.frameIndex) + " pts=" +
                                std::to_string(unit.ptsMs) + "ms " + unit.payloadSummary);

                media::FlvAudioTagPlan tag;
                status = muxer.buildAudioRawTag(unit, capture.config().channels, &tag);
                if (!status.ok()) {
                    return fail(status);
                }
                trace->push("mux",
                            "audio frame#" + std::to_string(tag.frameIndex) + " tag ts=" +
                                std::to_string(tag.timestampMs) + "ms " + tag.payloadSummary + " payload=" +
                                std::to_string(tag.payloadBytes.size()) + "B tag=" +
                                std::to_string(tag.tagBytes.size()) + "B");

                media::RtmpMessagePlan message;
                status = publisher.mapAudioTagToMessage(tag, &message);
                if (!status.ok()) {
                    return fail(status);
                }
                status = publisher.publishAudioTag(message);
                if (!status.ok()) {
                    return fail(status);
                }
                trace->push("publish",
                            "audio frame#" + std::to_string(message.frameIndex) + " message ts=" +
                                std::to_string(message.timestampMs) + "ms " + message.payloadSummary + " payload=" +
                                std::to_string(message.payloadBytes.size()) + "B");
            }
        }

        if (audioEnabled) {
            std::vector<media::EncodedAudioAccessUnit> pendingUnits;
            status = audioEncoder.flush(&pendingUnits);
            if (!status.ok()) {
                return fail(status);
            }

            for (const media::EncodedAudioAccessUnit& unit : pendingUnits) {
                media::FlvAudioTagPlan tag;
                status = muxer.buildAudioRawTag(unit, capture.config().channels, &tag);
                if (!status.ok()) {
                    return fail(status);
                }

                media::RtmpMessagePlan message;
                status = publisher.mapAudioTagToMessage(tag, &message);
                if (!status.ok()) {
                    return fail(status);
                }
                status = publisher.publishAudioTag(message);
                if (!status.ok()) {
                    return fail(status);
                }
                trace->push("publish",
                            "flushed audio message ts=" + std::to_string(message.timestampMs) + "ms " +
                                message.payloadSummary + " payload=" +
                                std::to_string(message.payloadBytes.size()) + "B");
            }
        }

        capture.stop();
        publisher.disconnect();
        trace->push("session", "stage1 trace finished cleanly");
        return Status::Ok();
    }

    void close() noexcept {}
};

}  // namespace live_studio::session
