add_executable(sentia_tiles_firmware
    main.c
    board/board_init.c
    board/pad_config.c
    diagnostics/i2c_scan.c
    diagnostics/capture.c
    diagnostics/calibration.c
    diagnostics/boot_stage.c
    diagnostics/chip.c
    diagnostics/fault.c
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
    services/show_link.c
    services/sound_menu.c
    services/haptics.c
    services/boot_sequence.c
    services/octave_control.c
    services/game_mode.c
    services/midi_channels.c
    services/device_mode.c
    services/mpe_alloc.c
    services/expression_control.c
    services/midi_clock.c
    services/op_mode.c
    services/drum_seq.c
    services/harmony.c
    services/harmony_core.c
    services/bounce.c
    services/bounce_core.c
    services/physics.c
    services/physics_core.c
    services/bend_core.c
    services/tune.c
    services/strike_settle.c
    services/rest_guard.c
    services/seq_pattern.c
    services/light_effects.c
    services/play_state.c
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
    storage/flash_staging.c
    storage/storage_flash.c
    profiles/content.c
    profiles/settings.c
    profiles/settings_persist.c
    profiles/settings_table.c
    usb_vendor/usb_vendor.c
    audio/audio_engine.c
    audio/fm_patches.c
    audio/fm_voice.c
    audio/sample_slot.c
    audio/sample_store.c
    audio/sample_stream.c
    audio/sampler.c
    audio/fx.c
    audio/fx_memory.c
    audio/synth_patches.c
    audio/synth_voice.c
    audio/usb_audio.c
    audio/wavetable.c
    audio/wavetable_banks.c
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
    ${CMAKE_CURRENT_SOURCE_DIR}/audio
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
    pico_multicore
    hardware_dma
    pico_rand
    pico_usb_reset
    m
)

# stdio over the USB-CDC console. Linking tinyusb_device explicitly (above)
# makes pico_stdio_usb leave tusb_init() and the descriptors to us (see
# midi/usb_device.h, midi/usb_descriptors.c).
pico_enable_stdio_usb(sentia_tiles_firmware 1)

# picotool's reset requests reach core 1 in standalone mode; usb_device.c
# hands them to core 0, which feeds the watchdog.
target_link_options(sentia_tiles_firmware PRIVATE
    -Wl,--wrap=rom_reset_usb_boot_extra
    -Wl,--wrap=watchdog_reboot
)
pico_enable_stdio_uart(sentia_tiles_firmware 0)

# printf() over USB-CDC waits for buffer room when a terminal holds the port
# open without reading: up to 500 ms per call by default, the cause of
# several freezes and stuck notes (services/HISTORY.md). 5 ms caps it: a
# reading terminal drains a packet every 1 ms frame, and one that isn't
# reading costs at most 5 ms once, then prints are dropped. Paired with the
# 1 KB CDC TX buffer in midi/tusb_config.h.
target_compile_definitions(sentia_tiles_firmware PRIVATE PICO_STDIO_USB_STDOUT_TIMEOUT_US=5000)

# Core 1 runs the audio engine (audio/usb_audio.c); 4 KB rather than the
# SDK's 2 KB leaves room for the engine and pico_time's sleep.
target_compile_definitions(sentia_tiles_firmware PRIVATE PICO_CORE1_STACK_SIZE=0x1000)
# panic() records its message and restarts (diagnostics/fault.h).
target_compile_definitions(sentia_tiles_firmware PRIVATE PICO_PANIC_FUNCTION=tiles_panic)

