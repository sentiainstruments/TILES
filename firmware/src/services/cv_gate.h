#pragma once

/* CV/gate output, working like a standard monophonic MIDI-to-CV converter:
 *   - VOUTA: 1 V/octave pitch CV.
 *   - VOUTB: pressure CV (as the board map labels it).
 *   - Gate (GP12, active high): high while ANY note is held.
 * Last-note priority: a new note jumps the pitch CV (legato, no glide, no
 * retrigger) and the gate stays high until the last note ends. It listens
 * to every note TILES plays (live, sequencer, Song, chord, game), like an
 * external converter would.
 *
 * Scaling is runtime-settable calibration (settings `cv_gate.*`), defaulting
 * to the standard unscaled convention until measured trims exist.
 *
 * Safety (docs/architecture/defaults-and-safeguards.md "CV and gate"):
 *   - Hard-disabled unless services/power.h reports cv_gate_permitted
 *     (external 12 V present). Checked on every note event, and the
 *     power-change callback drops everything the instant power goes.
 *   - OFF by default even with external power; tiles_cv_gate_set_enabled()
 *     is a separate switch that is never saved (boots off every time).
 *     Both must hold before anything is driven. */

#include <stdbool.h>
#include <stdint.h>

/* Pitch CV (VOUTA). volts_per_semitone/reference_note are the musical
 * mapping (default 1 V/octave, note 0 = 0 V, the Eurorack convention);
 * zero_trim_volts/gain_trim correct the real DAC/op-amp (identity until
 * measured). Volts at the jack = (note - reference_note) *
 * volts_per_semitone * gain_trim + zero_trim_volts, clamped to 0-10 V
 * (0-2.5 V at the DAC). */
typedef struct {
    float volts_per_semitone;
    int16_t reference_note;
    float zero_trim_volts;
    float gain_trim;
} tiles_cv_pitch_calibration_t;

/* Pressure CV (VOUTB). full_scale_volts = volts at pressure 127 (default
 * 10 V, the jack's full range); trims as for pitch. */
typedef struct {
    float full_scale_volts;
    float zero_trim_volts;
    float gain_trim;
} tiles_cv_pressure_calibration_t;

/* Claims SPI1 (drivers/dac80502.h) and sets up GP12 (gate) as a safe-off
 * output. Call after board_init(). */
void tiles_cv_gate_init(void);

/* Near no-op: updates are event-driven (note calls below) and power
 * changes arrive by callback. Kept so every service has init/scan; call
 * every main-loop pass. */
void tiles_cv_gate_scan(void);

/* Call at every note-on/off/pressure site, next to the MIDI send (like
 * services/haptics.h's calls), rather than hooking midi/, which knows
 * nothing about note priority or calibration. No-ops unless CV/gate is
 * both power-permitted and enabled, so callers need no guard. */
void tiles_cv_gate_note_on(uint8_t note, uint8_t velocity);
void tiles_cv_gate_note_off(uint8_t note);
void tiles_cv_gate_channel_pressure(uint8_t note, uint8_t pressure);

void tiles_cv_gate_set_enabled(bool enabled);
bool tiles_cv_gate_is_enabled(void);

/* True while enabled AND power-permitted (what the note calls check). For
 * diagnostics or a status LED. */
bool tiles_cv_gate_is_active(void);

void tiles_cv_gate_set_pitch_calibration(tiles_cv_pitch_calibration_t calibration);
tiles_cv_pitch_calibration_t tiles_cv_gate_get_pitch_calibration(void);

void tiles_cv_gate_set_pressure_calibration(tiles_cv_pressure_calibration_t calibration);
tiles_cv_pressure_calibration_t tiles_cv_gate_get_pressure_calibration(void);
