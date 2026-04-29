function(live_studio_require_libdatachannel)
    if(TARGET LibDataChannel::LibDataChannel)
        return()
    endif()

    set(_live_studio_libdatachannel_hints "")
    if(EXISTS "${LIVE_STUDIO_LIBDATACHANNEL_INSTALL_DIR}")
        list(APPEND _live_studio_libdatachannel_hints
            "${LIVE_STUDIO_LIBDATACHANNEL_INSTALL_DIR}")
    endif()

    find_package(LibDataChannel CONFIG REQUIRED
        HINTS ${_live_studio_libdatachannel_hints}
    )

    if(DEFINED LibDataChannel_DIR)
        message(STATUS "duanshipin-live-studio libdatachannel package: ${LibDataChannel_DIR}")
    endif()
endfunction()

function(live_studio_copy_target_runtime_dlls target_name)
    if(NOT WIN32)
        return()
    endif()

    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:${target_name}>
            $<TARGET_FILE_DIR:${target_name}>
        COMMAND_EXPAND_LISTS
    )

    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        get_filename_component(_live_studio_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
        foreach(_live_studio_runtime_dll
                libgcc_s_seh-1.dll
                libstdc++-6.dll
                libwinpthread-1.dll)
            if(EXISTS "${_live_studio_compiler_bin}/${_live_studio_runtime_dll}")
                add_custom_command(TARGET ${target_name} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_live_studio_compiler_bin}/${_live_studio_runtime_dll}"
                        "$<TARGET_FILE_DIR:${target_name}>"
                )
            endif()
        endforeach()
    endif()

    if(DEFINED LIVE_STUDIO_FFMPEG_ROOT AND EXISTS "${LIVE_STUDIO_FFMPEG_ROOT}/bin")
        file(GLOB _live_studio_ffmpeg_runtime_dlls "${LIVE_STUDIO_FFMPEG_ROOT}/bin/*.dll")
        if(_live_studio_ffmpeg_runtime_dlls)
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${_live_studio_ffmpeg_runtime_dlls}
                    "$<TARGET_FILE_DIR:${target_name}>"
                COMMAND_EXPAND_LISTS
            )
        endif()
    endif()
endfunction()
