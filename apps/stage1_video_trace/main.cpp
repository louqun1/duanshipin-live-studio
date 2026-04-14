#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>

#include "common/TraceLog.hpp"
#include "logging/StudioLogger.hpp"
#include "session/LiveSessionController.hpp"
#include "spdlog/spdlog.h"

namespace {

bool tryParsePositiveInt(const char* text, int* outValue) {
    if (text == nullptr || outValue == nullptr) {
        return false;
    }

    char* end = nullptr;
    const long parsedValue = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || parsedValue <= 0 || parsedValue > std::numeric_limits<int>::max()) {
        return false;
    }

    *outValue = static_cast<int>(parsedValue);
    return true;
}

void printUsage(const std::filesystem::path& defaultSourceFile) {
    std::cout << "usage: stage1_video_trace [rtmp_url] [source_file|frame_count] [frame_count]\n";
    std::cout << "default source file: " << defaultSourceFile.string() << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path projectRoot =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const std::filesystem::path defaultSourceFile = projectRoot / "apps" / "stage1_video_trace" /
                                                    "default_video_source.txt";

    std::string rtmpUrl = "rtmp://127.0.0.1/live/stage1-trace";
    std::string sourceConfigPath = defaultSourceFile.string();
    int frameCountOverride = 0;

    if (argc >= 2 && argv[1] != nullptr) {
        const std::string firstArg = argv[1];
        if (firstArg == "--help" || firstArg == "-h") {
            printUsage(defaultSourceFile);
            return 0;
        }
        rtmpUrl = argv[1];
    }
    if (argc >= 3 && argv[2] != nullptr) {
        int parsedFrameCount = 0;
        if (tryParsePositiveInt(argv[2], &parsedFrameCount)) {
            frameCountOverride = parsedFrameCount;
        } else {
            sourceConfigPath = argv[2];
        }
    }
    if (argc >= 4 && argv[3] != nullptr) {
        if (!tryParsePositiveInt(argv[3], &frameCountOverride)) {
            std::cerr << "frame_count must be a positive integer\n";
            printUsage(defaultSourceFile);
            return 1;
        }
    }

    const live_studio::Status loggingStatus = live_studio::logging::initializeDefaultLogger(
        "stage1_video_trace", "stage1_video_trace.log", projectRoot);
    if (!loggingStatus.ok()) {
        std::cerr << "failed to initialize spdlog: " << loggingStatus.message << "\n";
        return 1;
    }

    spdlog::info("[app] duanshipin-live-studio stage 1 video trace");
    spdlog::info("[app] rtmp_url={} source_file={} frame_count_override={}",
                 rtmpUrl,
                 sourceConfigPath,
                 frameCountOverride);

    live_studio::TraceLog trace;
    live_studio::session::LiveSessionController controller;
    const live_studio::Status status =
        controller.runStage1VideoTrace(rtmpUrl, sourceConfigPath, frameCountOverride, &trace);

    if (!status.ok()) {
        spdlog::error("[app] stage 1 trace failed: {}", status.message);
        live_studio::logging::shutdown();
        return 1;
    }

    spdlog::info("[app] controller={}", controller.moduleName());
    spdlog::info("[app] current milestone status: file-backed raw frames + real H.264 encode + trace mux/publish");
    spdlog::info("[app] capture source now carries synthetic YUV420P payloads; RtmpPublisher remains a trace implementation");
    spdlog::info("[app] log_file={}", live_studio::logging::currentLogFilePath());
    live_studio::logging::shutdown();
    return 0;
}
