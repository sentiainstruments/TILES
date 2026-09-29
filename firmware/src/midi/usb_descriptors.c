/*
 * Composite USB device descriptors: CDC (the diagnostics console
 * everything so far has used) + MIDI + Vendor (firmware/src/usb_
 * vendor.c's own control-software settings protocol -- see that
 * file's own header comment), in one device.
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
#include "pico/usb_reset.h"
#include "product_identity.h"
#include "tusb.h"

/* Full-speed only: RP2350's USB controller has no high-speed PHY, so
 * there is no separate high-speed descriptor path to maintain here
 * (unlike the TinyUSB examples this is adapted from, which support both). */

/* VID/PID and firmware version: midi/product_identity.h -- including why
 * this is no longer Raspberry Pi's VID with a self-picked PID (that PID
 * belonged to someone else's product).
 *
 * USB 2.1 (not 2.0) so Windows asks for the BOS descriptor below, which is
 * how it learns -- without any driver install -- that the settings and
 * reset interfaces want Microsoft's generic WinUSB driver. */
#define USB_BCD 0x0210u

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

    .idVendor = TILES_USB_VID,
    .idProduct = TILES_USB_PID,
    .bcdDevice = TILES_USB_BCD_DEVICE,

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
    ITF_NUM_VENDOR,
    ITF_NUM_RESET, /* picotool's software-reboot interface -- see tusb_config.h */
    ITF_NUM_TOTAL,
};

#define EPNUM_CDC_NOTIF 0x81u
#define EPNUM_CDC_OUT 0x02u
#define EPNUM_CDC_IN 0x82u

#define EPNUM_MIDI_OUT 0x03u
#define EPNUM_MIDI_IN 0x83u

#define EPNUM_VENDOR_OUT 0x04u
#define EPNUM_VENDOR_IN 0x84u

#define CONFIG_TOTAL_LEN \
    (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MIDI_DESC_LEN + TUD_VENDOR_DESC_LEN + TUD_RPI_RESET_DESC_LEN)

uint8_t const desc_fs_configuration[] = {
    /* Config number, interface count, string index, total length, attribute, power in mA */
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    /* Interface number, string index, EP notification address + size, EP data (out, in) + size */
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),

    /* Interface number, string index, EP out & in address, EP size */
    TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, 5, EPNUM_MIDI_OUT, EPNUM_MIDI_IN, 64),

    /* Interface number, string index, EP out & in address, EP size */
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, 6, EPNUM_VENDOR_OUT, EPNUM_VENDOR_IN, 64),

    /* Control-transfers-only (no data endpoints, no string) -- see
     * tusb_config.h's own comment on why this interface exists. */
    TUD_RPI_RESET_DESCRIPTOR(ITF_NUM_RESET, 0),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_fs_configuration;
}

/* -------------------------------------------------------------------- */
/* BOS + Microsoft OS 2.0 descriptors (Windows driverless access)        */
/* -------------------------------------------------------------------- */

/* Real feedback: "do 8 as how standardized stuff works. production ready
 * industry stuff" (the standardization round). Windows binds a driver to
 * every interface of a composite device; it has class drivers for CDC and
 * MIDI, but NOT for a vendor-specific interface -- without this, the
 * settings interface (usb_vendor/, the future companion app) and the
 * picotool reset interface show up as "unknown device" and need a manual
 * driver install (Zadig). The Microsoft OS 2.0 descriptor set below asks
 * Windows to load its own WinUSB driver for exactly those two interfaces,
 * automatically -- the standard way, and the one pico-sdk itself uses for
 * the reset interface (pico_usb_reset/usb_reset.c, which this mirrors and
 * extends; that file's own copy is off in this build because this project
 * provides its own descriptors). macOS and Linux ignore all of this.
 * NOT hardware-tested on Windows yet. */

#define MS_OS_20_VENDOR_CODE 0x01u

/* Function subset for the settings interface: WinUSB + a device interface
 * GUID the companion app can look the device up by. Same layout as
 * pico-sdk's RPI_RESET_MS_OS_20_DESCRIPTOR (and so the same length);
 * the GUID is this project's own, generated for this interface. */
