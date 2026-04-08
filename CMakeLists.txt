cmake_minimum_required(VERSION 3.20)

project(duanshipin_live_studio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(LIVE_STUDIO_SPDLOG_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party/spdlog/include")
if(NOT EXISTS "${LIVE_STUDIO_SPDLOG_INCLUDE_DIR}/spdlog/spdlog.h")
    message(FATAL_ERROR "vendored spdlog not found at ${LIVE_STUDIO_SPDLOG_INCLUDE_DIR}")
endif()

add_subdirectory(apps/stage1_video_trace)
