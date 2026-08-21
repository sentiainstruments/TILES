add_executable(sentia_tiles_firmware
    main.c
    board/board_init.c
    board/pad_config.c
    diagnostics/i2c_scan.c
    drivers/sk6805.c
    drivers/tca9554.c
    services/lighting.c
)

pico_generate_pio_header(sentia_tiles_firmware ${CMAKE_CURRENT_SOURCE_DIR}/drivers/sk6805.pio)

target_include_directories(sentia_tiles_firmware PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/board
    ${CMAKE_CURRENT_SOURCE_DIR}/diagnostics
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers
    ${CMAKE_CURRENT_SOURCE_DIR}/services
)

target_link_libraries(sentia_tiles_firmware
    pico_stdlib
    hardware_gpio
    hardware_i2c
    hardware_pio
    hardware_clocks
)

# USB diagnostics over the console during early bring-up; drop once
# usb_vendor/ owns the USB interface.
pico_enable_stdio_usb(sentia_tiles_firmware 1)
pico_enable_stdio_uart(sentia_tiles_firmware 0)

pico_add_extra_outputs(sentia_tiles_firmware)
