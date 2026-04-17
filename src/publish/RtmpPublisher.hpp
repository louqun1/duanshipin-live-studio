#pragma once

#include <memory>
#include <string>

#include "common/MediaTypes.hpp"
#include "common/Status.hpp"

namespace live_studio::publish {

struct PublisherContext;

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
    RtmpPublisher();
    ~RtmpPublisher();

    RtmpPublisher(const RtmpPublisher&) = delete;
    RtmpPublisher& operator=(const RtmpPublisher&) = delete;

    [[nodiscard]] std::string moduleName() const {
        return "RtmpPublisher";
    }

    [[nodiscard]] RtmpConnectionState state() const noexcept {
        return state_;
    }

    Status configureVideoStream(int width, int height, int fps);
    Status connect(const std::string& url);
    Status mapVideoTagToMessage(const media::FlvVideoTagPlan& tag, media::RtmpMessagePlan* outMessage) const;
    Status publishVideoTag(const media::RtmpMessagePlan& message);
    void disconnect() noexcept;

    [[nodiscard]] const std::string& url() const noexcept {
        return url_;
    }

    [[nodiscard]] const std::string& lastPublishedSummary() const noexcept {
        return lastPublishedSummary_;
    }

private:
    std::unique_ptr<PublisherContext> context_;
    RtmpConnectionState state_ = RtmpConnectionState::idle;
    std::string url_;
    std::string lastPublishedSummary_;
    int videoWidth_ = 0;
    int videoHeight_ = 0;
    int videoFps_ = 0;
};

}  // namespace live_studio::publish
