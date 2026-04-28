add_executable(rtc_guest_probe
    ../rtc_probe_common/SignalProbeApp.cpp
    main.cpp
)

target_include_directories(rtc_guest_probe
    PRIVATE
        ${CMAKE_SOURCE_DIR}/apps/rtc_probe_common
        ${CMAKE_SOURCE_DIR}/third_party/libdatachannel-src/deps/json/single_include
)

target_link_libraries(rtc_guest_probe
    PRIVATE
        LibDataChannel::LibDataChannel
)

set_target_properties(rtc_guest_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps/rtc_guest_probe
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/apps/rtc_guest_probe
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/apps/rtc_guest_probe
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/apps/rtc_guest_probe
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_BINARY_DIR}/apps/rtc_guest_probe
)

live_studio_copy_target_runtime_dlls(rtc_guest_probe)
