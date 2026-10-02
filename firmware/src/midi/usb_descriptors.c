/* Composite USB device: CDC (diagnostics console) + MIDI (two cables) +
 * vendor (settings, usb_vendor/) + picotool reset interface.
 *
 * Built with TinyUSB's per-class descriptor macros (patterns from its
 * cdc_msc and midi_test examples) rather than hand-counted bytes.
 * Replaces pico_stdio_usb's default descriptors because CMakeLists.txt
 * links tinyusb_device explicitly (see tusb_config.h). */

#include <stdio.h>
#include <string.h>

#include "board/unit_id.h"
#include "midi_ports.h"
#include "pico/unique_id.h"
#include "pico/usb_reset.h"
#include "product_identity.h"
#include "tusb.h"

/* Full speed only: the RP2350 has no high-speed PHY. */

/* VID/PID and firmware version: midi/product_identity.h.
 *
 * USB 2.1 (not 2.0) so Windows asks for the BOS descriptor below and binds
 * WinUSB to the settings and reset interfaces without a driver install. */
#define USB_BCD 0x0210u

/* ---- Device descriptor ---- */

tusb_desc_device_t const desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = USB_BCD,

    /* IAD, required when CDC shares a configuration with other classes. */
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

/* ---- Configuration descriptor ---- */

enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_MIDI,
    ITF_NUM_MIDI_STREAMING,
    ITF_NUM_VENDOR,
    ITF_NUM_RESET, /* picotool reset interface; see tusb_config.h */
    ITF_NUM_TOTAL,
};

#define EPNUM_CDC_NOTIF 0x81u
#define EPNUM_CDC_OUT 0x02u
#define EPNUM_CDC_IN 0x82u

#define EPNUM_MIDI_OUT 0x03u
#define EPNUM_MIDI_IN 0x83u

#define EPNUM_VENDOR_OUT 0x04u
#define EPNUM_VENDOR_IN 0x84u

/* String indices; see string_desc_arr below. */
enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CDC,
    STRID_MIDI,
    STRID_VENDOR,
    STRID_MIDI_PORT_MAIN,
    STRID_MIDI_PORT_DAW,
};

/* Two MIDI ports (cables) on one USB-MIDI interface; see
 * midi/midi_ports.h. Built from TinyUSB's per-cable macros: one
 * embedded+external jack pair per cable, each named (the jack string is
 * the port name hosts show), then each endpoint lists its embedded jacks
 * in cable order, which defines the cable numbers. */
#define MIDI_NUM_CABLES 2u
#define TILES_MIDI_DESC_LEN \
    (TUD_MIDI_DESC_HEAD_LEN + MIDI_NUM_CABLES * TUD_MIDI_DESC_JACK_LEN + 2u * TUD_MIDI_DESC_EP_LEN(MIDI_NUM_CABLES))
TU_VERIFY_STATIC(MIDI_NUM_CABLES == TILES_USB_MIDI_NUM_CABLES, "descriptor cable count vs midi_ports.h");

#define CONFIG_TOTAL_LEN \
    (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TILES_MIDI_DESC_LEN + TUD_VENDOR_DESC_LEN + TUD_RPI_RESET_DESC_LEN)

uint8_t const desc_fs_configuration[] = {
    /* config number, interface count, string index, total length, attributes, mA */
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    /* interface, string, notification EP + size, data EPs (out, in) + size */
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),

    /* MIDI: cable 0 = MAIN ("MIDI"), cable 1 = DAW ("DAW"). TinyUSB's jack
     * macros count cables from 1. */
    TUD_MIDI_DESC_HEAD(ITF_NUM_MIDI, STRID_MIDI, MIDI_NUM_CABLES),
    TUD_MIDI_DESC_JACK_DESC(1, STRID_MIDI_PORT_MAIN),
    TUD_MIDI_DESC_JACK_DESC(2, STRID_MIDI_PORT_DAW),
    TUD_MIDI_DESC_EP(EPNUM_MIDI_OUT, 64, MIDI_NUM_CABLES),
    TUD_MIDI_JACKID_IN_EMB(1),
    TUD_MIDI_JACKID_IN_EMB(2),
    TUD_MIDI_DESC_EP(EPNUM_MIDI_IN, 64, MIDI_NUM_CABLES),
    TUD_MIDI_JACKID_OUT_EMB(1),
    TUD_MIDI_JACKID_OUT_EMB(2),

    /* interface, string, EP out & in, EP size */
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, STRID_VENDOR, EPNUM_VENDOR_OUT, EPNUM_VENDOR_IN, 64),

    /* Control transfers only (no endpoints, no string); see tusb_config.h. */
    TUD_RPI_RESET_DESCRIPTOR(ITF_NUM_RESET, 0),
};
TU_VERIFY_STATIC(sizeof(desc_fs_configuration) == CONFIG_TOTAL_LEN, "configuration descriptor length");

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_fs_configuration;
}

