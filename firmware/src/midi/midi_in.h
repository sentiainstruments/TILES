#pragma once

/*
 * Shared USB MIDI IN byte-stream parser. Real feedback: "lets implemebt
 * a new mode that triggers scenes in ableton live... can we pull the
 * colors of the scenes from ableton?" -- the scene-launch mode's own
 * "clip/scene state" feedback from Ableton is the first thing this
 * codebase has ever needed to actually READ from incoming MIDI beyond
 * System Real-Time bytes, so this file exists to become the ONE place
 * that owns tud_midi_stream_read() -- services/midi_clock.c used to call
 * it directly (see that file's own now-superseded header comment) and
 * silently discard every byte that wasn't one of the four Real-Time
 * bytes it cared about. Two independent readers draining the SAME shared
 * RX FIFO would race each other for bytes, so midi_clock.c was refactored
 * to register a callback here instead of reading MIDI itself -- see this
 * file's own tiles_midi_in_register_realtime_callback().
 *
 * Scope, deliberately narrow (matches midi_clock.c's own prior framing,
 * just widened by exactly one thing): Real-Time bytes (unchanged,
 * verbatim from what midi_clock.c used to do inline) PLUS SysEx frames
 * (0xF0 ... 0xF7), since that's what services/op_mode.c's new Scene
 * Launch mode needs for its own Ableton-Live-color-feedback protocol
 * (see that mode's own section in op_mode.c, and
 * daw-integration/ableton/TILES/scene_launch.py for the DAW side). Any
 * other MIDI message (Note-On/Off, CC, etc. arriving on USB MIDI IN) is
 * still silently discarded, same as before this file existed -- a full
 * running-status-aware channel-message parser is a separate, not-yet-
 * built feature, same "intentionally narrow" framing midi_clock.c's own
 * header already used.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void tiles_midi_in_init(void);

/* Drains and parses every byte currently available from USB MIDI IN,
 * firing registered callbacks for each complete Real-Time byte or SysEx
 * frame found. Call every main-loop iteration, after tud_task() (so
 * this iteration's USB RX FIFO is current) and BEFORE
 * tiles_midi_clock_scan() (which no longer reads MIDI itself -- it
 * reacts to this file's own callback instead, so it needs this to have
 * already run this same scan). */
void tiles_midi_in_scan(void);

/* Fired once per Real-Time byte (0xF8 Clock, 0xFA Start, 0xFB Continue,
 * 0xFC Stop) found, synchronously from within tiles_midi_in_scan() --
 * same timing guarantee services/midi_clock.c's own inline read loop
 * used to provide, just via a callback now. `now_ms` is captured once
 * per tiles_midi_in_scan() call, not re-read per byte. */
typedef void (*tiles_midi_in_realtime_callback_t)(uint8_t realtime_byte, uint32_t now_ms);

/* Fired once per complete SysEx frame (the bytes strictly BETWEEN 0xF0
 * and 0xF7, neither delimiter included), synchronously from within
 * tiles_midi_in_scan(). `data`/`len` point at an internal buffer valid
 * only for the duration of the callback -- copy out anything needed
 * past that. A frame longer than this file's own internal buffer is
 * silently dropped (never delivered, not truncated-and-delivered) --
 * see midi_in.c's own MIDI_IN_SYSEX_MAX comment. */
typedef void (*tiles_midi_in_sysex_callback_t)(const uint8_t *data, size_t len);

/* Up to TILES_MIDI_IN_MAX_CALLBACKS listeners each, same fixed-size-
 * table-no-dynamic-allocation shape services/power.h's own
 * tiles_power_register_callback() already established. Returns false
 * if the relevant table is full. */
#define TILES_MIDI_IN_MAX_CALLBACKS 4u
bool tiles_midi_in_register_realtime_callback(tiles_midi_in_realtime_callback_t callback);
bool tiles_midi_in_register_sysex_callback(tiles_midi_in_sysex_callback_t callback);
