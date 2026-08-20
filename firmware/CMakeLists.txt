# Placeholder build file — not yet wired up.
#
# Pending decisions before this is real:
#   - pico-sdk / pico-extras location (submodule vs PICO_SDK_PATH env var)
#   - board id (pico2 / pico2_w) and TinyUSB config
#   - which of src/{board,drivers,services,midi,usb_vendor,profiles,
#     diagnostics,storage} have code yet, added incrementally per the
#     bring-up phases in docs/hardware/SENTIA_FIRMWARE_CODEX_START.md
#
# cmake_minimum_required(VERSION 3.13)
# include(pico_sdk_import.cmake)
# project(sentia_tiles_firmware C CXX ASM)
# set(CMAKE_C_STANDARD 11)
# set(CMAKE_CXX_STANDARD 17)
# pico_sdk_init()
# add_subdirectory(src)
