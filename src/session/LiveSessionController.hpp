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

    Status runStage1VideoTrace(const std::string& rtmpUrl, int frameCount, TraceLog* trace) {
        if (trace == nullptr) {
            return Status::InvalidArgument("trace is null");
        }
        if (rtmpUrl.empty()) {
            return Status::InvalidArgument("rtmpUrl is empty");
        }
        if (frameCount <= 0) {
            return Status::InvalidArgument("frameCount must be positive");
        }

        capture::VideoCaptureSource capture;
        timeline::AvTimeline timeline;
        encode::VideoEncoder encoder;
        mux::FlvMuxer muxer;
        publish::RtmpPublisher publisher;

        trace->push("session", "stage1 trace begins");

        Status status = capture.configureSyntheticSource(1280, 720, 25, frameCount);
        if (!status.ok()) {
            return status;
        }
        trace->push("capture", "synthetic source configured: 1280x720 @25fps");

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
        trace->push("encode", "trace encoder configured: H.264 1280x720 bitrate=2000000");

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
        trace->push("encode", "sequence header ready: " + parameterSets.spsLabel + " / " + parameterSets.ppsLabel);

        media::FlvVideoTagPlan sequenceHeaderTag;
        status = muxer.buildVideoSequenceHeader(parameterSets, &sequenceHeaderTag);
        if (!status.ok()) {
            return status;
        }
        trace->push("mux", "built FLV sequence header: " + sequenceHeaderTag.payloadSummary);

        media::RtmpMessagePlan sequenceHeaderMessage;
        status = publisher.mapVideoTagToMessage(sequenceHeaderTag, &sequenceHeaderMessage);
        if (!status.ok()) {
            return status;
        }
        status = publisher.publishVideoTag(sequenceHeaderMessage);
        if (!status.ok()) {
            return status;
        }
        trace->push("publish", "planned RTMP sequence header message: " + sequenceHeaderMessage.payloadSummary);

        for (int i = 0; i < frameCount; ++i) {
            timeline::VideoTick tick;
            status = timeline.nextVideoTick(&tick);
            if (!status.ok()) {
                return status;
            }

            media::VideoFrame frame;
            status = capture.captureSyntheticFrame(tick.frameIndex, tick.ptsMs, &frame);
            if (!status.ok()) {
                return status;
            }
            trace->push("capture", "frame#" + std::to_string(frame.frameIndex) + " pts=" + std::to_string(frame.ptsMs) +
                                      "ms source=" + frame.sourceLabel);

            media::EncodedVideoAccessUnit accessUnit;
            status = encoder.encodeTraceFrame(frame, &accessUnit);
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
                                  std::to_string(tag.timestampMs) + "ms " + tag.payloadSummary);

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
                                      std::to_string(message.timestampMs) + "ms " + message.payloadSummary);
        }

        capture.stop();
        publisher.disconnect();
        trace->push("session", "stage1 trace finished cleanly");
        return Status::Ok();
    }

    void close() noexcept {}
};

}  // namespace live_studio::session
