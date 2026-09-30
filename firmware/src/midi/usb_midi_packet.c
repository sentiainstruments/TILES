#include "usb_midi_packet.h"

#include <stdbool.h>

/* Code Index Numbers (USB MIDI 1.0, Table 4-1). */
#define CIN_SYSCOMMON_2 0x2u
#define CIN_SYSCOMMON_3 0x3u
#define CIN_SYSEX_START 0x4u /* SysEx starts or continues: 3 bytes */
#define CIN_SYSEX_END_1 0x5u /* SysEx ends with 1 byte -- also a 1-byte System Common */
#define CIN_SINGLE_BYTE 0xFu

static const uint8_t CIN_LENGTH[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};

uint8_t tiles_usb_midi_cin_length(uint8_t cin) {
    return CIN_LENGTH[cin & 0x0Fu];
}

static void put(uint8_t packet[4], uint8_t cable, uint8_t cin, const uint8_t *bytes, size_t n) {
    packet[0] = (uint8_t)((cable << 4) | cin);
    for (size_t i = 0; i < 3u; i++) {
        packet[1u + i] = i < n ? bytes[i] : 0u;
    }
}

static size_t pack_sysex(uint8_t cable, const uint8_t *msg, size_t len, uint8_t packets[][4], size_t max_packets) {
    if (len < 2u || msg[len - 1u] != 0xF7u) {
        return 0u;
    }
    for (size_t i = 1u; i + 1u < len; i++) {
        if (msg[i] >= 0x80u) {
            return 0u; /* only data bytes between F0 and F7 */
        }
    }
    size_t count = (len + 2u) / 3u;
    if (count > max_packets) {
        return 0u;
    }
    size_t pos = 0u;
    for (size_t p = 0u; p < count; p++) {
        size_t remaining = len - pos;
        if (remaining > 3u) {
            put(packets[p], cable, CIN_SYSEX_START, msg + pos, 3u);
            pos += 3u;
        } else {
            put(packets[p], cable, (uint8_t)(CIN_SYSEX_END_1 + remaining - 1u), msg + pos, remaining);
            pos += remaining;
        }
    }
    return count;
}

size_t tiles_usb_midi_pack(uint8_t cable, const uint8_t *msg, size_t len, uint8_t packets[][4], size_t max_packets) {
    if (len == 0u || max_packets == 0u || cable > 0x0Fu || msg[0] < 0x80u) {
        return 0u;
    }
    uint8_t status = msg[0];
    if (status == 0xF0u) {
        return pack_sysex(cable, msg, len, packets, max_packets);
    }
    uint8_t cin;
    size_t expected;
    if (status < 0xF0u) {
        cin = (uint8_t)(status >> 4);
        expected = CIN_LENGTH[cin];
    } else if (status >= 0xF8u) {
        cin = CIN_SINGLE_BYTE;
        expected = 1u;
    } else if (status == 0xF2u) {
        cin = CIN_SYSCOMMON_3;
        expected = 3u;
    } else if (status == 0xF1u || status == 0xF3u) {
        cin = CIN_SYSCOMMON_2;
        expected = 2u;
    } else if (status == 0xF6u) {
        cin = CIN_SYSEX_END_1;
        expected = 1u;
    } else {
        return 0u; /* 0xF4/0xF5 undefined; a lone 0xF7 isn't a message */
    }
    if (len != expected) {
        return 0u;
    }
    for (size_t i = 1u; i < len; i++) {
        if (msg[i] >= 0x80u) {
            return 0u;
        }
    }
    put(packets[0], cable, cin, msg, len);
    return 1u;
}
