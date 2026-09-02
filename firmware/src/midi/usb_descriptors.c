/*
 * Composite USB device descriptors: CDC (the diagnostics console
 * everything so far has used) + MIDI, in one device.
 *
 * Structure adapted from TinyUSB's own reference examples rather than
 * hand-built from scratch: the CDC+other-class composite pattern
 * (interface numbering, IAD device class, TUD_CDC_DESCRIPTOR usage)
 * from examples/device/cdc_msc/src/usb_descriptors.c, and the MIDI
 * interface descriptor (TUD_MIDI_DESCRIPTOR usage) from
 * examples/device/midi_test/src/usb_descriptors.c. Using TinyUSB's own
 * descriptor-building macros for each class, rather than hand-counting
 * descriptor bytes, is what keeps this from being the kind of thing
 * that's subtly wrong in a way that's painful to debug on real hardware.
 *
 * Compiles instead of pico_stdio_usb's bundled default descriptors
 * because firmware/src/CMakeLists.txt links tinyusb_device explicitly --
 * see tusb_config.h's header comment.
 */

#include <stdio.h>
#include <string.h>

#include "board/unit_id.h"
#include "pico/unique_id.h"
#include "tusb.h"

/* Full-speed only: RP2350's USB controller has no high-speed PHY, so
 * there is no separate high-speed descriptor path to maintain here
 * (unlike the TinyUSB examples this is adapted from, which support both). */

/* SENTIA has no registered USB-IF vendor ID of its own yet, so this
 * borrows Raspberry Pi Trading's VID (0x2E8A), same informal practice
 * pico-sdk's own default descriptors use for RP-based boards. The PID
 * is deliberately NOT pico-sdk's reserved single-CDC-only default
 * (0x0009 on non-RP2040 targets) -- this is a different interface
 * layout (composite CDC+MIDI), and reusing that PID risks a host OS
 * reapplying a driver association it cached for the plain-CDC layout. */
#define USB_VID 0x2E8Au
#define USB_PID 0x100Au
#define USB_BCD 0x0200u

/* -------------------------------------------------------------------- */
/* Device descriptor                                                     */
/* -------------------------------------------------------------------- */

tusb_desc_device_t const desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = USB_BCD,

    /* Interface Association Descriptor for CDC, required whenever CDC
     * is combined with another class in one configuration. */
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,

    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100u,

    .iManufacturer = 0x01u,
    .iProduct = 0x02u,
    .iSerialNumber = 0x03u,

    .bNumConfigurations = 0x01u,
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

/* -------------------------------------------------------------------- */
/* Configuration descriptor                                              */
/* -------------------------------------------------------------------- */

enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_MIDI,
    ITF_NUM_MIDI_STREAMING,
    ITF_NUM_TOTAL,
};

#define EPNUM_CDC_NOTIF 0x81u
#define EPNUM_CDC_OUT 0x02u
#define EPNUM_CDC_IN 0x82u

#define EPNUM_MIDI_OUT 0x03u
#define EPNUM_MIDI_IN 0x83u

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MIDI_DESC_LEN)

uint8_t const desc_fs_configuration[] = {
    /* Config number, interface count, string index, total length, attribute, power in mA */
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    /* Interface number, string index, EP notification address + size, EP data (out, in) + size */
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),

    /* Interface number, string index, EP out & in address, EP size */
    TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, 5, EPNUM_MIDI_OUT, EPNUM_MIDI_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_fs_configuration;
}

/* -------------------------------------------------------------------- */
/* String descriptors                                                    */
/* -------------------------------------------------------------------- */

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CDC,
    STRID_MIDI,
};

static char const *string_desc_arr[] = {
    NULL, /* 0: language ID, handled specially below */
    "SENTIA Instruments",
    NULL, /* 2: product, built from unit_id.h's TILES_UNIT_NUMBER/COUNT below */
    NULL, /* 3: serial, filled from the RP2350's unique flash ID below */
    "SENTIA TILES Diagnostics",
    "SENTIA TILES MIDI",
};

static uint16_t desc_str[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    size_t chr_count;

    switch (index) {
        case STRID_LANGID: {
            desc_str[1] = 0x0409u; /* English (US) */
            chr_count = 1;
            break;
        }
        case STRID_PRODUCT: {
            /* Real feedback: "were moving to have identifiers" -- see
             * unit_id.h's own header for the full reasoning. Built here
             * (rather than a plain string_desc_arr entry) so this is
             * visible without a serial terminal: `picotool info -a` and
             * the host OS's own USB device listing both show the
             * product string. */
            char buf[32];
            int written = snprintf(buf, sizeof(buf), "SENTIA TILES (Unit %u/%u)", (unsigned)TILES_UNIT_NUMBER,
                                    (unsigned)TILES_UNIT_COUNT);
            chr_count = (written > 0) ? (size_t)written : 0u;
            const size_t max_count = sizeof(desc_str) / sizeof(desc_str[0]) - 1u;
            if (chr_count > max_count) {
                chr_count = max_count;
            }
            for (size_t i = 0; i < chr_count; i++) {
                desc_str[1 + i] = (uint16_t)buf[i];
            }
            break;
        }
        case STRID_SERIAL: {
            pico_unique_board_id_t id;
            pico_get_unique_board_id(&id);
            chr_count = 0;
            for (size_t i = 0; i < sizeof(id.id) && chr_count < 32; i++) {
                static const char hex[] = "0123456789ABCDEF";
                desc_str[1 + chr_count++] = (uint16_t)hex[(id.id[i] >> 4) & 0x0Fu];
                desc_str[1 + chr_count++] = (uint16_t)hex[id.id[i] & 0x0Fu];
            }
            break;
        }
        default: {
            if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0]) || string_desc_arr[index] == NULL) {
                return NULL;
            }
            const char *str = string_desc_arr[index];
            chr_count = strlen(str);
            const size_t max_count = sizeof(desc_str) / sizeof(desc_str[0]) - 1u;
            if (chr_count > max_count) {
                chr_count = max_count;
            }
            for (size_t i = 0; i < chr_count; i++) {
                desc_str[1 + i] = (uint16_t)str[i];
            }
            break;
        }
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2u * chr_count + 2u));
    return desc_str;
}
