add_executable(sentia_tiles_firmware
    main.c
    board/board_init.c
    board/pad_config.c
)

target_include_directories(sentia_tiles_firmware PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/board
)

target_link_libraries(sentia_tiles_firmware
    pico_stdlib
    hardware_gpio
    hardware_i2c
)

# USB diagnostics over the console during early bring-up; drop once
# usb_vendor/ owns the USB interface.
pico_enable_stdio_usb(sentia_tiles_firmware 1)
pico_enable_stdio_uart(sentia_tiles_firmware 0)

pico_add_extra_outputs(sentia_tiles_firmware)
