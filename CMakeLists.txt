cmake_minimum_required(VERSION 3.20)

project(duanshipin_live_studio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(LIVE_STUDIO_SPDLOG_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party/spdlog/include")
if(NOT EXISTS "${LIVE_STUDIO_SPDLOG_INCLUDE_DIR}/spdlog/spdlog.h")
    message(FATAL_ERROR "vendored spdlog not found at ${LIVE_STUDIO_SPDLOG_INCLUDE_DIR}")
endif()

function(live_studio_import_shared_library target_name import_lib runtime_dll)
    if(NOT TARGET ${target_name})
        add_library(${target_name} SHARED IMPORTED GLOBAL)
        set_target_properties(${target_name} PROPERTIES
            IMPORTED_IMPLIB "${import_lib}"
            IMPORTED_LOCATION "${runtime_dll}"
        )
    endif()
endfunction()

set(LIVE_STUDIO_FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/third_party/ffmpeg6.0")

set(LIVE_STUDIO_FFMPEG_INCLUDE_DIR "${LIVE_STUDIO_FFMPEG_ROOT}/include")

live_studio_import_shared_library(
    FFmpeg::avcodec
    "${LIVE_STUDIO_FFMPEG_ROOT}/lib/avcodec.lib"
    "${LIVE_STUDIO_FFMPEG_ROOT}/bin/avcodec-60.dll"
)
live_studio_import_shared_library(
    FFmpeg::avutil
    "${LIVE_STUDIO_FFMPEG_ROOT}/lib/avutil.lib"
    "${LIVE_STUDIO_FFMPEG_ROOT}/bin/avutil-58.dll"
)

message(STATUS "duanshipin-live-studio FFmpeg root: ${LIVE_STUDIO_FFMPEG_ROOT}")

add_subdirectory(apps/stage1_video_trace)