# No fused multiply-adds in the bend math, so the computer's replay
# (test/bend_replay.c, built the same way) matches the board bit for bit.
set_source_files_properties(services/bend_core.c PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")

# cmake -DTILES_DSP_BENCH=ON: the DSP stress test (audio/dsp_bench.c,
# settings shell BENCH). Never in a release: it holds 180 KB of RAM.
option(TILES_DSP_BENCH "Build the DSP stress test" OFF)
if(TILES_DSP_BENCH)
    target_sources(sentia_tiles_firmware PRIVATE audio/dsp_bench.c)
    target_compile_definitions(sentia_tiles_firmware PRIVATE TILES_DSP_BENCH=1)
endif()

# cmake -DTILES_UNIT=N: a build for unit N (board/unit_id.h's default
# otherwise; tools/make_update_kit.sh).
if(DEFINED TILES_UNIT)
    target_compile_definitions(sentia_tiles_firmware PRIVATE TILES_UNIT_NUMBER=${TILES_UNIT}u)
endif()

pico_add_extra_outputs(sentia_tiles_firmware)

# The system clock: 150 MHz (the RP2350's rating) unless qualified
# otherwise; 230.4 MHz is the target, the steps above it margin tests
# (docs/architecture/synth-engines.md, "Next"). Each step's PLL and core
# voltage; above 160 MHz the flash runs at a third of the clock, sampled 3
# half-cycles late: about the timing 150 MHz gives (75 MHz, 6.7 ns).
set(TILES_CLK_KHZ 150000 CACHE STRING "System clock in kHz: 150000, 153600, 230400, 268800 or 288000")
set(TILES_VREG "" CACHE STRING "Core voltage as a VREG_VOLTAGE_ suffix (1_15, 1_20); empty: the step's own")
if(NOT TILES_CLK_KHZ EQUAL 150000)
    if(TILES_CLK_KHZ EQUAL 153600)
        set(tiles_vco 768000000)
        set(tiles_pd1 5)
        set(tiles_pd2 1)
        set(tiles_vreg 1_10)
    elseif(TILES_CLK_KHZ EQUAL 230400)
        set(tiles_vco 1152000000)
        set(tiles_pd1 5)
        set(tiles_pd2 1)
        set(tiles_vreg 1_15)
    elseif(TILES_CLK_KHZ EQUAL 268800)
        set(tiles_vco 1344000000)
        set(tiles_pd1 5)
        set(tiles_pd2 1)
        set(tiles_vreg 1_20)
    elseif(TILES_CLK_KHZ EQUAL 288000)
        set(tiles_vco 864000000)
        set(tiles_pd1 3)
        set(tiles_pd2 1)
        set(tiles_vreg 1_20)
    else()
        message(FATAL_ERROR "TILES_CLK_KHZ=${TILES_CLK_KHZ}: not one of the steps")
    endif()
    if(TILES_VREG)
        set(tiles_vreg ${TILES_VREG})
    endif()
    message(STATUS "System clock ${TILES_CLK_KHZ} kHz, core VREG_VOLTAGE_${tiles_vreg}")
    target_compile_definitions(sentia_tiles_firmware PRIVATE
        SYS_CLK_KHZ=${TILES_CLK_KHZ}
        PLL_SYS_VCO_FREQ_HZ=${tiles_vco}
        PLL_SYS_POSTDIV1=${tiles_pd1}
        PLL_SYS_POSTDIV2=${tiles_pd2}
        SYS_CLK_VREG_VOLTAGE_AUTO_ADJUST=1
        SYS_CLK_VREG_VOLTAGE_MIN=VREG_VOLTAGE_${tiles_vreg}
        TILES_VREG_VOLTAGE=VREG_VOLTAGE_${tiles_vreg})
    if(TILES_CLK_KHZ GREATER 160000)
        pico_define_boot_stage2(tiles_boot2 ${PICO_DEFAULT_BOOT_STAGE2_FILE})
        target_compile_definitions(tiles_boot2 PRIVATE PICO_FLASH_SPI_CLKDIV=3 PICO_FLASH_SPI_RXDELAY=3)
        pico_set_boot_stage2(sentia_tiles_firmware tiles_boot2)
    endif()
endif()

# Core 1's USB path and the engine's note handling run from RAM
# (link/default_text_excludes.incl says why).
pico_add_linker_script_override_path(sentia_tiles_firmware ${CMAKE_CURRENT_LIST_DIR}/link GLOB_FILES)

# The USB stack runs from RAM (link/), where its size matters more than
# the last of its speed: compiled for size, a tud_task() is still tens of us.
set(TILES_USB_SIZE_SOURCES
    ${PICO_SDK_PATH}/lib/tinyusb/src/tusb.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/common/tusb_fifo.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/device/usbd.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/device/usbd_control.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/class/audio/audio_device.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/class/cdc/cdc_device.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/class/midi/midi_device.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/class/vendor/vendor_device.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/portable/raspberrypi/rp2040/dcd_rp2040.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/portable/raspberrypi/rp2040/rp2040_usb.c
    ${CMAKE_CURRENT_LIST_DIR}/midi/usb_descriptors.c
    ${CMAKE_CURRENT_LIST_DIR}/midi/usb_device.c)
set_source_files_properties(${TILES_USB_SIZE_SOURCES} TARGET_DIRECTORY sentia_tiles_firmware
    PROPERTIES COMPILE_OPTIONS "-Os")

# Core 1's audio code: on the Cortex-M33 a fused multiply-add takes 3
# cycles against 2 for a multiply then an add (measured: the ladder 81 -> 67
# cycles a sample), so GCC mustn't fuse them.
set_source_files_properties(audio/synth_voice.c audio/fm_voice.c audio/audio_engine.c audio/sampler.c audio/fx.c
    PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
