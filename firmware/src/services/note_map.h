#pragma once

/* Pad -> MIDI note mapping: one owner for "which note does this pad play"
 * (scale, key, octave, plus the bass guitar and chord layouts).
 *
 * Default layout: the bottom row holds the lowest notes, ascending left to
 * right, and each row up continues where the row below ended. In logical
 * pad numbers (pad 1 = top-left, pad 24 = bottom-right), chromatic:
 *
 *   pads 19-24 (row 4)    = C, C#, D, D#, E, F      (pad 19 = lowest)
 *   pads 13-18 (row 3)    = F#, G, G#, A, A#, B
 *   pads 7-12  (row 2)    = C ... F                 (next octave)
 *   pads 1-6   (row 1)    = F# ... B                (highest)
 *
 * 24 pads = 2 octaves chromatic. A scale maps each pad's position (degree
 * 0-23) through its interval table, wrapping to the next octave every N
 * degrees (N = notes in the scale). Adding a scale = an enum value + a
 * table in note_map.c; the layout code doesn't change. */

#include <stdbool.h>
#include <stdint.h>

/* Call once, early in main(). With `crash_recovered` (from
 * watchdog_enable_caused_reboot()), scale/octave/key are left as they were:
 * they live in __uninitialized_ram and survived the reset (see
 * note_map.c). This covers watchdog resets only; RAM doesn't survive power
 * off. */
void tiles_note_map_init(bool crash_recovered);

/* Scales, one pad each in the scale picker (SCALE_GRID_ORDER in
 * note_map.c): chromatic first (the way back), then major, minor, and the
 * rest; pads 16-24 are CUSTOM_* placeholders.
 *
 * PHRYGIAN, LOCRIAN, COMBINATION_DIMINISHED and RAGA_TODI still have
 * correct tables but are no longer on the picker (trimmed for a less
 * overwhelming menu: kept the approachable and "fun" exotic scales). Their
 * enum values stay so nothing else churns.
 *
 * CUSTOM_* are valid enum values with no table yet
 * (tiles_note_map_scale_is_defined() is false): the picker treats them as
 * unavailable, and get_note() would fall back to chromatic if one were
 * ever selected. */
typedef enum {
    TILES_SCALE_CHROMATIC = 0,
    TILES_SCALE_IONIAN,
    TILES_SCALE_DORIAN,
    TILES_SCALE_PHRYGIAN,
    TILES_SCALE_LYDIAN,
    TILES_SCALE_MIXOLYDIAN,
    TILES_SCALE_AEOLIAN,
    TILES_SCALE_LOCRIAN,
    TILES_SCALE_BLUES_MAJOR,
    TILES_SCALE_BLUES_MINOR,
    TILES_SCALE_ARABIAN,
    TILES_SCALE_DIMINISHED,
    TILES_SCALE_COMBINATION_DIMINISHED,
    TILES_SCALE_PENTATONIC_MAJOR,
    TILES_SCALE_PENTATONIC_MINOR,
    TILES_SCALE_EGYPTIAN,
    TILES_SCALE_WHOLE_TONE,
    TILES_SCALE_JAPANESE_MIYAKOBUSHI,
    TILES_SCALE_RAGA_TODI,
    TILES_SCALE_CUSTOM_1,
    TILES_SCALE_CUSTOM_2,
    TILES_SCALE_CUSTOM_3,
    TILES_SCALE_CUSTOM_4,
    TILES_SCALE_CUSTOM_5,
    TILES_SCALE_CUSTOM_6,
    TILES_SCALE_CUSTOM_7,
    TILES_SCALE_CUSTOM_8,
    TILES_SCALE_CUSTOM_9,
    TILES_NUM_SCALE_VALUES, /* sentinel */
} tiles_scale_mode_t;

/* The picker has one scale per pad: 24 slots. */
#define TILES_NOTE_MAP_NUM_SCALE_GRID_SLOTS 24u

/* The scale on picker slot 1-24 (see SCALE_GRID_ORDER). CHROMATIC for an
 * out-of-range slot. */
tiles_scale_mode_t tiles_note_map_scale_for_grid_slot(uint8_t slot_1_to_24);

/* True if `scale` has an interval table; false for the CUSTOM_*
 * placeholders. */
bool tiles_note_map_scale_is_defined(tiles_scale_mode_t scale);

/* MIDI note of the lowest pad (19) in chromatic: C3 (MIDI 60 = C4). The
 * one place to change the base octave. */
#define TILES_NOTE_MAP_BASE_NOTE 48u

void tiles_note_map_set_scale(tiles_scale_mode_t scale);
tiles_scale_mode_t tiles_note_map_get_scale(void);

/* Octave shift in whole octaves, set by services/octave_control.c
 * ("-"/"+") but owned here with the rest of the mapping.
 * +/-3 matches the LED patterns and keeps the 24-pad span well inside
 * 0-127 (12..107): a UX bound, not a MIDI-range clamp. */
