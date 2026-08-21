cmake_minimum_required(VERSION 3.13)

# Requires PICO_SDK_PATH set in the environment, and pico_sdk_import.cmake
# copied into this directory from $PICO_SDK_PATH/external/pico_sdk_import.cmake
# -- see BUILD.md.
include(pico_sdk_import.cmake)

project(sentia_tiles_firmware C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_subdirectory(src)
