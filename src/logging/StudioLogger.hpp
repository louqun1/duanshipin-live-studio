#pragma once

#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "common/Status.hpp"
#include "spdlog/logger.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/spdlog.h"

namespace live_studio::logging {

inline std::string& currentLogFilePathStorage() {
    static std::string logFilePath;
    return logFilePath;
}

inline Status initializeDefaultLogger(const std::string& loggerName,
                                      const std::string& fileName,
                                      const std::filesystem::path& baseDir = std::filesystem::current_path()) {
    if (loggerName.empty()) {
        return Status::InvalidArgument("loggerName is empty");
    }
    if (fileName.empty()) {
        return Status::InvalidArgument("fileName is empty");
    }

    try {
        const std::filesystem::path logDir = baseDir / "logs";
        std::error_code error;
        std::filesystem::create_directories(logDir, error);
        if (error) {
            return Status::InternalError("failed to create log directory: " + error.message());
        }

        const std::filesystem::path logFilePath = logDir / fileName;
        auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFilePath.string(), false);

        std::vector<spdlog::sink_ptr> sinks{consoleSink, fileSink};
        auto logger = std::make_shared<spdlog::logger>(loggerName, sinks.begin(), sinks.end());
        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::info);

        spdlog::set_default_logger(logger);
        spdlog::set_level(spdlog::level::info);
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [thread %t] %v");

        currentLogFilePathStorage() = logFilePath.string();
        spdlog::info("[logging] initialized logger={} file={}", loggerName, currentLogFilePathStorage());
        return Status::Ok();
    } catch (const std::exception& exception) {
        return Status::InternalError("failed to initialize spdlog: " + std::string(exception.what()));
    }
}

inline const std::string& currentLogFilePath() {
    return currentLogFilePathStorage();
}

inline void shutdown() {
    spdlog::shutdown();
}

}  // namespace live_studio::logging
