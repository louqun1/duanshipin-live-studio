#pragma once

#include <string>

#include "common/MediaTypes.hpp"
#include "common/Status.hpp"

namespace live_studio::publish {

enum class RtmpConnectionState {
    idle,
    connecting,
    connected,
    publishing,
    reconnecting,
    failed,
};

class RtmpPublisher {
public:
    [[nodiscard]] std::string moduleName() const {
        return "RtmpPublisher";
    }

    [[nodiscard]] RtmpConnectionState state() const noexcept {
        return state_;
    }

    Status connect(const std::string& url) {
        if (url.empty()) {
            return Status::InvalidArgument("rtmp url is empty");
        }
        if (url.rfind("rtmp://", 0) != 0) {
            return Status::InvalidArgument("rtmp url must start with rtmp://");
        }
        state_ = RtmpConnectionState::connecting;
        url_ = url;
        state_ = RtmpConnectionState::publishing;
        return Status::Ok();
    }

    Status mapVideoTagToMessage(const media::FlvVideoTagPlan& tag, media::RtmpMessagePlan* outMessage) const {
        if (outMessage == nullptr) {
            return Status::InvalidArgument("outMessage is null");
        }
        if (tag.timestampMs < 0) {
            return Status::InvalidArgument("tag timestamp must be >= 0");
        }
        if (tag.payloadBytes.empty()) {
            return Status::InvalidArgument("tag payload bytes are empty");
        }

        outMessage->chunkStreamId = 6;
        outMessage->messageStreamId = 1;
        outMessage->typeId = 9;
        outMessage->frameIndex = tag.frameIndex;
        outMessage->timestampMs = tag.timestampMs;
        outMessage->payloadSummary = (tag.sequenceHeader ? "rtmp-video-sequence-header(" : "rtmp-video-tag(") +
                                     tag.payloadSummary + ")";
        outMessage->payloadBytes = tag.payloadBytes;
        return Status::Ok();
    }

    Status publishVideoTag(const media::RtmpMessagePlan& message) {
        if (state_ != RtmpConnectionState::publishing) {
            return Status::InvalidArgument("publisher is not in publishing state");
        }
        if (message.payloadBytes.empty()) {
            return Status::InvalidArgument("message payload bytes are empty");
        }
        lastPublishedSummary_ = message.payloadSummary;
        return Status::Ok();
    }

    void disconnect() noexcept {
        state_ = RtmpConnectionState::idle;
        url_.clear();
        lastPublishedSummary_.clear();
    }

    [[nodiscard]] const std::string& url() const noexcept {
        return url_;
    }

    [[nodiscard]] const std::string& lastPublishedSummary() const noexcept {
        return lastPublishedSummary_;
    }

private:
    RtmpConnectionState state_ = RtmpConnectionState::idle;
    std::string url_;
    std::string lastPublishedSummary_;
};

}  // namespace live_studio::publish
