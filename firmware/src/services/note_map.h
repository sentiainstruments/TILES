#pragma once

/* Pad -> MIDI note: scale, key, octave, and the bass guitar and chord
 * layouts. Default: the bottom row is lowest, left to right, each row up
 * continuing the one below (pad 19 lowest, pad 6 highest); 24 pads = 2
 * octaves chromatic. A scale folds positions through its interval table; a
 * new scale is an enum value plus a table. */

#include <stdbool.h>
#include <stdint.h>

/* Call once, early. With `crash_recovered`, scale/octave/key are kept
 * (__uninitialized_ram survives a watchdog reset, not power off). */
void tiles_note_map_init(bool crash_recovered);

/* Scales in picker order (SCALE_GRID_ORDER): chromatic, major, minor, ...;
 * pads 16-24 are CUSTOM_1..9 (from the companion app; an empty one isn't
 * selectable and falls back to chromatic). PHRYGIAN, LOCRIAN,
 * COMBINATION_DIMINISHED and RAGA_TODI are off the picker; values stay. */
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

/* True if `scale` has an interval table; false for an empty CUSTOM_* slot. */
bool tiles_note_map_scale_is_defined(tiles_scale_mode_t scale);

/* Custom scale slot 1-9: 1-12 intervals from 0, strictly rising, below 12.
 * count 0 empties it. False (unchanged) for a bad slot or table. */
#define TILES_NOTE_MAP_NUM_CUSTOM_SCALES 9u
#define TILES_NOTE_MAP_MAX_SCALE_NOTES 12u
bool tiles_note_map_custom_scale_valid(const int8_t *intervals, uint8_t count);
bool tiles_note_map_set_custom_scale(uint8_t slot_1_to_9, const int8_t *intervals, uint8_t count);
/* Copies a custom slot's table into `out` (capacity
 * TILES_NOTE_MAP_MAX_SCALE_NOTES); returns its count, 0 if empty. */
uint8_t tiles_note_map_get_custom_scale(uint8_t slot_1_to_9, int8_t *out);

/* MIDI note of the lowest pad (19) in chromatic: C3 (MIDI 60 = C4). The
 * one place to change the base octave. */
#define TILES_NOTE_MAP_BASE_NOTE 48u

void tiles_note_map_set_scale(tiles_scale_mode_t scale);
tiles_scale_mode_t tiles_note_map_get_scale(void);

/* Octave shift (set by octave_control.c). +/-3 keeps the span in 12..107:
 * a UX bound. */
#define TILES_NOTE_MAP_MAX_OCTAVE_SHIFT 3
void tiles_note_map_set_octave_shift(int8_t octaves);
int8_t tiles_note_map_get_octave_shift(void);

/* Chord mode's chord strip has its own octave, same range (circle +
 * "-"/"+" in octave_control.c); the melody grid keeps the one above. */
void tiles_note_map_set_chord_octave_shift(int8_t octaves);
int8_t tiles_note_map_get_chord_octave_shift(void);

/* Melodic layouts (Layouts menu; layout.melodic), melodic mode only.
 *   ROWS     the default above.
 *   COLUMNS  three 2-pad columns rising two notes a row; scales of up to 7
 *            notes restart each column on the root, an octave up. */
/*   FOURTHS  rows a fourth apart (in key: scale steps).
 *   KEYS     a piano in row pairs, black keys above white, gaps silent.
 *   FIFTHS   rows a fifth apart, an octave lower.
 *   TONNETZ  right = a fifth, up = a major third, in C3..F#5.
 *   QUARTER_TONES  one octave in 24 steps; odd pads bent 50 cents (MPE).
 * Keys, Tonnetz and Quarter tones: the scale lights pads, all play. */
