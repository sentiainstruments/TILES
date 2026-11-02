#pragma once

/* USB-MIDI 1.0 event packets (cable + CIN, then up to three bytes); pure,
 * tested natively. Used instead of TinyUSB's byte stream, which isn't
 * safe with two cables. */

#include <stddef.h>
#include <stdint.h>

/* The largest message packed is a SysEx frame: 32 bytes
 * (midi_out.c SYSEX_SEND_BUF_MAX) = 11 packets. */
#define TILES_USB_MIDI_MAX_PACKETS 12u

/* Packs ONE complete message (channel message, System Common, a single
 * Real-Time byte, or a whole SysEx frame F0...F7) for `cable` (0-15).
 * Returns the packet count, or 0 if `msg` isn't one complete message or
 * needs more than `max_packets`. */
size_t tiles_usb_midi_pack(uint8_t cable, const uint8_t *msg, size_t len, uint8_t packets[][4], size_t max_packets);

/* Number of MIDI bytes in a received packet, from its CIN
 * (packet[0] & 0x0F). 0 for the reserved CINs 0x0/0x1. */
uint8_t tiles_usb_midi_cin_length(uint8_t cin);
