#pragma once

/* MIDI input: the ONE reader of USB MIDI (both cables) and DIN. Two
 * readers on the same FIFO would race for bytes, so everything else
 * (services/midi_clock.c, Scene Launch, the melodic echo, identity)
 * registers callbacks here.
 *
 * Parses Real-Time bytes, SysEx frames (0xF0 ... 0xF7) and Note-On/Off,
 * with running status. Other channel-voice messages (CC, Program Change,
 * pressure, pitch bend) are consumed to keep alignment but not dispatched;
 * nothing needs them yet. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "midi_ports.h"

void tiles_midi_in_init(void);

/* Reads and parses everything available from both USB cables and DIN,
 * firing callbacks as messages complete. Call every main-loop pass, after
 * tud_task() and BEFORE tiles_midi_clock_scan() (which reacts to these
 * callbacks). */
void tiles_midi_in_scan(void);

/* Fired per Real-Time byte (0xF8 Clock, 0xFA Start, 0xFB Continue, 0xFC
 * Stop), synchronously inside tiles_midi_in_scan(). `now_ms` is read once
 * per scan. */
typedef void (*tiles_midi_in_realtime_callback_t)(uint8_t realtime_byte, uint32_t now_ms);

/* Fired per complete SysEx frame (the bytes between 0xF0 and 0xF7).
 * `data` is valid only during the callback. Oversized frames are dropped,
 * never truncated (MIDI_IN_SYSEX_MAX). `port` is where it arrived. */
typedef void (*tiles_midi_in_sysex_callback_t)(tiles_midi_port_t port, const uint8_t *data, size_t len);

/* Fired per Note-On/Off, running-status aware. `channel` is the raw nibble
 * 0-15. Note-On with velocity 0 and real Note-Off (0x8n) both arrive as
 * note_on=false (velocity 0). From every port: the melodic echo's notes
 * usually arrive on the DAW port (from the TILES DISPLAY device via the
 * control surface script); a MIDI track routed to TILES arrives on MAIN. */
typedef void (*tiles_midi_in_note_callback_t)(uint8_t channel, uint8_t note, uint8_t velocity, bool note_on,
                                               uint32_t now_ms);

/* Up to TILES_MIDI_IN_MAX_CALLBACKS listeners per kind, no allocation.
 * False if the table is full. */
#define TILES_MIDI_IN_MAX_CALLBACKS 4u
bool tiles_midi_in_register_realtime_callback(tiles_midi_in_realtime_callback_t callback);
bool tiles_midi_in_register_sysex_callback(tiles_midi_in_sysex_callback_t callback);
bool tiles_midi_in_register_note_callback(tiles_midi_in_note_callback_t callback);

/* Count of MEANINGFUL events received: Note-On/Off and Real-Time
 * Clock/Start/Continue/Stop, i.e. a DAW playing or an echo running. Not
 * SysEx (Scene Launch feedback arrives whenever Live is open), stray data
 * or Active Sensing. services/standby.c compares it with its last value
 * to keep the screensaver off during playback. Wraps harmlessly. */
uint32_t tiles_midi_in_activity_count(void);
