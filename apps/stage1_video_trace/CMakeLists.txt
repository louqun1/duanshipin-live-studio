add_executable(stage1_video_trace
    main.cpp
    ${CMAKE_SOURCE_DIR}/src/capture/LiveDeviceCaptureSource.cpp
    ${CMAKE_SOURCE_DIR}/src/encode/AudioEncoder.cpp
    ${CMAKE_SOURCE_DIR}/src/encode/VideoEncoder.cpp
    ${CMAKE_SOURCE_DIR}/src/publish/RtmpPublisher.cpp
)

target_include_directories(stage1_video_trace
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${LIVE_STUDIO_SPDLOG_INCLUDE_DIR}
        ${LIVE_STUDIO_FFMPEG_INCLUDE_DIR}
)

target_link_libraries(stage1_video_trace
    PRIVATE
        FFmpeg::avcodec
        FFmpeg::avutil
        FFmpeg::swresample
        FFmpeg::swscale
)

if(WIN32)
    target_link_libraries(stage1_video_trace
        PRIVATE
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

set_target_properties(stage1_video_trace PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps/stage1_video_trace
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/apps/stage1_video_trace
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/apps/stage1_video_trace
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/apps/stage1_video_trace
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_BINARY_DIR}/apps/stage1_video_trace
)

file(GLOB _live_studio_ffmpeg_runtime_files
    "${LIVE_STUDIO_FFMPEG_ROOT}/bin/*.dll"
)

foreach(_live_studio_runtime_file IN LISTS _live_studio_ffmpeg_runtime_files)
    add_custom_command(TARGET stage1_video_trace POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_live_studio_runtime_file}"
            $<TARGET_FILE_DIR:stage1_video_trace>
    )
endforeach()
