cmake_minimum_required(VERSION 3.20)

project(duanshipin_live_studio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(LiveStudioRtcDeps)

set(LIVE_STUDIO_QT_ROOT "D:/itffmpeg/Qt/6.10.3/llvm-mingw_64" CACHE PATH
    "Qt 6 root used for the Widgets publish panel")
set(_live_studio_default_build_qt_panel ON)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND LIVE_STUDIO_QT_ROOT MATCHES "llvm-mingw")
    set(_live_studio_default_build_qt_panel OFF)
endif()
option(LIVE_STUDIO_BUILD_QT_PANEL "Build the minimal Qt Widgets publish panel"
       ${_live_studio_default_build_qt_panel})
option(LIVE_STUDIO_BUILD_RTC_PROBES "Build the libdatachannel-based RTC probe apps" OFF)
set(_live_studio_default_libdatachannel_install_dir
    "${CMAKE_SOURCE_DIR}/third_party/libdatachannel-install")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND
   EXISTS "${CMAKE_SOURCE_DIR}/third_party/libdatachannel-install-clang")
    set(_live_studio_default_libdatachannel_install_dir
        "${CMAKE_SOURCE_DIR}/third_party/libdatachannel-install-clang")
endif()
set(LIVE_STUDIO_LIBDATACHANNEL_INSTALL_DIR
    "${_live_studio_default_libdatachannel_install_dir}"
    CACHE PATH
    "Root directory of the installed LibDataChannel package")
if(LIVE_STUDIO_QT_ROOT MATCHES "llvm-mingw")
    set(_live_studio_legacy_libdatachannel_install_dir
        "${CMAKE_SOURCE_DIR}/third_party/libdatachannel-install")
    set(_live_studio_llvm_libdatachannel_install_dir
        "${CMAKE_SOURCE_DIR}/third_party/libdatachannel-install-clang")
    set(_live_studio_legacy_libdatachannel_package_dir
        "${_live_studio_legacy_libdatachannel_install_dir}/lib/cmake/LibDataChannel")
    set(_live_studio_llvm_libdatachannel_package_dir
        "${_live_studio_llvm_libdatachannel_install_dir}/lib/cmake/LibDataChannel")

    if(LIVE_STUDIO_LIBDATACHANNEL_INSTALL_DIR STREQUAL
       "${_live_studio_legacy_libdatachannel_install_dir}" AND
       EXISTS "${_live_studio_llvm_libdatachannel_package_dir}")
        set(LIVE_STUDIO_LIBDATACHANNEL_INSTALL_DIR
            "${_live_studio_llvm_libdatachannel_install_dir}"
            CACHE PATH
            "Root directory of the installed LibDataChannel package"
            FORCE)
    endif()

    if(DEFINED LibDataChannel_DIR AND
       LibDataChannel_DIR STREQUAL "${_live_studio_legacy_libdatachannel_package_dir}" AND
       EXISTS "${_live_studio_llvm_libdatachannel_package_dir}")
        set(LibDataChannel_DIR
            "${_live_studio_llvm_libdatachannel_package_dir}"
            CACHE PATH
            "CMake package directory for LibDataChannel"
            FORCE)
    endif()
endif()
set(LIVE_STUDIO_OPENSSL_ROOT_DIR
    "D:/itffmpeg/x86_64-15.2.0-release-posix-seh-ucrt-rt_v13-rev1/mingw64/opt"
    CACHE PATH
    "Root directory of the OpenSSL runtime used by libdatachannel-based apps")

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
set(LIVE_STUDIO_SDL2_INCLUDE_DIR "${LIVE_STUDIO_FFMPEG_ROOT}/include/SDL2")

