add_executable(stage0_probe
    main.cpp
)

target_include_directories(stage0_probe
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

set_target_properties(stage0_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps/stage0_probe
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/apps/stage0_probe
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/apps/stage0_probe
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/apps/stage0_probe
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_BINARY_DIR}/apps/stage0_probe
)
