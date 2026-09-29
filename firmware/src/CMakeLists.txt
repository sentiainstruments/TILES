add_executable(sentia_tiles_firmware
    main.c
    board/board_init.c
    board/pad_config.c
    diagnostics/i2c_scan.c
    diagnostics/calibration.c
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
    storage/storage_flash.c
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

# USB diagnostics console (CDC) + MIDI, composite device. Linking
# tinyusb_device explicitly (above) makes pico_stdio_usb defer both
# calling tusb_init() and providing descriptors to us -- see
# midi/usb_device.c and midi/usb_descriptors.c. Drop the CDC half once
# usb_vendor/ owns diagnostics instead.
pico_enable_stdio_usb(sentia_tiles_firmware 1)
pico_enable_stdio_uart(sentia_tiles_firmware 0)

# printf() over USB-CDC waits for buffer room when a terminal holds the port
# open but isn't reading -- up to pico-sdk's default 500 ms per stall, the
# cause of several "freeze"/"stuck note" bugs in this codebase (see
# services/README.md, "printf() can no longer stall the main loop"). 5 ms
# caps that for every printf at once: a reading terminal drains a 64-byte
# packet every 1 ms USB frame, so it never waits that long; one that isn't
# reading costs at most 5 ms once, after which prints are dropped without
# waiting until it reads again. Paired with the larger CDC TX buffer in
# midi/tusb_config.h so an ordinary burst never has to wait at all.
target_compile_definitions(sentia_tiles_firmware PRIVATE PICO_STDIO_USB_STDOUT_TIMEOUT_US=5000)

pico_add_extra_outputs(sentia_tiles_firmware)
