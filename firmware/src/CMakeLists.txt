add_executable(sentia_tiles_firmware
    main.c
    board/board_init.c
    board/pad_config.c
    diagnostics/i2c_scan.c
    diagnostics/calibration.c
    diagnostics/loop_stats.c
    drivers/sk6805.c
    drivers/tca9554.c
    drivers/tca9548a.c
    drivers/tmag5273.c
    drivers/pca9685.c
    drivers/mpr121.c
    drivers/i2c_bus.c
    drivers/dac80502.c
    services/cv_gate.c
    services/lighting.c
    services/hall.c
    services/buttons.c
    services/touch.c
    services/pedal.c
    services/note_map.c
    services/pixel_font.c
    services/expression.c
    services/power.c
    services/standby.c
    services/haptics.c
    services/boot_sequence.c
    services/octave_control.c
    services/game_mode.c
    services/midi_channels.c
    services/mpe_alloc.c
    services/expression_control.c
    services/midi_clock.c
    services/op_mode.c
    services/drum_seq.c
    services/drum_pattern.c
    services/debug_mode.c
    services/crash_indicator.c
    midi/usb_device.c
    midi/usb_descriptors.c
    midi/midi_out.c
    midi/midi_in.c
    midi/usb_midi_packet.c
    midi/identity.c
    midi/din_midi.c
    midi/din_midi_queue.c
    storage/kv_store.c
    storage/log_store.c
    storage/storage_flash.c
    profiles/content.c
    profiles/settings.c
    profiles/settings_persist.c
    profiles/settings_table.c
    usb_vendor/usb_vendor.c
)

pico_generate_pio_header(sentia_tiles_firmware ${CMAKE_CURRENT_SOURCE_DIR}/drivers/sk6805.pio)
pico_generate_pio_header(sentia_tiles_firmware ${CMAKE_CURRENT_SOURCE_DIR}/midi/din_midi_tx.pio)

target_include_directories(sentia_tiles_firmware PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/board
    ${CMAKE_CURRENT_SOURCE_DIR}/diagnostics
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers
    ${CMAKE_CURRENT_SOURCE_DIR}/services
    ${CMAKE_CURRENT_SOURCE_DIR}/midi
    ${CMAKE_CURRENT_SOURCE_DIR}/usb_vendor
    ${CMAKE_CURRENT_SOURCE_DIR}/storage
    ${CMAKE_CURRENT_SOURCE_DIR}/profiles
)

target_link_libraries(sentia_tiles_firmware
    pico_stdlib
    hardware_gpio
    hardware_i2c
    hardware_spi
    hardware_pio
    hardware_uart
    hardware_irq
    hardware_clocks
    hardware_adc
    hardware_watchdog
    hardware_flash
    tinyusb_device
    pico_unique_id
    pico_rand
    pico_usb_reset
    m
)

# stdio over the USB-CDC console. Linking tinyusb_device explicitly (above)
# makes pico_stdio_usb leave tusb_init() and the descriptors to us (see
# midi/usb_device.h, midi/usb_descriptors.c).
pico_enable_stdio_usb(sentia_tiles_firmware 1)
pico_enable_stdio_uart(sentia_tiles_firmware 0)

# printf() over USB-CDC waits for buffer room when a terminal holds the port
# open without reading: up to 500 ms per call by default, the cause of
# several freezes and stuck notes (services/HISTORY.md). 5 ms caps it: a
# reading terminal drains a packet every 1 ms frame, and one that isn't
# reading costs at most 5 ms once, then prints are dropped. Paired with the
# 1 KB CDC TX buffer in midi/tusb_config.h.
target_compile_definitions(sentia_tiles_firmware PRIVATE PICO_STDIO_USB_STDOUT_TIMEOUT_US=5000)

pico_add_extra_outputs(sentia_tiles_firmware)
