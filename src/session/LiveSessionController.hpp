#pragma once

#include <string>

#include "capture/VideoCaptureSource.hpp"
#include "common/TraceLog.hpp"
#include "common/Status.hpp"
#include "encode/VideoEncoder.hpp"
#include "mux/FlvMuxer.hpp"
#include "publish/RtmpPublisher.hpp"
#include "timeline/AvTimeline.hpp"

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
            "TODO(user): in stage 1, orchestrate capture -> encode -> mux -> publish for video-only live");
    }

    Status runStage1VideoTrace(const std::string& rtmpUrl,
                               const std::string& sourceConfigPath,
                               int frameCountOverride,
                               TraceLog* trace) {
        if (trace == nullptr) {
            return Status::InvalidArgument("trace is null");
        }
        if (rtmpUrl.empty()) {
            return Status::InvalidArgument("rtmpUrl is empty");
        }
        if (sourceConfigPath.empty()) {
            return Status::InvalidArgument("sourceConfigPath is empty");
        }
        if (frameCountOverride < 0) {
            return Status::InvalidArgument("frameCountOverride must be >= 0");
        }

        capture::VideoCaptureSource capture;
        timeline::AvTimeline timeline;
        encode::VideoEncoder encoder;
        mux::FlvMuxer muxer;
        publish::RtmpPublisher publisher;

        trace->push("session", "stage1 trace begins");

        Status status = capture.configureFileSource(sourceConfigPath);
        if (!status.ok()) {
            return status;
        }

        const int frameCount = frameCountOverride > 0 ? frameCountOverride : capture.config().frameCount;
        if (frameCount > capture.config().frameCount) {
            return Status::InvalidArgument("frameCountOverride exceeds source file frame_count");
        }

        trace->push("capture",
                    "file source configured: label=" + capture.config().sourceLabel + " path=" +
                        capture.config().sourcePath + " " + std::to_string(capture.config().width) + "x" +
                        std::to_string(capture.config().height) + " @" + std::to_string(capture.config().fps) +
                        "fps frames=" + std::to_string(frameCount));

        status = capture.start();
        if (!status.ok()) {
            return status;
        }
        trace->push("capture", "source started");

        status = timeline.configureVideoClock(capture.config().fps);
        if (!status.ok()) {
            return status;
        }
        trace->push("timeline", "video clock configured from capture fps");

        status = encoder.configureH264(capture.config().width, capture.config().height, capture.config().fps, 2000000);
        if (!status.ok()) {
            return status;
        }
        trace->push("encode",
                    "FFmpeg encoder configured: H.264 " + std::to_string(capture.config().width) + "x" +
                        std::to_string(capture.config().height) + " bitrate=2000000");

        status = publisher.configureVideoStream(capture.config().width, capture.config().height, capture.config().fps);
        if (!status.ok()) {
            return status;
        }
        trace->push("publish",
                    "publisher video stream configured: " + std::to_string(capture.config().width) + "x" +
                        std::to_string(capture.config().height) + " @" + std::to_string(capture.config().fps) +
                        "fps");

        status = publisher.connect(rtmpUrl);
        if (!status.ok()) {
            return status;
        }
        trace->push("publish", "publisher entered publishing state for " + publisher.url());

        media::H264ParameterSets parameterSets;
        status = encoder.buildSequenceHeader(&parameterSets);
        if (!status.ok()) {
            return status;
        }
        trace->push("encode",
                    "sequence header ready: profile=" + parameterSets.profile + " " + parameterSets.spsLabel + " / " +
                        parameterSets.ppsLabel);

        media::FlvVideoTagPlan sequenceHeaderTag;
        status = muxer.buildVideoSequenceHeader(parameterSets, &sequenceHeaderTag);
        if (!status.ok()) {
            return status;
        }
        trace->push("mux",
                    "built FLV sequence header: " + sequenceHeaderTag.payloadSummary + " payload=" +
                        std::to_string(sequenceHeaderTag.payloadBytes.size()) + "B tag=" +
                        std::to_string(sequenceHeaderTag.tagBytes.size()) + "B");

        media::RtmpMessagePlan sequenceHeaderMessage;
        status = publisher.mapVideoTagToMessage(sequenceHeaderTag, &sequenceHeaderMessage);
        if (!status.ok()) {
            return status;
        }
        status = publisher.publishVideoTag(sequenceHeaderMessage);
        if (!status.ok()) {
            return status;
        }
        trace->push("publish",
                    "planned RTMP sequence header message: " + sequenceHeaderMessage.payloadSummary + " payload=" +
                        std::to_string(sequenceHeaderMessage.payloadBytes.size()) + "B");

        for (int i = 0; i < frameCount; ++i) {
            timeline::VideoTick tick;
            status = timeline.nextVideoTick(&tick);
            if (!status.ok()) {
                return status;
            }

            media::VideoFrame frame;
            status = capture.captureFrame(tick.frameIndex, tick.ptsMs, &frame);
            if (!status.ok()) {
                return status;
            }
            trace->push("capture", "frame#" + std::to_string(frame.frameIndex) + " pts=" + std::to_string(frame.ptsMs) +
                                      "ms source=" + frame.sourceLabel + " kind=" + frame.sourceKind + " payload=" +
                                      std::to_string(frame.pixelPayload.size()) + "B");

            media::EncodedVideoAccessUnit accessUnit;
            status = encoder.encodeFrame(frame, &accessUnit);
            if (!status.ok()) {
                return status;
            }
            trace->push("encode", "frame#" + std::to_string(accessUnit.frameIndex) +
                                     (accessUnit.keyFrame ? " keyframe " : " interframe ") +
                                     "pts=" + std::to_string(accessUnit.ptsMs) + "ms " + accessUnit.nalSummary);

            media::FlvVideoTagPlan tag;
            status = muxer.buildVideoNaluTag(accessUnit, &tag);
            if (!status.ok()) {
                return status;
            }
            trace->push("mux", "frame#" + std::to_string(tag.frameIndex) + " tag ts=" +
                                  std::to_string(tag.timestampMs) + "ms " + tag.payloadSummary + " payload=" +
                                  std::to_string(tag.payloadBytes.size()) + "B tag=" +
                                  std::to_string(tag.tagBytes.size()) + "B");

            media::RtmpMessagePlan message;
            status = publisher.mapVideoTagToMessage(tag, &message);
            if (!status.ok()) {
                return status;
            }
            status = publisher.publishVideoTag(message);
            if (!status.ok()) {
                return status;
            }
            trace->push("publish", "frame#" + std::to_string(message.frameIndex) + " message ts=" +
                                      std::to_string(message.timestampMs) + "ms " + message.payloadSummary +
                                      " payload=" + std::to_string(message.payloadBytes.size()) + "B");
        }

        capture.stop();
        publisher.disconnect();
        trace->push("session", "stage1 trace finished cleanly");
        return Status::Ok();
    }

    void close() noexcept {}
};

}  // namespace live_studio::session
