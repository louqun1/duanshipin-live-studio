add_executable(live_studio_panel
    LiveStudioWindow.cpp
    LiveStudioWindow.hpp
    main.cpp
)

target_include_directories(live_studio_panel
    PRIVATE
        ${LIVE_STUDIO_SPDLOG_INCLUDE_DIR}
)

target_link_libraries(live_studio_panel
    PRIVATE
        Qt6::Concurrent
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        live_studio_core
)

set_target_properties(live_studio_panel PROPERTIES
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
