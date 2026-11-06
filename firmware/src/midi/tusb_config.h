#pragma once

/* TinyUSB config for the composite CDC + MIDI + vendor device, modeled on
 * pico-sdk's (not TinyUSB's example). It wins over pico_stdio_usb's bundled
 * config because that one is a SYSTEM include, searched after ours. */

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

/* USB audio: one stereo 48 kHz / 24-bit input (audio/usb_audio.h; the
 * numbers are checked against it there). No flow control: each packet
 * carries what usb_audio.c writes, so a late one can be made up. */
#define CFG_TUD_AUDIO 1
#define TILES_AUDIO_FUNC_DESC_LEN                                                                                 \
    (TUD_AUDIO_DESC_IAD_LEN + TUD_AUDIO_DESC_STD_AC_LEN + TUD_AUDIO_DESC_CS_AC_LEN + TUD_AUDIO_DESC_CLK_SRC_LEN + \
     TUD_AUDIO_DESC_INPUT_TERM_LEN + TUD_AUDIO_DESC_OUTPUT_TERM_LEN + 2 * TUD_AUDIO_DESC_STD_AS_INT_LEN +         \
     TUD_AUDIO_DESC_CS_AS_INT_LEN + TUD_AUDIO_DESC_TYPE_I_FORMAT_LEN + TUD_AUDIO_DESC_STD_AS_ISO_EP_LEN +         \
     TUD_AUDIO_DESC_CS_AS_ISO_EP_LEN)
#define CFG_TUD_AUDIO_FUNC_1_DESC_LEN TILES_AUDIO_FUNC_DESC_LEN
#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT 1
#define CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ 64
#define CFG_TUD_AUDIO_ENABLE_EP_IN 1
#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX 4
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX 2
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX 768
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ (2 * 768)
#define CFG_TUD_AUDIO_EP_IN_FLOW_CONTROL 0

/* pico-sdk's USB reset interface, so picotool can reboot into BOOTSEL
 * without the button. Options spelled out as the SDK defaults; the CDC
 * magic-baud reset stays off. */
#define PICO_ENABLE_USB_RESET_VIA_VENDOR_INTERFACE 1
#define PICO_USB_RESET_SUPPORT_RESET_TO_BOOTSEL 1
#define PICO_USB_RESET_SUPPORT_RESET_TO_FLASH_BOOT 1
#define PICO_USB_RESET_INCLUDE_DEFAULT_APP_DRIVER_CB 1
