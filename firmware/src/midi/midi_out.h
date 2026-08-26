#pragma once

/*
 * USB MIDI note output.
 *
 * V1 scope, deliberately minimal: single MIDI channel. No MPE per-note
 * channel allocation yet (needs the voice-stealing policy described in
 * docs/architecture/defaults-and-safeguards.md). Velocity/aftertouch
 * come from services/expression.c's Hall-derived strike acceleration
 * and press depth -- see that module for how, and for the unmeasured
 * (V1 placeholder) scaling constants that produce them.
 */

#include <stdint.h>

void tiles_midi_note_on(uint8_t note, uint8_t velocity);
void tiles_midi_note_off(uint8_t note);

/* Polyphonic Key Pressure / aftertouch (0xA0 | channel, note,
 * pressure) -- per-note, not per-channel, so multiple pads held at
 * once (even on this single V1 channel) can each report their own
 * pressure independently. */
void tiles_midi_send_poly_aftertouch(uint8_t note, uint8_t pressure);

/* Sends a Control Change message (0xB0 | channel, controller, value)
 * on the same V1 single channel. Used for the pedal (sustain = CC64,
 * expression = CC11) and available for anything else that needs a raw
 * CC later. */
void tiles_midi_send_cc(uint8_t controller, uint8_t value);

/* Sends a Pitch Bend Change (0xE0 | channel, LSB, MSB) on the same V1
 * single channel. bend_14bit is the full unsigned wire value (0-16383,
 * 8192 = center/no bend) -- callers do the signed-to-wire conversion
 * themselves. Channel-wide, not per-note: this is a real limitation
 * without MPE's per-note channel allocation (see this file's own "V1
 * scope" note) -- bending one pad's pitch bends every other note
 * currently held on this same channel too. services/expression.c's
 * pitch-bend feature works around this by only ever having one pad "own"
 * the bend at a time; see its own header for the full reasoning. */
void tiles_midi_send_pitch_bend(uint16_t bend_14bit);
