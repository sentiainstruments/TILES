/* midi/usb_midi_packet.c -- USB-MIDI 1.0 event packets (cable number + Code Index Number). */
#include "usb_midi_packet.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t p[TILES_USB_MIDI_MAX_PACKETS][4];

static void expect(size_t i, uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) {
    uint8_t want[4] = {b0, b1, b2, b3};
    assert(memcmp(p[i], want, 4) == 0);
}

#define PACK(cable, ...)                                                    \
    ({                                                                      \
        uint8_t _m[] = {__VA_ARGS__};                                       \
        tiles_usb_midi_pack(cable, _m, sizeof _m, p, TILES_USB_MIDI_MAX_PACKETS); \
    })

int main(void) {
    /* 1. channel messages: CIN = status high nibble, cable in the high nibble of byte 0 */
    assert(PACK(0, 0x91, 60, 100) == 1);
    expect(0, 0x09, 0x91, 60, 100);
    assert(PACK(1, 0xB0, 105, 127) == 1); /* a Scene Launch CC on the DAW cable */
    expect(0, 0x1B, 0xB0, 105, 127);
    assert(PACK(0, 0xD3, 42) == 1); /* Channel Pressure: 2 bytes, padded with 0 */
    expect(0, 0x0D, 0xD3, 42, 0);
    assert(PACK(0, 0xE1, 0x00, 0x40) == 1);
    expect(0, 0x0E, 0xE1, 0x00, 0x40);

    /* 2. Real-Time and System Common */
    assert(PACK(0, 0xFA) == 1);
    expect(0, 0x0F, 0xFA, 0, 0);
    assert(PACK(0, 0xF2, 0x10, 0x20) == 1);
    expect(0, 0x03, 0xF2, 0x10, 0x20);
    assert(PACK(0, 0xF3, 0x05) == 1);
    expect(0, 0x02, 0xF3, 0x05, 0);

    /* 3. SysEx: 3-byte chunks (CIN 4), then an end packet whose CIN says how many bytes */
    assert(PACK(1, 0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7) == 2); /* 6 bytes: 3 + 3 */
    expect(0, 0x14, 0xF0, 0x7E, 0x7F);
    expect(1, 0x17, 0x06, 0x01, 0xF7);
    assert(PACK(0, 0xF0, 0x7D, 0x01, 0x12, 0xF7) == 2); /* 5 bytes: 3 + 2 */
    expect(1, 0x06, 0x12, 0xF7, 0);
    assert(PACK(0, 0xF0, 0x7D, 0x01, 0xF7) == 2); /* 4 bytes: 3 + 1 */
    expect(1, 0x05, 0xF7, 0, 0);
    assert(PACK(0, 0xF0, 0x01, 0xF7) == 1); /* 3 bytes: one end packet */
    expect(0, 0x07, 0xF0, 0x01, 0xF7);
    assert(PACK(0, 0xF0, 0xF7) == 1);
    expect(0, 0x06, 0xF0, 0xF7, 0);

    /* 4. the identity reply fits (15 bytes -> 5 packets) */
    assert(PACK(0, 0xF0, 0x7E, 0x7F, 0x06, 0x02, 0x7D, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0xF7) == 5);

    /* 5. anything that isn't exactly one complete message is refused */
    assert(PACK(0, 0x90, 60) == 0);            /* Note-On missing velocity */
    assert(PACK(0, 0x90, 60, 100, 5) == 0);    /* extra byte */
    assert(PACK(0, 0x3C, 100) == 0);           /* no status byte */
    assert(PACK(0, 0x90, 0x80, 100) == 0);     /* status byte in the data */
    assert(PACK(0, 0xF0, 0x7D, 0x01) == 0);    /* unterminated SysEx */
    assert(PACK(0, 0xF0, 0x7D, 0x90, 0xF7) == 0); /* status byte inside SysEx */
    assert(PACK(0, 0xF7) == 0);
    assert(PACK(0, 0xF4) == 0);
    assert(PACK(16, 0x90, 60, 100) == 0);      /* no cable 16 */
    uint8_t big[40] = {0xF0};
    big[39] = 0xF7;
    assert(tiles_usb_midi_pack(0, big, sizeof big, p, TILES_USB_MIDI_MAX_PACKETS) == 0); /* 14 packets > 12 */

    /* 6. receive side: bytes per CIN */
    assert(tiles_usb_midi_cin_length(0x9) == 3 && tiles_usb_midi_cin_length(0xC) == 2 &&
           tiles_usb_midi_cin_length(0xD) == 2 && tiles_usb_midi_cin_length(0xF) == 1);
    assert(tiles_usb_midi_cin_length(0x4) == 3 && tiles_usb_midi_cin_length(0x5) == 1 &&
           tiles_usb_midi_cin_length(0x6) == 2 && tiles_usb_midi_cin_length(0x7) == 3);
    assert(tiles_usb_midi_cin_length(0x0) == 0 && tiles_usb_midi_cin_length(0x1) == 0);
    assert(tiles_usb_midi_cin_length(0x19) == 3); /* only the low nibble counts */

    printf("usb_midi_packet: all tests pass\n");
    return 0;
}
