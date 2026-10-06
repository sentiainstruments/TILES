#pragma once

/* TinyUSB configuration for the composite CDC + MIDI + vendor device.
 *
 * Modeled on pico-sdk's pico_stdio_usb/include/tusb_config.h, not
 * TinyUSB's example config, which redefines things (CFG_TUSB_MCU,
 * CFG_TUSB_OS, EP0 size, section macros) the SDK already provides.
 *
 * Must win over pico_stdio_usb's bundled config (CDC only): linking
 * tinyusb_device explicitly (src/CMakeLists.txt) makes pico_stdio_usb
 * leave TinyUSB init and descriptors to us, and its config directory is a
 * SYSTEM include, searched after ours. */

#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE)

#define CFG_TUD_CDC 1
#define CFG_TUD_MIDI 1
#define CFG_TUD_MSC 0
#define CFG_TUD_HID 0
/* Vendor interface for the settings protocol (usb_vendor/): the companion
 * app's structured channel, separate from the human CDC console. */
#define CFG_TUD_VENDOR 1

/* CDC buffers. TX is 1 KB (pico_stdio_usb uses 64) so a burst of console
 * output fits without printf() waiting on the host; see
 * PICO_STDIO_USB_STDOUT_TIMEOUT_US in src/CMakeLists.txt. */
#define CFG_TUD_CDC_RX_BUFSIZE 64
#define CFG_TUD_CDC_TX_BUFSIZE 1024
#define CFG_TUD_CDC_EP_BUFSIZE 64

/* MIDI FIFOs: 256 bytes = 64 packets each way (the endpoint stays 64
 * bytes). The zone declaration alone is 60 CCs, a panic 48, and Live's
 * clip-colour burst dozens of SysEx packets; they queue here instead of
 * stalling in midi_out.c's retry wait. */
#define CFG_TUD_MIDI_RX_BUFSIZE 256
#define CFG_TUD_MIDI_TX_BUFSIZE 256

/* Vendor FIFOs: commands are single short lines; replies are queued and
 * drained by usb_vendor.c. */
#define CFG_TUD_VENDOR_RX_BUFSIZE 64
#define CFG_TUD_VENDOR_TX_BUFSIZE 64

/* pico-sdk's USB reset interface (pico_usb_reset): control transfers only,
 * lets `picotool load -f` / `reboot -u` reboot the board into BOOTSEL
 * without the button (class 0xFF/0x00/0x01, added in usb_descriptors.c).
 * The options below match the SDK defaults for this build
 * (pico/usb_reset_config.h) and are spelled out so nothing relies on an
 * unstated default. The CDC "magic baud rate" reset stays off; one reset
 * path is enough. */
#define PICO_ENABLE_USB_RESET_VIA_VENDOR_INTERFACE 1
#define PICO_USB_RESET_SUPPORT_RESET_TO_BOOTSEL 1
#define PICO_USB_RESET_SUPPORT_RESET_TO_FLASH_BOOT 1
#define PICO_USB_RESET_INCLUDE_DEFAULT_APP_DRIVER_CB 1
