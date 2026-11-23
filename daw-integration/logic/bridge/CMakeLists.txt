cmake_minimum_required(VERSION 3.16)

# Native Apple-silicon binary. Set before project() so it applies to the
# compiler detection; -DCMAKE_OSX_ARCHITECTURES on the command line overrides.
if(APPLE AND NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "macOS target architecture")
endif()

project(tiles_logic_bridge LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TILES_LOGIC_BUILD_TESTS "Build tiles_logic_core, StudioLink, and LogicManager unit tests" ON)

add_library(tiles_logic_core STATIC
    tiles_logic_core.c
    tiles_logic_core.h
)

target_include_directories(tiles_logic_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_options(tiles_logic_core PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wpedantic
)

add_library(tiles_studio_link STATIC
    StudioLink.cpp
    StudioLink.hpp
)

target_link_libraries(tiles_studio_link PUBLIC
    tiles_logic_core
)

target_include_directories(tiles_studio_link PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_options(tiles_studio_link PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wpedantic
)

add_library(tiles_logic_manager STATIC
    LogicManager.cpp
    LogicManager.hpp
)

target_link_libraries(tiles_logic_manager PUBLIC
    tiles_logic_core
)

target_include_directories(tiles_logic_manager PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_options(tiles_logic_manager PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wpedantic
)

add_library(tiles_midi_logger STATIC
    MIDILogger.cpp
    MIDILogger.hpp
)

target_link_libraries(tiles_midi_logger PUBLIC
    tiles_logic_core
)

target_include_directories(tiles_midi_logger PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_options(tiles_midi_logger PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wpedantic
)

add_library(tiles_midi_monitor STATIC
    MIDIMonitor.cpp
    MIDIMonitor.hpp
)

target_link_libraries(tiles_midi_monitor PUBLIC
    tiles_logic_core
)

target_include_directories(tiles_midi_monitor PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_options(tiles_midi_monitor PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wpedantic
)

if(APPLE)
    find_library(COREMIDI_FRAMEWORK CoreMIDI REQUIRED)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    find_library(APPKIT_FRAMEWORK AppKit REQUIRED)

    target_link_libraries(tiles_logic_manager PUBLIC
        "${APPKIT_FRAMEWORK}"
        "${COREFOUNDATION_FRAMEWORK}"
        objc
    )

    target_link_libraries(tiles_midi_monitor PUBLIC
        "${APPKIT_FRAMEWORK}"
        "${COREFOUNDATION_FRAMEWORK}"
        objc
    )

    add_executable(tiles-logic-bridge
        tiles_logic_bridge.cpp
    )

    target_link_libraries(tiles-logic-bridge PRIVATE
        tiles_studio_link
        tiles_logic_manager
        tiles_midi_logger
        tiles_midi_monitor
        "${COREMIDI_FRAMEWORK}"
        "${COREFOUNDATION_FRAMEWORK}"
    )

    target_compile_options(tiles-logic-bridge PRIVATE
        -Wall
        -Wextra
        -Werror
    )

    install(TARGETS tiles-logic-bridge
        RUNTIME DESTINATION bin
    )
endif()

if(TILES_LOGIC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_logic_core.c")
    enable_testing()
    add_executable(test_logic_core
        ../tests/test_logic_core.c
    )
    target_link_libraries(test_logic_core PRIVATE
        tiles_logic_core
    )
    target_compile_options(test_logic_core PRIVATE
        -Wall
        -Wextra
        -Werror
        -Wpedantic
    )
    add_test(NAME tiles_logic_core_tests COMMAND test_logic_core)

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_studio_link.cpp")
        add_executable(test_studio_link
            ../tests/test_studio_link.cpp
        )
        target_link_libraries(test_studio_link PRIVATE
            tiles_studio_link
        )
        target_compile_options(test_studio_link PRIVATE
            -Wall
            -Wextra
            -Werror
            -Wpedantic
        )
        add_test(NAME tiles_studio_link_tests COMMAND test_studio_link)
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_logic_manager.cpp")
        add_executable(test_logic_manager
            ../tests/test_logic_manager.cpp
        )
        target_link_libraries(test_logic_manager PRIVATE
            tiles_logic_manager
        )
        target_compile_options(test_logic_manager PRIVATE
            -Wall
            -Wextra
            -Werror
            -Wpedantic
        )
        add_test(NAME tiles_logic_manager_tests COMMAND test_logic_manager)
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_midi_logger.cpp")
        add_executable(test_midi_logger
            ../tests/test_midi_logger.cpp
        )
        target_link_libraries(test_midi_logger PRIVATE
            tiles_midi_logger
        )
        target_compile_options(test_midi_logger PRIVATE
            -Wall
            -Wextra
            -Werror
            -Wpedantic
        )
        add_test(NAME tiles_midi_logger_tests COMMAND test_midi_logger)
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_midi_monitor.cpp")
        add_executable(test_midi_monitor
            ../tests/test_midi_monitor.cpp
        )
        target_link_libraries(test_midi_monitor PRIVATE
            tiles_midi_monitor
        )
        target_compile_options(test_midi_monitor PRIVATE
            -Wall
            -Wextra
            -Werror
            -Wpedantic
        )
        add_test(NAME tiles_midi_monitor_tests COMMAND test_midi_monitor)
    endif()
endif()