typedef enum {
    TILES_LAYOUT_ROWS = 0,
    TILES_LAYOUT_COLUMNS,
    TILES_LAYOUT_FOURTHS,
    TILES_LAYOUT_KEYS,
    TILES_LAYOUT_FIFTHS,
    TILES_LAYOUT_TONNETZ,
    TILES_LAYOUT_QUARTER_TONES,
    TILES_NUM_LAYOUTS,
} tiles_melodic_layout_t;
void tiles_note_map_set_layout(tiles_melodic_layout_t layout);
tiles_melodic_layout_t tiles_note_map_get_layout(void);
/* op_mode.c: true in melodic mode only; elsewhere the pads use Rows. */
void tiles_note_map_set_layout_enabled(bool enabled);
bool tiles_note_map_get_layout_enabled(void);
/* False for a pad with no note (Keys' gaps): it plays nothing, stays dark. */
bool tiles_note_map_pad_has_note(uint8_t logical_pad);
/* The pad's offset above its note in cents (Quarter tones' odd pads: 50). */
uint8_t tiles_note_map_get_pad_cents(uint8_t logical_pad);

/* Key (transpose) in semitones, 0 = C (boot default) .. 11 = B. Wraps,
 * since it's a position on the note wheel. Set by octave_control.c's
 * transpose mode. */
void tiles_note_map_set_key_offset(int8_t offset);
int8_t tiles_note_map_get_key_offset(void);

/* MIDI note (0-127, clamped) for pad 1-24 under the current scale, key and
 * octave (or bass guitar / chord layout). 0 for an out-of-range pad. */
uint8_t tiles_note_map_get_note(uint8_t logical_pad);

/* Nearest in-scale note (lower wins a tie, within an octave). Stored
 * sequencer notes play through this, so a scale change needs no rewrite. */
uint8_t tiles_note_map_quantize_to_scale(uint8_t note);

/* True if the pad plays the tonic (positional: transposing doesn't move
 * it). For idle lighting; false out of range. */
bool tiles_note_map_is_root_pad(uint8_t logical_pad);

/* True if the pad is a perfect fifth above the tonic, by interval (a scale
 * without one has none). For idle lighting; false out of range. */
bool tiles_note_map_is_fifth_pad(uint8_t logical_pad);

/* The scale's third: the major third (4 semitones) if the scale has one,
 * else its minor third (3), else none. Same degree math as the fifth. */
bool tiles_note_map_is_third_pad(uint8_t logical_pad);

/* True if the pad's current note is a natural (white key). Depends on the
 * key, unlike the root check. For idle lighting. True out of range. */
bool tiles_note_map_is_natural_pad(uint8_t logical_pad);

/* ---- Bass guitar mode --------------------------------------------------
 * Rows are strings, columns frets (4-string tuning, TAB order). While on,
 * get_note() uses it, so expression.c plays it unchanged. */
void tiles_note_map_set_guitar_mode(bool active);
bool tiles_note_map_is_guitar_mode_active(void);

/* First fret of the visible 6-fret window (0 = open position), clamped
 * for a 24-fret neck (GUITAR_MAX_FRET_OFFSET). Stepped by "-"/"+". */
void tiles_note_map_set_guitar_fret_offset(uint8_t offset);
uint8_t tiles_note_map_get_guitar_fret_offset(void);

/* True if the pad's fret has a neck inlay (3/5/7/9/15/17/19/21; 12 and 24
 * set *out_is_octave). Bass guitar idle lighting. */
bool tiles_note_map_is_guitar_fret_marker_pad(uint8_t logical_pad, bool *out_is_octave);

/* ---- Chord mode --------------------------------------------------------
 * Columns 1-2 are the chord strip (op_mode.c plays its chords); columns
 * 3-6 a 4x4 melody grid through expression.c. */
void tiles_note_map_set_chord_mode(bool active);
bool tiles_note_map_is_chord_mode_active(void);

/* True if the pad is in the chord strip (chord mode only). For lighting,
 * which paints the strip one color. */
bool tiles_note_map_is_chord_region_pad(uint8_t logical_pad);

/* A chord pad's diatonic stack: root, 3rd ... 13th (0-6), from the current
 * scale if it has 7 notes, else Ionian; an octave below the melody. Always
 * the same 7 notes (voicing is op_mode.c's). Zeros for a non-chord pad. */
#define TILES_NOTE_MAP_CHORD_NUM_NOTES 7u
void tiles_note_map_get_chord_notes(uint8_t logical_pad, uint8_t out_notes[TILES_NOTE_MAP_CHORD_NUM_NOTES]);
