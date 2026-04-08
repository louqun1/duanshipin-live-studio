#pragma once

#include <cctype>
#include <cstdint>
#include <fstream>
#include <string>

#include "common/MediaTypes.hpp"
#include "common/Status.hpp"

namespace live_studio::capture {

struct VideoFrameDescriptor {
    int width = 0;
    int height = 0;
    int fps = 0;
    std::int64_t ptsMs = 0;
};

class VideoCaptureSource {
public:
    [[nodiscard]] std::string moduleName() const {
        return "VideoCaptureSource";
    }

    Status configureCamera(const std::string& deviceId, int width, int height, int fps) {
        if (deviceId.empty()) {
            return Status::InvalidArgument("deviceId is empty");
        }
        if (width <= 0 || height <= 0 || fps <= 0) {
            return Status::InvalidArgument("camera parameters must be positive");
        }
        return Status::NotImplemented(
            "TODO(user): connect a real camera capture backend after the file-backed source milestone");
    }

    Status configureFileSource(const std::string& sourcePath) {
        if (sourcePath.empty()) {
            return Status::InvalidArgument("sourcePath is empty");
        }
        media::VideoCaptureConfig parsedConfig;
        Status status = loadFileSourceConfig(sourcePath, &parsedConfig);
        if (!status.ok()) {
            return status;
        }

        config_ = parsedConfig;
        started_ = false;
        return Status::Ok();
    }

    Status start() {
        if (config_.fps <= 0 || config_.sourceKind == "unconfigured") {
            return Status::NotImplemented(
                "TODO(user): configure a file-backed or real camera source before start");
        }
        started_ = true;
        return Status::Ok();
    }

    Status captureFrame(int frameIndex, std::int64_t ptsMs, media::VideoFrame* outFrame) const {
        if (outFrame == nullptr) {
            return Status::InvalidArgument("outFrame is null");
        }
        if (!started_) {
            return Status::InvalidArgument("capture source is not started");
        }
        if (frameIndex < 0 || ptsMs < 0) {
            return Status::InvalidArgument("frameIndex and ptsMs must be >= 0");
        }
        if (config_.frameCount > 0 && frameIndex >= config_.frameCount) {
            return Status::InvalidArgument("frameIndex exceeds configured source frame count");
        }

        if (config_.sourceKind != "file") {
            return Status::NotImplemented("TODO(user): implement real frame capture for non-file sources");
        }

        outFrame->sourceKind = config_.sourceKind;
        outFrame->sourceLabel = config_.sourceLabel;
        outFrame->frameIndex = frameIndex;
        outFrame->width = config_.width;
        outFrame->height = config_.height;
        outFrame->fps = config_.fps;
        outFrame->ptsMs = ptsMs;
        outFrame->hasPixelPayload = false;
        return Status::Ok();
    }

    void stop() noexcept {
        started_ = false;
    }

    [[nodiscard]] const media::VideoCaptureConfig& config() const noexcept {
        return config_;
    }

private:
    static std::string trim(const std::string& text) {
        std::size_t first = 0;
        while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first])) != 0) {
            ++first;
        }

        std::size_t last = text.size();
        while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])) != 0) {
            --last;
        }
        return text.substr(first, last - first);
    }

    static Status parsePositiveInt(const std::string& key, const std::string& value, int* outValue) {
        if (outValue == nullptr) {
            return Status::InvalidArgument("outValue is null");
        }

        std::size_t parsedLength = 0;
        int parsedValue = 0;
        try {
            parsedValue = std::stoi(value, &parsedLength);
        } catch (...) {
            return Status::InvalidArgument("source config key '" + key + "' must be an integer");
        }

        if (parsedLength != value.size() || parsedValue <= 0) {
            return Status::InvalidArgument("source config key '" + key + "' must be a positive integer");
        }

        *outValue = parsedValue;
        return Status::Ok();
    }

    static Status loadFileSourceConfig(const std::string& sourcePath, media::VideoCaptureConfig* outConfig) {
        if (outConfig == nullptr) {
            return Status::InvalidArgument("outConfig is null");
        }

        std::ifstream input(sourcePath);
        if (!input.is_open()) {
            return Status::InvalidArgument("cannot open source config file: " + sourcePath);
        }

        media::VideoCaptureConfig parsedConfig;
        parsedConfig.sourceKind = "file";
        parsedConfig.sourceLabel = "file-source-0";
        parsedConfig.sourcePath = sourcePath;

        std::string line;
        int lineNumber = 0;
        while (std::getline(input, line)) {
            ++lineNumber;

            const std::string trimmedLine = trim(line);
            if (trimmedLine.empty() || trimmedLine[0] == '#') {
                continue;
            }

            const std::size_t separator = trimmedLine.find('=');
            if (separator == std::string::npos) {
                return Status::InvalidArgument(
                    "source config line " + std::to_string(lineNumber) + " must use key=value format");
            }

            const std::string key = trim(trimmedLine.substr(0, separator));
            const std::string value = trim(trimmedLine.substr(separator + 1));
            if (key.empty() || value.empty()) {
                return Status::InvalidArgument(
                    "source config line " + std::to_string(lineNumber) + " has an empty key or value");
            }

            Status status = Status::Ok();
            if (key == "source_label") {
                parsedConfig.sourceLabel = value;
            } else if (key == "width") {
                status = parsePositiveInt(key, value, &parsedConfig.width);
            } else if (key == "height") {
                status = parsePositiveInt(key, value, &parsedConfig.height);
            } else if (key == "fps") {
                status = parsePositiveInt(key, value, &parsedConfig.fps);
            } else if (key == "frame_count") {
                status = parsePositiveInt(key, value, &parsedConfig.frameCount);
            } else {
                return Status::InvalidArgument("unsupported source config key '" + key + "'");
            }

            if (!status.ok()) {
                return status;
            }
        }

        if (parsedConfig.width <= 0 || parsedConfig.height <= 0 || parsedConfig.fps <= 0 ||
            parsedConfig.frameCount <= 0) {
            return Status::InvalidArgument(
                "source config must define positive width, height, fps, and frame_count values");
        }

        *outConfig = parsedConfig;
        return Status::Ok();
    }

    media::VideoCaptureConfig config_;
    bool started_ = false;
};

}  // namespace live_studio::capture