#define TILES_CONTROL_MS_OS_20_DESC_LEN (0x08 + 0x14 + 0x80)
#define TILES_CONTROL_MS_OS_20_DESCRIPTOR(itf_num)                                                                     \
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION), itf_num, 0,                                 \
        U16_TO_U8S_LE(TILES_CONTROL_MS_OS_20_DESC_LEN), U16_TO_U8S_LE(0x0014),                                         \
        U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID), 'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00, 0x00, 0x00, 0x00,       \
        0x00, 0x00, 0x00, 0x00, 0x00, U16_TO_U8S_LE(0x0080), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),             \
        U16_TO_U8S_LE(0x0001), U16_TO_U8S_LE(0x0028), 'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0, 'I', 0, 'n', 0, \
        't', 0, 'e', 0, 'r', 0, 'f', 0, 'a', 0, 'c', 0, 'e', 0, 'G', 0, 'U', 0, 'I', 0, 'D', 0, 0, 0,                  \
        U16_TO_U8S_LE(0x004E), /* {8cfdc7ad-aecd-411d-9610-df8c9929713c} */                                            \
        '{', 0, '8', 0, 'c', 0, 'f', 0, 'd', 0, 'c', 0, '7', 0, 'a', 0, 'd', 0, '-', 0, 'a', 0, 'e', 0, 'c', 0, 'd', 0, \
        '-', 0, '4', 0, '1', 0, '1', 0, 'd', 0, '-', 0, '9', 0, '6', 0, '1', 0, '0', 0, '-', 0, 'd', 0, 'f', 0, '8', 0, \
        'c', 0, '9', 0, '9', 0, '2', 0, '9', 0, '7', 0, '1', 0, '3', 0, 'c', 0, '}', 0, 0, 0

#define MS_OS_20_DESC_LEN (0x0A + TILES_CONTROL_MS_OS_20_DESC_LEN + RPI_RESET_MS_OS_20_DESC_LEN)

static uint8_t const desc_ms_os_20[] = {
    /* Set header: length, type, Windows version (8.1+), total length */
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR), U32_TO_U8S_LE(0x06030000),
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN),
    TILES_CONTROL_MS_OS_20_DESCRIPTOR(ITF_NUM_VENDOR),
    RPI_RESET_MS_OS_20_DESCRIPTOR(ITF_NUM_RESET),
};
TU_VERIFY_STATIC(sizeof(desc_ms_os_20) == MS_OS_20_DESC_LEN, "MS OS 2.0 descriptor set length");

#define BOS_TOTAL_LEN (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)

static uint8_t const desc_bos[] = {
    TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, MS_OS_20_VENDOR_CODE),
};
TU_VERIFY_STATIC(sizeof(desc_bos) == BOS_TOTAL_LEN, "BOS descriptor length");

uint8_t const *tud_descriptor_bos_cb(void) {
    return desc_bos;
}

/* Every vendor-type control request lands here (TinyUSB routes them before
 * any class driver) -- the only one this device answers is Windows' "get
 * the MS OS 2.0 descriptor set" (bRequest = the vendor code advertised in
 * the BOS above, wIndex 7). Anything else stalls. The picotool reset
 * request is a CLASS request to its interface, so it never comes here. */
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }
    if (request->bRequest == MS_OS_20_VENDOR_CODE && request->wIndex == 7u) {
        return tud_control_xfer(rhport, request, (void *)(uintptr_t)desc_ms_os_20, sizeof(desc_ms_os_20));
    }
    return false;
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
    STRID_VENDOR,
};

static char const *string_desc_arr[] = {
    NULL, /* 0: language ID, handled specially below */
    "SENTIA Instruments",
    NULL, /* 2: product, built from unit_id.h's TILES_UNIT_NUMBER/COUNT below */
    NULL, /* 3: serial, filled from the RP2350's unique flash ID below */
    "SENTIA TILES Diagnostics",
    "SENTIA TILES MIDI",
    "SENTIA TILES Control",
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