live_studio_import_shared_library(
    FFmpeg::avcodec
    "${LIVE_STUDIO_FFMPEG_ROOT}/lib/avcodec.lib"
    "${LIVE_STUDIO_FFMPEG_ROOT}/bin/avcodec-60.dll"
)
live_studio_import_shared_library(
    FFmpeg::avdevice
    "${LIVE_STUDIO_FFMPEG_ROOT}/lib/avdevice.lib"
    "${LIVE_STUDIO_FFMPEG_ROOT}/bin/avdevice-60.dll"
)
live_studio_import_shared_library(
    FFmpeg::avformat
    "${LIVE_STUDIO_FFMPEG_ROOT}/lib/avformat.lib"
    "${LIVE_STUDIO_FFMPEG_ROOT}/bin/avformat-60.dll"
)
live_studio_import_shared_library(
    FFmpeg::avutil
    "${LIVE_STUDIO_FFMPEG_ROOT}/lib/avutil.lib"
    "${LIVE_STUDIO_FFMPEG_ROOT}/bin/avutil-58.dll"
)
live_studio_import_shared_library(
    FFmpeg::swresample
    "${LIVE_STUDIO_FFMPEG_ROOT}/lib/swresample.lib"
    "${LIVE_STUDIO_FFMPEG_ROOT}/bin/swresample-4.dll"
)
live_studio_import_shared_library(
    FFmpeg::swscale
    "${LIVE_STUDIO_FFMPEG_ROOT}/lib/swscale.lib"
    "${LIVE_STUDIO_FFMPEG_ROOT}/bin/swscale-7.dll"
)
live_studio_import_shared_library(
    SDL2::SDL2
    "${LIVE_STUDIO_FFMPEG_ROOT}/lib/SDL2.lib"
    "${LIVE_STUDIO_FFMPEG_ROOT}/bin/SDL2.dll"
)

message(STATUS "duanshipin-live-studio FFmpeg root: ${LIVE_STUDIO_FFMPEG_ROOT}")

add_library(live_studio_core
    src/capture/LiveDeviceCaptureSource.cpp
    src/common/WavFileWriter.cpp
    src/decode/OpusAudioDecoder.cpp
    src/encode/AudioEncoder.cpp
    src/encode/OpusAudioEncoder.cpp
    src/encode/VideoEncoder.cpp
    src/publish/RtmpPublisher.cpp
    src/session/LiveSessionController.cpp
    src/session/LiveSessionControllerPipeline.cpp
    src/session/LiveSessionControllerPublish.cpp
    src/session/LiveSessionControllerWorker.cpp
)

target_include_directories(live_studio_core
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src
    PRIVATE
        ${LIVE_STUDIO_SPDLOG_INCLUDE_DIR}
        ${LIVE_STUDIO_FFMPEG_INCLUDE_DIR}
)

target_link_libraries(live_studio_core
    PUBLIC
        FFmpeg::avcodec
        FFmpeg::avutil
        FFmpeg::swresample
        FFmpeg::swscale
)

if(WIN32)
    target_link_libraries(live_studio_core
        PUBLIC
            avrt
            ksuser
            mf
            mfplat
            mfreadwrite
            mfuuid
            mmdevapi
            ole32
            uuid
            ws2_32
    )
endif()

if(LIVE_STUDIO_BUILD_RTC_PROBES)
    live_studio_require_libdatachannel()
    add_subdirectory(apps/rtc_probe)
    add_subdirectory(apps/rtc_guest_probe)
endif()

if(LIVE_STUDIO_BUILD_QT_PANEL)
    if(EXISTS "${LIVE_STUDIO_QT_ROOT}/lib/cmake/Qt6/Qt6Config.cmake")
        list(PREPEND CMAKE_PREFIX_PATH "${LIVE_STUDIO_QT_ROOT}")
    endif()

    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND LIVE_STUDIO_QT_ROOT MATCHES "llvm-mingw")
        message(WARNING
            "LIVE_STUDIO_QT_ROOT points to an llvm-mingw Qt package. "
            "If the Qt panel build fails under GCC, configure a separate build directory with "
            "D:/itffmpeg/Qt/Tools/llvm-mingw1706_64/bin/clang++.exe and clang.exe.")
    endif()

    find_package(Qt6 COMPONENTS Core Gui Widgets Concurrent Network OpenGL OpenGLWidgets QUIET)
    if(Qt6_FOUND)
        add_subdirectory(apps/live_studio_panel)
    else()
        message(WARNING
            "Qt6 was not found; live_studio_panel will be skipped. "
            "Set LIVE_STUDIO_QT_ROOT or CMAKE_PREFIX_PATH to a valid Qt 6 installation.")
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND LIVE_STUDIO_QT_ROOT MATCHES "llvm-mingw")
    message(STATUS
        "Skipping live_studio_panel by default for GCC builds because LIVE_STUDIO_QT_ROOT points to "
        "an llvm-mingw Qt package. Use a separate clang/llvm-mingw build directory or set "
        "LIVE_STUDIO_BUILD_QT_PANEL=ON to override.")
endif()
