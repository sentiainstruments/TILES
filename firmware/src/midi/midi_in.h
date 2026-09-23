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
 * Scope, widened twice since (matches midi_clock.c's own prior framing,
 * each round adding exactly one thing): Real-Time bytes (unchanged,
 * verbatim from what midi_clock.c used to do inline), SysEx frames
 * (0xF0 ... 0xF7, for services/op_mode.c's Scene Launch mode -- see that
 * mode's own section in op_mode.c, and daw-integration/ableton/TILES/
 * scene_launch.py for the DAW side), and now Note-On/Off specifically.
 *
 * Real feedback: "in midi melodic mode is there any way we could read
 * the playing melody of the armed track and display it back on tiles?"
 * -- needs a real, running-status-aware channel-voice parser, the "not-
 * yet-built feature" this file's own header used to flag. Every other
 * channel-voice message type (CC, Program Change, Channel/Poly
 * Pressure, Pitch Bend) is still correctly CONSUMED (so running status
 * and byte alignment stay correct for whatever comes after it) but not
 * dispatched anywhere -- nothing needs those yet, same "intentionally
 * narrow" framing this file has always used, just widened by exactly
 * what real feedback actually asked for. */

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

/* Fired once per complete Note-On or Note-Off, synchronously from within
 * tiles_midi_in_scan(), running-status aware (a sender that omits a
 * repeated status byte between consecutive Note-Ons on the same channel
 * -- standard MIDI, common from DAW output -- is still parsed
 * correctly). `channel` is 0-15 (raw nibble, matching every other
 * channel value in this codebase -- see midi/midi_out.h's own
 * convention). `note_on` is true for a genuine Note-On (status 0x9n
 * with velocity > 0); a Note-On with velocity 0, or a real Note-Off
 * status (0x8n), both normalize to `note_on = false` here -- callers
 * never have to handle the zero-velocity convention themselves.
 * `velocity` is the raw 0-127 byte either way (0 for every note_on=false
 * case). `now_ms` is captured once per tiles_midi_in_scan() call, same
 * as the Real-Time callback above. */
typedef void (*tiles_midi_in_note_callback_t)(uint8_t channel, uint8_t note, uint8_t velocity, bool note_on,
                                               uint32_t now_ms);

/* Up to TILES_MIDI_IN_MAX_CALLBACKS listeners each, same fixed-size-
 * table-no-dynamic-allocation shape services/power.h's own
 * tiles_power_register_callback() already established. Returns false
 * if the relevant table is full. */
#define TILES_MIDI_IN_MAX_CALLBACKS 4u
bool tiles_midi_in_register_realtime_callback(tiles_midi_in_realtime_callback_t callback);
bool tiles_midi_in_register_sysex_callback(tiles_midi_in_sysex_callback_t callback);
bool tiles_midi_in_register_note_callback(tiles_midi_in_note_callback_t callback);

/* Monotonic count of MEANINGFUL MIDI events received -- every Note-On/Off
 * and every Real-Time Clock/Start/Continue/Stop, i.e. exactly what a DAW
 * produces while it's actually playing or a melody is being echoed (see
 * services/op_mode.c's melodic-echo section). Deliberately NOT bumped by
 * SysEx (Scene Launch's own feedback fires on any clip change, playing or
 * not), stray data bytes, or Active Sensing -- "MIDI is being received"
 * for the purpose that asked for this (services/standby.c: real feedback,
 * "no screensaver can activate if ableton is playing or midi is being
 * received") means the board is actually being used, not that Ableton
 * happens to be open. Callers detect activity by comparing against a
 * previously seen value (wraps harmlessly; only inequality matters), same
 * "poll a counter, don't register another callback" shape that keeps this
 * off the 4-listener registration tables above. */
uint32_t tiles_midi_in_activity_count(void);