/* ---- BOS + Microsoft OS 2.0 descriptors (Windows driverless access) ---- */

/* Windows has class drivers for CDC and MIDI but not for vendor
 * interfaces, so without this the settings and reset interfaces show up as
 * "unknown device" and need Zadig. The MS OS 2.0 descriptor set asks
 * Windows to load WinUSB for those two interfaces automatically: the
 * standard approach, and the one pico-sdk uses for its reset interface
 * (pico_usb_reset/usb_reset.c, mirrored and extended here; its own copy is
 * off because we provide the descriptors). macOS and Linux ignore it.
 * Not yet tested on Windows hardware. */

#define MS_OS_20_VENDOR_CODE 0x01u

/* Settings interface subset: WinUSB plus a device interface GUID the
 * companion app can find the device by. Same layout (and length) as
 * pico-sdk's RPI_RESET_MS_OS_20_DESCRIPTOR; the GUID is ours. */
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
    /* set header: length, type, Windows version (8.1+), total length */
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

/* Vendor-type control requests arrive here before any class driver. The
 * only one answered is Windows' MS OS 2.0 descriptor request (bRequest =
 * our vendor code, wIndex 7); anything else stalls. The picotool reset
 * request is a class request, so it never comes here. */
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }
    if (request->bRequest == MS_OS_20_VENDOR_CODE && request->wIndex == 7u) {
        return tud_control_xfer(rhport, request, (void *)(uintptr_t)desc_ms_os_20, sizeof(desc_ms_os_20));
    }
    return false;
}

/* ---- String descriptors ---- */

/* The PRODUCT name carries the unit number ("SENTIA TILES 2"). Hosts show
 * it as the device and DAWs build the port names from it ("SENTIA TILES 2
 * MIDI" / "SENTIA TILES 2 DAW" on macOS, "SENTIA TILES 2 (MIDI)" in Live),
 * so two units played at once can't be mixed up. With one name for every
 * unit, Live numbers the second one "#2" in plug-in order, and the two
 * could swap tracks after a replug. The cost: a Live set made with one
 * unit doesn't find another by itself (pick its ports again). The unit
 * label also rides on the CDC interface name and the settings INFO reply;
 * the serial number is the chip ID. */
static char const *string_desc_arr[] = {
    NULL, /* 0: language ID, handled below */
    "SENTIA Instruments",
    NULL, /* 2: product, built with the unit number */
    NULL, /* 3: serial, from the RP2350 unique ID */
    NULL, /* 4: diagnostics interface, built with the unit label */
    "SENTIA TILES MIDI",
    "SENTIA TILES Control",
    "MIDI", /* MAIN port's jacks (midi/midi_ports.h) */
    "DAW",  /* DAW port's jacks */
};

static uint16_t desc_str[40 + 1];

/* Copies an ASCII string into desc_str, cut to fit; returns its length. */
static size_t desc_str_set(const char *str) {
    size_t chr_count = strlen(str);
    const size_t max_count = sizeof(desc_str) / sizeof(desc_str[0]) - 1u;
    if (chr_count > max_count) {
        chr_count = max_count;
    }
    for (size_t i = 0; i < chr_count; i++) {
        desc_str[1 + i] = (uint16_t)str[i];
    }
    return chr_count;
}

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
            char buf[24];
            snprintf(buf, sizeof(buf), "SENTIA TILES %u", (unsigned)TILES_UNIT_NUMBER);
            chr_count = desc_str_set(buf);
            break;
        }
        case STRID_CDC: {
            char buf[40];
            snprintf(buf, sizeof(buf), "SENTIA TILES Diagnostics (Unit %u/%u)", (unsigned)TILES_UNIT_NUMBER,
                     (unsigned)TILES_UNIT_COUNT);
            chr_count = desc_str_set(buf);
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
            chr_count = desc_str_set(string_desc_arr[index]);
            break;
        }
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2u * chr_count + 2u));
    return desc_str;
}