#define TILES_NOTE_MAP_MAX_OCTAVE_SHIFT 3
void tiles_note_map_set_octave_shift(int8_t octaves);
int8_t tiles_note_map_get_octave_shift(void);

/* Key (transpose) in semitones, 0 = C (boot default) .. 11 = B. Wraps,
 * since it's a position on the note wheel. Set by octave_control.c's
 * transpose mode. */
void tiles_note_map_set_key_offset(int8_t offset);
int8_t tiles_note_map_get_key_offset(void);

/* MIDI note (0-127, clamped) for pad 1-24 under the current scale, key and
 * octave (or bass guitar / chord layout). 0 for an out-of-range pad. */
uint8_t tiles_note_map_get_note(uint8_t logical_pad);

/* Nearest note to `note` that is in the current scale and key (`note`
 * itself if it already is). The sequencer stores notes unchanged and runs
 * them through this when they play, so changing the scale changes what
 * you hear without rewriting patterns. Searches outward by semitone
 * (lower neighbor wins a tie), at most an octave. */
uint8_t tiles_note_map_quantize_to_scale(uint8_t note);

/* True if this pad plays the tonic (for idle lighting). Positional:
 * transposing changes what the root pads play, not which pads they are.
 * The count depends on the scale: 2 in chromatic and 7-note scales, more
 * for shorter ones (pentatonic 5, whole tone 4, diminished 3). False for
 * an out-of-range pad. */
bool tiles_note_map_is_root_pad(uint8_t logical_pad);

/* True if this pad's degree is a PERFECT fifth (7 semitones) above the
 * tonic, checked against the interval table, not the degree position (a
 * scale without a perfect fifth gets no fifth pads). Chord-mode aware.
 * For idle lighting. False out of range. */
bool tiles_note_map_is_fifth_pad(uint8_t logical_pad);

/* True if the pad's current note is a natural (white key). Depends on the
 * key, unlike the root check. For idle lighting. True out of range. */
bool tiles_note_map_is_natural_pad(uint8_t logical_pad);

/* ---- Bass guitar mode --------------------------------------------------
 * Each row is a string, each column a fret (standard 4-string bass tuning,
 * TAB order; see note_map.c). While active, get_note() uses this instead
 * of the scale layout, so expression.c's whole pipeline plays it unchanged.
 * Set by services/op_mode.c's bass guitar mode. */
void tiles_note_map_set_guitar_mode(bool active);
bool tiles_note_map_is_guitar_mode_active(void);

/* First fret of the visible 6-fret window (0 = open position), clamped
 * for a 24-fret neck (GUITAR_MAX_FRET_OFFSET). Stepped by "-"/"+". */
void tiles_note_map_set_guitar_fret_offset(uint8_t offset);
uint8_t tiles_note_map_get_guitar_fret_offset(void);

/* True if the pad's current fret carries a standard neck inlay
 * (3/5/7/9/15/17/19/21, double dots at 12/24). Marks the whole column,
 * like a real inlay. *out_is_octave (if non-NULL) flags 12/24. For idle
 * lighting; bass guitar mode only. */
bool tiles_note_map_is_guitar_fret_marker_pad(uint8_t logical_pad, bool *out_is_octave);

/* ---- Chord mode --------------------------------------------------------
 * Columns 1-2 (8 pads) are a chord strip read bottom-to-top, left-to-right
 * like the rest of the board; columns 3-6 are a 4x4 melody grid using the
 * normal scale folding (see note_map.c chord_mode_degree()).
 *
 * Chord pads are claimed by services/op_mode.c, which plays
 * tiles_note_map_get_chord_notes() directly (one MIDI note can't be a
 * chord). Melody pads go through expression.c's normal pipeline. */
void tiles_note_map_set_chord_mode(bool active);
bool tiles_note_map_is_chord_mode_active(void);

/* True if the pad is in the chord strip (chord mode only). For lighting,
 * which paints the strip one color. */
bool tiles_note_map_is_chord_region_pad(uint8_t logical_pad);

/* The diatonic chord stack for a chord pad: root, 3rd, 5th, 7th, 9th,
 * 11th, 13th (indices 0-6), built by stacking thirds in a 7-note scale so
 * the triad quality follows the scale like an auto-chord organ. Already
 * one octave below the matching melody note. Uses the current scale if it
 * has 7 notes, else Ionian (stacked thirds only make real chords in a
 * diatonic scale).
 *
 * Always the same 7 notes for a pad: voicing (spread, bass note, how many
 * voices per pressure tier) is op_mode.c's job and is deliberately static
 * and predictable. Writes zeros for a non-chord pad. */
#define TILES_NOTE_MAP_CHORD_NUM_NOTES 7u
void tiles_note_map_get_chord_notes(uint8_t logical_pad, uint8_t out_notes[TILES_NOTE_MAP_CHORD_NUM_NOTES]);
