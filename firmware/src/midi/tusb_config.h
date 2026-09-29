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
/* Real feedback: "we need the control software" -- the third composite
 * interface, alongside CDC (this file's own diagnostics console) and
 * MIDI: a separate USB vendor interface for firmware/src/usb_vendor.c's
 * own settings protocol (see that file's own header comment), matching
 * companion-app/README.md's already-documented intent ("Talks to the
 * device over the USB vendor interface"). Deliberately NOT the same
 * pipe as CDC -- that stays the human debug console; this is the
 * structured control-software channel, kept separate per docs/protocol/
 * README.md's own framing. */
#define CFG_TUD_VENDOR 1

/* CDC FIFO/endpoint buffer sizes. TX is 1 KB, not pico_stdio_usb's 64
 * bytes: a whole burst of console output (a mode change, a settings dump)
 * fits without printf() ever waiting for the host -- see the
 * PICO_STDIO_USB_STDOUT_TIMEOUT_US note in firmware/src/CMakeLists.txt. */
#define CFG_TUD_CDC_RX_BUFSIZE 64
#define CFG_TUD_CDC_TX_BUFSIZE 1024
#define CFG_TUD_CDC_EP_BUFSIZE 64

/* MIDI FIFO sizes -- full-speed only on RP2350, so the smaller size
 * always applies; matches TinyUSB's own midi_test example default. */
#define CFG_TUD_MIDI_RX_BUFSIZE 64
#define CFG_TUD_MIDI_TX_BUFSIZE 64

/* Vendor FIFO sizes -- this protocol is a line at a time (see usb_
 * vendor.c's own header comment), well under 64 bytes per command or
 * response, so the same small default every other class here already
 * uses is plenty. */
#define CFG_TUD_VENDOR_RX_BUFSIZE 64
#define CFG_TUD_VENDOR_TX_BUFSIZE 64

/* pico-sdk's own USB reset interface (pico_usb_reset, linked in
 * CMakeLists.txt) -- a fourth composite interface, control-transfers
 * only (no data endpoints), that lets `picotool` reboot this board into
 * BOOTSEL over USB instead of a physical button. Real feedback: "will
 * we be able to flash updates without putting the board in bootloader
 * mode" -> "yes add the software reboot command."
 *
 * `picotool load -f ...` (or `reboot -u`) already knows this interface
 * (class 0xFF, subclass 0x00, protocol 0x01, added to the descriptor in
 * usb_descriptors.c) without any picotool-side configuration -- this is
 * the exact mechanism pico_stdio_usb itself would set up automatically
 * if this codebase used its descriptors instead of its own (see this
 * file's own header comment on why it doesn't). Every other #define
 * this needs (PICO_USB_RESET_SUPPORT_RESET_TO_BOOTSEL/_FLASH_BOOT,
 * PICO_USB_RESET_INCLUDE_DEFAULT_APP_DRIVER_CB) already defaults to the
 * right value for this build (confirmed against
 * src/rp2_common/pico_usb_reset/include/pico/usb_reset_config.h) --
 * spelled out here anyway so this isn't relying on an unstated SDK
 * default. PICO_ENABLE_USB_RESET_VIA_BAUD_RATE stays OFF (its own
 * default is already 0 for a project linking tinyusb_device directly,
 * like this one) -- the CDC "magic baud rate" reset trick is a
 * pico_stdio_usb-only convenience this composite device doesn't need a
 * second reset path for. */
#define PICO_ENABLE_USB_RESET_VIA_VENDOR_INTERFACE 1
#define PICO_USB_RESET_SUPPORT_RESET_TO_BOOTSEL 1
#define PICO_USB_RESET_SUPPORT_RESET_TO_FLASH_BOOT 1
#define PICO_USB_RESET_INCLUDE_DEFAULT_APP_DRIVER_CB 1
