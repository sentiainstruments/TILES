/* Composite USB device: CDC console + MIDI (three cables) + vendor
 * (settings) + picotool reset + audio input, built with TinyUSB's
 * descriptor macros.
 * Replaces pico_stdio_usb's descriptors (tinyusb_device is linked). */

#include <stdio.h>
#include <string.h>

#include "board/unit_id.h"
#include "midi_ports.h"
#include "pico/unique_id.h"
#include "pico/usb_reset.h"
#include "product_identity.h"
#include "tusb.h"
#include "usb_audio.h"

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
    /* Audio last, so the interfaces above keep their numbers. */
    ITF_NUM_AUDIO_CONTROL,
    ITF_NUM_AUDIO_STREAMING,
    ITF_NUM_TOTAL,
};

#define EPNUM_CDC_NOTIF 0x81u
#define EPNUM_CDC_OUT 0x02u
#define EPNUM_CDC_IN 0x82u

#define EPNUM_MIDI_OUT 0x03u
#define EPNUM_MIDI_IN 0x83u

#define EPNUM_VENDOR_OUT 0x04u
#define EPNUM_VENDOR_IN 0x84u

#define EPNUM_AUDIO_IN 0x85u

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
    STRID_MIDI_PORT_PARTS,
};

/* Three MIDI ports (cables) on one USB-MIDI interface; see
 * midi/midi_ports.h. Built from TinyUSB's per-cable macros: one
 * embedded+external jack pair per cable, each named (the jack string is
 * the port name hosts show), then each endpoint lists its embedded jacks
 * in cable order, which defines the cable numbers. */
#define MIDI_NUM_CABLES 3u
#define TILES_MIDI_DESC_LEN \
    (TUD_MIDI_DESC_HEAD_LEN + MIDI_NUM_CABLES * TUD_MIDI_DESC_JACK_LEN + 2u * TUD_MIDI_DESC_EP_LEN(MIDI_NUM_CABLES))
TU_VERIFY_STATIC(MIDI_NUM_CABLES == TILES_USB_MIDI_NUM_CABLES, "descriptor cable count vs midi_ports.h");

#define CONFIG_TOTAL_LEN                                                                                     \
    (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TILES_MIDI_DESC_LEN + TUD_VENDOR_DESC_LEN + TUD_RPI_RESET_DESC_LEN + \
     TILES_AUDIO_FUNC_DESC_LEN)

TU_VERIFY_STATIC(CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX == TILES_AUDIO_EP_SIZE, "tusb_config.h vs usb_audio.h");
TU_VERIFY_STATIC(CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX == TILES_AUDIO_BYTES_PER_SAMPLE, "sample size");
TU_VERIFY_STATIC(CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX == TILES_AUDIO_CHANNELS, "channel count");

#define AUDIO_STEREO (AUDIO_CHANNEL_CONFIG_FRONT_LEFT | AUDIO_CHANNEL_CONFIG_FRONT_RIGHT)
/* UAC2 terminal type "Synthesizer" (embedded function); TinyUSB has no name for it. */
#define AUDIO_TERM_TYPE_SYNTHESIZER 0x0713u

