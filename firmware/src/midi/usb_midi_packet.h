#pragma once

/*
 * USB-MIDI 1.0 event packets -- the 4-byte unit a USB-MIDI device actually
 * sends and receives (USB Device Class Definition for MIDI Devices 1.0,
 * section 4): byte 0 = cable number (high nibble) + Code Index Number (low
 * nibble, what kind of message), bytes 1-3 = up to three MIDI bytes. Pure
 * functions, no TinyUSB, tested natively (firmware/test/test_usb_midi_
 * packet.c).
 *
 * Why this file exists instead of TinyUSB's own byte-stream calls
 * (tud_midi_stream_write()/_read()): with two ports (midi/midi_ports.h)
 * the stream API isn't safe -- stream_read() merges every cable's bytes
 * into one stream, and stream_write() keeps ONE partial-message state for
 * all cables, so a message cut short on one cable would be finished on
 * the other. Packets carry their own cable number and are written whole.
 */

#include <stddef.h>
#include <stdint.h>

/* The largest message tiles_usb_midi_pack() is ever asked to pack is a
 * SysEx frame; 32 bytes (midi_out.c's SYSEX_SEND_BUF_MAX) is 11 packets. */
#define TILES_USB_MIDI_MAX_PACKETS 12u

/* Packs ONE complete MIDI message -- a channel message (status + its data
 * bytes), a System Common message, a single Real-Time byte, or a whole
 * SysEx frame (0xF0 ... 0xF7) -- into packets for `cable` (0-15). Returns
 * the number of packets written to `packets`, or 0 if `msg` isn't one
 * complete message or needs more than `max_packets`. */
size_t tiles_usb_midi_pack(uint8_t cable, const uint8_t *msg, size_t len, uint8_t packets[][4], size_t max_packets);

/* How many of a received packet's bytes 1-3 are MIDI data, from its Code
 * Index Number (packet[0] & 0x0F). 0 for the reserved CINs 0x0/0x1. */
uint8_t tiles_usb_midi_cin_length(uint8_t cin);
