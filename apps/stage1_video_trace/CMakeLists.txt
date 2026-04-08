add_executable(stage1_video_trace
    main.cpp
)

target_include_directories(stage1_video_trace
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${LIVE_STUDIO_SPDLOG_INCLUDE_DIR}
)

set_target_properties(stage1_video_trace PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps/stage1_video_trace
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/apps/stage1_video_trace
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/apps/stage1_video_trace
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/apps/stage1_video_trace
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_BINARY_DIR}/apps/stage1_video_trace
)
