if(NOT TARGET Qt6::Core OR NOT TARGET Qt6::Gui OR NOT TARGET Qt6::Widgets OR
   NOT TARGET Qt6::Concurrent OR NOT TARGET Qt6::Network OR
   NOT TARGET Qt6::OpenGL OR NOT TARGET Qt6::OpenGLWidgets)
    message(WARNING "Skipping live_studio_panel because required Qt6 base modules are missing.")
    return()
endif()

live_studio_require_libdatachannel()

find_package(Qt6WebSockets QUIET)
if(NOT TARGET Qt6::WebSockets)
    message(WARNING "Skipping live_studio_panel because Qt6 WebSockets is not available.")
    return()
endif()

add_executable(live_studio_panel
    BusinessLiveClient.cpp
    BusinessLiveClient.hpp
    BusinessLiveTypes.hpp
    LiveStudioWindow.cpp
    LiveStudioWindow.hpp
    main.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/SdlAudioOutput.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/SdlAudioOutput.hpp
    ${CMAKE_SOURCE_DIR}/src/decode/H264VideoDecoder.cpp
    ${CMAKE_SOURCE_DIR}/src/decode/H264VideoDecoder.hpp
    ${CMAKE_SOURCE_DIR}/src/render/VideoOpenGLWidget.cpp
    ${CMAKE_SOURCE_DIR}/src/render/VideoOpenGLWidget.hpp
    ${CMAKE_SOURCE_DIR}/src/linkmic/LinkMicRoomState.cpp
    ${CMAKE_SOURCE_DIR}/src/linkmic/LinkMicSession.cpp
    ${CMAKE_SOURCE_DIR}/src/linkmic/LinkMicSignalingClient.cpp
    ${CMAKE_SOURCE_DIR}/src/linkmic/RoomMemberDirectory.cpp
    ${CMAKE_SOURCE_DIR}/src/rtc/RtcSignalingBridge.cpp
    ${CMAKE_SOURCE_DIR}/src/rtc/RtcRemoteAudioSink.cpp
    ${CMAKE_SOURCE_DIR}/src/rtc/RtcRemoteAudioSink.hpp
    ${CMAKE_SOURCE_DIR}/src/rtc/RtcRemoteVideoSink.cpp
    ${CMAKE_SOURCE_DIR}/src/rtc/RtcRemoteVideoSink.hpp
    ${CMAKE_SOURCE_DIR}/src/rtc/RtcPeerSession.cpp
)

target_include_directories(live_studio_panel
    PRIVATE
        ${LIVE_STUDIO_FFMPEG_INCLUDE_DIR}
        ${LIVE_STUDIO_SPDLOG_INCLUDE_DIR}
        ${LIVE_STUDIO_SDL2_INCLUDE_DIR}
)

target_link_libraries(live_studio_panel
    PRIVATE
        Qt6::Concurrent
        Qt6::Core
        Qt6::Gui
        Qt6::Network
        Qt6::OpenGL
        Qt6::OpenGLWidgets
        Qt6::WebSockets
        Qt6::Widgets
        LibDataChannel::LibDataChannel
        SDL2::SDL2
        live_studio_core
)

set_target_properties(live_studio_panel PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps/live_studio_panel
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/apps/live_studio_panel
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/apps/live_studio_panel
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/apps/live_studio_panel
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_BINARY_DIR}/apps/live_studio_panel
)

file(GLOB _live_studio_ffmpeg_runtime_files
    "${LIVE_STUDIO_FFMPEG_ROOT}/bin/*.dll"
)

foreach(_live_studio_runtime_file IN LISTS _live_studio_ffmpeg_runtime_files)
    add_custom_command(TARGET live_studio_panel POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_live_studio_runtime_file}"
            $<TARGET_FILE_DIR:live_studio_panel>
    )
endforeach()

live_studio_copy_target_runtime_dlls(live_studio_panel)

if(WIN32)
    find_program(LIVE_STUDIO_WINDEPLOYQT_EXECUTABLE
        NAMES windeployqt windeployqt6
        HINTS "${LIVE_STUDIO_QT_ROOT}/bin"
    )

    if(LIVE_STUDIO_WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET live_studio_panel POST_BUILD
            COMMAND "${LIVE_STUDIO_WINDEPLOYQT_EXECUTABLE}"
                --no-translations
                --verbose
                0
                "$<TARGET_FILE:live_studio_panel>"
            COMMENT "Deploying Qt runtime for live_studio_panel"
        )
    endif()
endif()
