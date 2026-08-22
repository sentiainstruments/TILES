#pragma once

/*
 * TinyUSB compile-time configuration for SENTIA TILES' composite
 * CDC (diagnostics console) + MIDI device.
 *
 * Modeled on pico-sdk's own pico_stdio_usb/include/tusb_config.h
 * (the version proven to work with pico-sdk's CMake integration) rather
 * than TinyUSB's own generic example tusb_config.h, which targets
 * TinyUSB's separate example-project build system and sets several
 * things (CFG_TUSB_MCU, CFG_TUSB_OS, CFG_TUD_ENDPOINT0_SIZE, memory
 * section/alignment macros) that pico-sdk's tinyusb_device library
 * already provides via its own compile definitions -- redefining them
 * here would risk a mismatch with what the SDK actually built for.
 *
 * This file must be found ahead of pico_stdio_usb's own bundled
 * tusb_config.h (which only enables CDC) -- see firmware/src/CMakeLists.txt:
 * linking tinyusb_device explicitly makes pico_stdio_usb defer both
 * TinyUSB init and descriptor provision to us (LIB_TINYUSB_DEVICE
 * becomes defined), and pico_stdio_usb's own tusb_config.h is added as
 * a SYSTEM include directory, which the compiler searches after our
 * normal (non-system) include directories -- so ours wins.
 */

#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE)

#define CFG_TUD_CDC 1
#define CFG_TUD_MIDI 1
#define CFG_TUD_MSC 0
#define CFG_TUD_HID 0
#define CFG_TUD_VENDOR 0

/* CDC FIFO/endpoint buffer sizes -- same defaults pico_stdio_usb uses. */
#define CFG_TUD_CDC_RX_BUFSIZE 64
#define CFG_TUD_CDC_TX_BUFSIZE 64
#define CFG_TUD_CDC_EP_BUFSIZE 64

/* MIDI FIFO sizes -- full-speed only on RP2350, so the smaller size
 * always applies; matches TinyUSB's own midi_test example default. */
#define CFG_TUD_MIDI_RX_BUFSIZE 64
#define CFG_TUD_MIDI_TX_BUFSIZE 64