uint8_t const desc_fs_configuration[] = {
    /* config number, interface count, string index, total length, attributes, mA */
    /* 500 mA, the USB 2.0 maximum: TILES draws ~0.15 A idle and up to
     * ~0.42 A on USB (docs/hardware/current-measurements.md), so the old
     * 100 mA undersold it to hubs and hosts. */
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 500),

    /* interface, string, notification EP + size, data EPs (out, in) + size */
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),

    /* MIDI: cable 0 = MAIN ("MIDI"), cable 1 = DAW ("DAW"), cable 2 = PARTS
     * ("Parts"). TinyUSB's jack macros count cables from 1. */
    TUD_MIDI_DESC_HEAD(ITF_NUM_MIDI, STRID_MIDI, MIDI_NUM_CABLES),
    TUD_MIDI_DESC_JACK_DESC(1, STRID_MIDI_PORT_MAIN),
    TUD_MIDI_DESC_JACK_DESC(2, STRID_MIDI_PORT_DAW),
    TUD_MIDI_DESC_JACK_DESC(3, STRID_MIDI_PORT_PARTS),
    TUD_MIDI_DESC_EP(EPNUM_MIDI_OUT, 64, MIDI_NUM_CABLES),
    TUD_MIDI_JACKID_IN_EMB(1),
    TUD_MIDI_JACKID_IN_EMB(2),
    TUD_MIDI_JACKID_IN_EMB(3),
    TUD_MIDI_DESC_EP(EPNUM_MIDI_IN, 64, MIDI_NUM_CABLES),
    TUD_MIDI_JACKID_OUT_EMB(1),
    TUD_MIDI_JACKID_OUT_EMB(2),
    TUD_MIDI_JACKID_OUT_EMB(3),

    /* interface, string, EP out & in, EP size */
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, STRID_VENDOR, EPNUM_VENDOR_OUT, EPNUM_VENDOR_IN, 64),

    /* Control transfers only (no endpoints, no string); see tusb_config.h. */
    TUD_RPI_RESET_DESCRIPTOR(ITF_NUM_RESET, 0),

    /* USB audio (UAC2): the test voice -> USB streaming, on a fixed 48 kHz
     * clock. Alt 1 streams stereo 24-bit in 4-byte subslots. */
    TUD_AUDIO_DESC_IAD(ITF_NUM_AUDIO_CONTROL, 2, 0),
    TUD_AUDIO_DESC_STD_AC(ITF_NUM_AUDIO_CONTROL, 0, 0),
    TUD_AUDIO_DESC_CS_AC(0x0200, AUDIO_FUNC_MUSICAL_INSTRUMENT,
                         TUD_AUDIO_DESC_CLK_SRC_LEN + TUD_AUDIO_DESC_INPUT_TERM_LEN + TUD_AUDIO_DESC_OUTPUT_TERM_LEN, 0),
    TUD_AUDIO_DESC_CLK_SRC(TILES_AUDIO_ENTITY_CLOCK, AUDIO_CLOCK_SOURCE_ATT_INT_FIX_CLK,
                           (AUDIO_CTRL_R << AUDIO_CLOCK_SOURCE_CTRL_CLK_FRQ_POS) |
                               (AUDIO_CTRL_R << AUDIO_CLOCK_SOURCE_CTRL_CLK_VAL_POS),
                           0, 0),
    TUD_AUDIO_DESC_INPUT_TERM(TILES_AUDIO_ENTITY_INPUT, AUDIO_TERM_TYPE_SYNTHESIZER, 0, TILES_AUDIO_ENTITY_CLOCK,
                              TILES_AUDIO_CHANNELS, AUDIO_STEREO, 0, 0, 0),
    TUD_AUDIO_DESC_OUTPUT_TERM(TILES_AUDIO_ENTITY_OUTPUT, AUDIO_TERM_TYPE_USB_STREAMING, 0, TILES_AUDIO_ENTITY_INPUT,
                               TILES_AUDIO_ENTITY_CLOCK, 0, 0),
    TUD_AUDIO_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING, 0, 0, 0),
    TUD_AUDIO_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING, 1, 1, 0),
    TUD_AUDIO_DESC_CS_AS_INT(TILES_AUDIO_ENTITY_OUTPUT, AUDIO_CTRL_NONE, AUDIO_FORMAT_TYPE_I,
                             AUDIO_DATA_FORMAT_TYPE_I_PCM, TILES_AUDIO_CHANNELS, AUDIO_STEREO, 0),
    TUD_AUDIO_DESC_TYPE_I_FORMAT(TILES_AUDIO_BYTES_PER_SAMPLE, TILES_AUDIO_BITS_PER_SAMPLE),
    TUD_AUDIO_DESC_STD_AS_ISO_EP(EPNUM_AUDIO_IN,
                                 (uint8_t)(TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ASYNCHRONOUS | TUSB_ISO_EP_ATT_DATA),
                                 TILES_AUDIO_EP_SIZE, 1),
    TUD_AUDIO_DESC_CS_AS_ISO_EP(AUDIO_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, AUDIO_CTRL_NONE,
                                AUDIO_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0),
};
TU_VERIFY_STATIC(sizeof(desc_fs_configuration) == CONFIG_TOTAL_LEN, "configuration descriptor length");

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_fs_configuration;
}

/* ---- BOS + Microsoft OS 2.0 descriptors (Windows driverless access) ---- */

/* MS OS 2.0 descriptors so Windows loads WinUSB for the vendor and reset
 * interfaces (no Zadig), as pico-sdk does. Untested on Windows. */

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

/* The product name carries the unit number ("SENTIA TILES 2"), which DAWs
 * build port names from, so two units can't swap tracks after a replug.
 * Cost: a Live set doesn't find another unit by itself. */
static char const *string_desc_arr[] = {
    NULL, /* 0: language ID, handled below */
    "SENTIA Instruments",
    NULL, /* 2: product, built with the unit number */
    NULL, /* 3: serial, from the RP2350 unique ID */
    NULL, /* 4: diagnostics interface, built with the unit label */
    "SENTIA TILES MIDI",
    "SENTIA TILES Control",
    "MIDI",  /* MAIN port's jacks (midi/midi_ports.h) */
    "DAW",   /* DAW port's jacks */
    "Parts", /* PARTS port's jacks */
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
