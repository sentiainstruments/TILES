#pragma once

/*
 * CV/gate output -- real feedback: "lets implement cv/gate functionality
 * as it would work standard but with modifiable standard controls for
 * the control software down the line."
 *
 * "As it would work standard": a classic monophonic MIDI-to-CV
 * converter, the same shape virtually every hardware one uses --
 * 1V/octave pitch CV (VOUTA), a second CV channel tracking pressure/
 * aftertouch (VOUTB, matching what docs/hardware/
 * sentia_tiles_board_map_v1.json's own board map already labels that
 * channel), and a single active-high Gate that's high for as long as
 * ANY note this instrument is currently playing is held, low otherwise.
 * Last-note-priority when more than one note is held at once (the same
 * "most recently touched/bent pad wins" rule this session's non-MPE
 * pitch-bend mode already established for its own shared-channel
 * arbitration, applied here to a genuinely single-voice output instead
 * of a shared MIDI channel) -- switching priority note is an immediate
 * pitch-CV jump (legato, no glide/portamento), not a retrigger; the
 * gate itself never drops as long as at least one note stays held.
 * Deliberately note-based, not pad/lane/slot-based -- this listens to
 * every note this instrument's own MIDI output ever plays (live touch,
 * sequencer, Song mode, chord mode, the hidden game melody), the same
 * way a real external MIDI-to-CV box listens to the note stream itself
 * rather than caring which internal feature produced it.
 *
 * "Modifiable standard controls for the control software down the
 * line": both CV channels' scaling are runtime-settable structs, not
 * fixed compile-time constants -- defaulting to the standard, unscaled
 * convention (see each struct's own comment) until a companion app
 * (once usb_vendor/ exists, same "no on-device gesture yet" shape this
 * session's other companion-app-hook toggles already have -- pedal
 * mode, MPE mode) sets real measured trim values. Not yet persisted to
 * flash -- same as pedal.c's polarity/mode and expression.c's pitch-
 * bend sensitivity, this resets to standard defaults on every reboot;
 * real persistence belongs in a `storage/` module that doesn't exist in
 * this codebase yet (docs/hardware/SENTIA_TILES_FIRMWARE_HANDOFF.md's
 * own recommended module boundary lists it as a separate, not-yet-built
 * piece), not invented here just for this one value.
 *
 * Safety, per docs/architecture/defaults-and-safeguards.md's own "CV
 * and gate" rules (both already-established, non-negotiable hardware
 * facts, not re-decided here):
 *   - Hard-disabled unless services/power.h's tiles_power_get_state()
 *     reports cv_gate_permitted (external 12V confirmed present via
 *     GP22) -- checked on every note event AND reacted to instantly via
 *     tiles_power_register_callback() the moment power state changes
 *     mid-hold, not just on this module's own next poll.
 *   - Defaults OFF even when external power is valid -- tiles_cv_
 *     gate_set_enabled() is the explicit, separate software switch. Both
 *     gates (power-permitted AND software-enabled) must hold before
 *     anything is ever actually driven.
 */

#include <stdbool.h>
#include <stdint.h>

/* Pitch CV (VOUTA). volts_per_semitone/reference_note are the MUSICAL
 * mapping (default: standard 1V/octave, MIDI note 0 = 0V, matching the
 * convention most Eurorack/analog gear expects); zero_trim_volts/
 * gain_trim are a SEPARATE, later correction layer for the real op-amp/
 * DAC's own imperfection at the jack (defaults to identity -- 0V trim,
 * 1.0 gain -- exactly the "no trim until measured" default docs/
 * architecture/defaults-and-safeguards.md's own "CV range" section
 * calls for). Final volts at the jack = (note - reference_note) *
 * volts_per_semitone * gain_trim + zero_trim_volts, clamped to the
 * DAC's own 0-2.5V (pre-amp) / 0-10V (post-amp) range. */
typedef struct {
    float volts_per_semitone;
    int16_t reference_note;
    float zero_trim_volts;
    float gain_trim;
} tiles_cv_pitch_calibration_t;

/* Pressure CV (VOUTB). full_scale_volts is the MUSICAL mapping (volts
 * at MIDI channel-pressure 127, default the jack's own full 10V nominal
 * range); zero_trim_volts/gain_trim are the same later hardware-
 * correction layer as pitch's own, same identity default. */
typedef struct {
    float full_scale_volts;
    float zero_trim_volts;
    float gain_trim;
} tiles_cv_pressure_calibration_t;

/* Claims SPI1 (via drivers/dac80502.h) and configures GP12 (gate) as a
 * safe-off output. Must run after board_init(). */
void tiles_cv_gate_init(void);

/* Currently a near no-op -- every real update happens event-driven, from
 * the note_on/note_off/channel_pressure calls below, and power-state
 * reactions are instant via the registered callback, not polled. Kept
 * for the same "every service gets an _init()/_scan() pair" consistency
 * this codebase's other services already establish, and as a ready home
 * for any future time-based behavior (e.g. a retrigger pulse) that
 * isn't part of "standard" basic CV/gate and wasn't asked for. Call
 * every main-loop iteration regardless. */
void tiles_cv_gate_scan(void);

/* Call these at every one of this instrument's own note-on/off/
 * pressure sites, alongside (not instead of) the matching MIDI send --
 * the same "explicit call at each site" shape services/haptics.h's own
 * trigger_kick()/stop() already use throughout services/expression.c,
 * op_mode.c, and game_mode.c, rather than a hidden hook inside midi/
 * (which has no business knowing about note priority, calibration, or
 * any other services/-level concept -- see this file's own header
 * comment on why the module boundary matters here). No-ops entirely
 * whenever CV/gate isn't both power-permitted and software-enabled --
 * callers never need their own guard. */
void tiles_cv_gate_note_on(uint8_t note, uint8_t velocity);
void tiles_cv_gate_note_off(uint8_t note);
void tiles_cv_gate_channel_pressure(uint8_t note, uint8_t pressure);

void tiles_cv_gate_set_enabled(bool enabled);
bool tiles_cv_gate_is_enabled(void);

/* True only while both gates hold: software-enabled AND services/
 * power.h currently reports cv_gate_permitted. What tiles_cv_gate_
 * note_on() etc. actually check internally -- exposed for diagnostics/
 * a future status LED, not required for correct use. */
bool tiles_cv_gate_is_active(void);

void tiles_cv_gate_set_pitch_calibration(tiles_cv_pitch_calibration_t calibration);
tiles_cv_pitch_calibration_t tiles_cv_gate_get_pitch_calibration(void);

void tiles_cv_gate_set_pressure_calibration(tiles_cv_pressure_calibration_t calibration);
tiles_cv_pressure_calibration_t tiles_cv_gate_get_pressure_calibration(void);
