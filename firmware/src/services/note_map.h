#pragma once

/*
 * Pad -> MIDI note mapping.
 *
 * Physical layout: bottom row is the lowest 6 notes, ascending
 * left-to-right; the next row up continues chromatically from where
 * the row below left off (not restarting); this repeats to the top
 * row. Concretely, in the row-major logical pad numbering (pad 1 =
 * top-left, pad 24 = bottom-right):
 *
 *   pad 19 (bottom-left)  = lowest note (C)
 *   pads 19-24 (row 4)    = C, C#, D, D#, E, F
 *   pads 13-18 (row 3)    = F#, G, G#, A, A#, B   (continues from row 4)
 *   pads 7-12  (row 2)    = C, C#, D, D#, E, F    (next octave up)
 *   pads 1-6   (row 1)    = F#, G, G#, A, A#, B   (highest, pad 1 = F#, pad 6 = B)
 *
 * 4 rows x 6 pads = 24 semitones = exactly 2 octaves in the default
 * chromatic scale.
 *
 * Scale-mode architecture: tiles_note_map_get_note() applies whichever
 * scale is currently selected to the pad's position-derived scale
 * degree. Adding a scale means adding an enum value + interval table
 * here -- the physical layout logic and pad_config.c never change.
 * Only TILES_SCALE_CHROMATIC is implemented for now; other scales are
 * a later layer (per the user's own framing: build the switchable
 * architecture now, fill in more tables when ready). profiles/ will
 * eventually own *persisting* the selected scale and exposing it to
 * the companion app; tiles_note_map_set_scale() is the firmware-level
 * hook that will sit behind that once usb_vendor/ and profiles/ exist.
 */

#include <stdbool.h>
#include <stdint.h>

/* Real feedback: "for melodic it toggles different scale modes... lets
 * do in an ableton push style for the lighting... ionian, dorian,
 * phrigian, lydian, mixo, aeolian, locrian, bluse major and minor,
 * arabian, dim, combination dim, pentatonic major and minor, egyptian,
 * whole tone, japanese miyakobushi, raga todi, the remaining ones are
 * spaces for costume scales." 18 named scales fill most of services/
 * op_mode.h's melodic-mode scale-picker grid (one pad per scale); see
 * TILES_NOTE_MAP_NUM_SCALE_GRID_SLOTS below.
 * **Chromatic is grid slot 1**, real feedback: "add the first mode as
 * chromatic, not major shifting all onse step so we can return to
 * chromatic mode" -- there was originally no way to get BACK to
 * chromatic once a real scale was picked, short of a power cycle.
 * Chromatic already has a real, valid interval table (`scale_table()`'s
 * own `TILES_SCALE_CHROMATIC` case in note_map.c), so it needed no new
 * code to become selectable -- just a slot in `SCALE_GRID_ORDER`. Every
 * named scale shifted down one slot to make room; with only 24 slots
 * total and 1 (chromatic) + 18 (named) already claiming 19, the 6
 * reserved "custom" placeholders shrank to 5 (`CUSTOM_6` dropped) to
 * fit -- none of the 6 have a real interval table yet regardless (see
 * `tiles_note_map_scale_is_defined()` below), so this costs nothing
 * functional, just one fewer future custom slot.
 *
 * Each non-chromatic scale has fewer than 12 notes per octave, so a
 * pad's position-derived "degree" (0-23) indexes into the scale's own
 * interval table in note_map.c, with octave-doubling every N degrees
 * (N = that scale's note count) rather than the fixed 1-pad-per-semitone
 * folding chromatic uses -- see note_map.c's scale_table()/
 * tiles_note_map_get_note() for the actual math, and the legacy
 * prototype's scaleInterval() in docs/reference/legacy-prototype-v1/ for
 * the shape it's modeled on (not its code). The 5 remaining CUSTOM_*
 * values are real, valid enum values (so the grid has something to
 * reference for those slots) but have no interval table yet -- see
 * tiles_note_map_scale_is_defined() below; selecting one from the menu
 * is a UI no-op (services/op_mode.h treats undefined slots as
 * unselectable, matching "unavailable" in the standardized menu
 * language), and tiles_note_map_get_note() would fall back to chromatic
 * internally if one somehow got selected anyway, rather than producing
 * garbage. */
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
    TILES_NUM_SCALE_VALUES, /* sentinel -- 1 (chromatic) + 24 (grid) = 25 */
} tiles_scale_mode_t;

/* The scale-picker grid is exactly this many pads -- matches
 * TILES_NUM_PADS (board/board_pins.h) 1:1, one scale per pad. */
#define TILES_NOTE_MAP_NUM_SCALE_GRID_SLOTS 24u

/* The scale assigned to grid slot 1-24 (services/op_mode.h's melodic
 * scale-picker, one pad per slot, in the same order real feedback listed
 * them: ionian..raga todi filling slots 1-18, CUSTOM_1-6 filling
 * 19-24). Returns TILES_SCALE_CHROMATIC (never actually placed on the
 * grid) for an out-of-range slot. */
tiles_scale_mode_t tiles_note_map_scale_for_grid_slot(uint8_t slot_1_to_24);

/* True if `scale` has a real interval table (every named scale above);
 * false for the 6 CUSTOM_* placeholders, which are valid enum values
 * with nothing behind them yet. */
bool tiles_note_map_scale_is_defined(tiles_scale_mode_t scale);

/* MIDI note for the lowest pad (pad 19, bottom-left) under the
 * chromatic scale. C3 in the common convention where MIDI 60 = C4/
 * middle C -- matches the base note the very first placeholder demo
 * map used. This is the one place to change if a different starting
 * octave is wanted; nothing else should hardcode an octave. */
#define TILES_NOTE_MAP_BASE_NOTE 48u

void tiles_note_map_set_scale(tiles_scale_mode_t scale);
tiles_scale_mode_t tiles_note_map_get_scale(void);

/* Octave shift applied on top of the scale-derived note, in whole
 * octaves (each unit = +/-12 semitones). Driven by services/octave_control.c
 * (SW1 "-"/SW2 "+", the default function of those two buttons) but
 * lives here, not there, since it's a note-mapping parameter exactly
 * like the scale above -- one owner for "how a pad's position becomes a
 * MIDI note."
 *
 * Clamped to +/-TILES_NOTE_MAP_MAX_OCTAVE_SHIFT: chosen to match the
 * highest octave_control.c LED pattern (3) and because it keeps the
 * full 24-pad chromatic span (BASE_NOTE..BASE_NOTE+23) safely inside
 * 0-127 at the extremes (12..107) with real margin either side, so the
 * limit is never actually reached by the 0-127 clamp in
 * tiles_note_map_get_note() below -- it's a deliberate UX bound, not a
 * MIDI-range safety clamp. */
#define TILES_NOTE_MAP_MAX_OCTAVE_SHIFT 3
void tiles_note_map_set_octave_shift(int8_t octaves);
int8_t tiles_note_map_get_octave_shift(void);

/* Transpose ("key center") offset in semitones, 0-11: 0 = C (the
 * default key the board boots into), 11 = B. Wraps rather than clamps
 * (unlike octave shift above) since it's a position on the 12-note
 * chromatic wheel, not a magnitude with a real edge -- stepping past B
 * lands back on C and vice versa. Driven by services/octave_control.c's
 * transpose mode (SW1+SW2 held together toggles it, then "-"/"+" step
 * the key while it's active) but lives here for the same reason octave
 * shift does: one owner for "how a pad's position becomes a MIDI
 * note." */
void tiles_note_map_set_key_offset(int8_t offset);
int8_t tiles_note_map_get_key_offset(void);

/* MIDI note number (0-127, clamped) for logical pad (1-24) under the
 * currently selected scale and octave shift. Returns 0 for an
 * out-of-range pad. */
uint8_t tiles_note_map_get_note(uint8_t logical_pad);

/* True if this pad is currently the key's tonic (root) note -- driven by
 * services/lighting.c's idle pad coloring (real feedback: "root should
 * be blue"). Purely positional: independent of the current key offset --
 * transposing the whole grid changes WHICH note the root pads play,
 * never WHICH pads they are, since every pad shifts by the same amount
 * together. Count of root pads now DOES depend on the current scale
 * (added alongside the scale-picker above): exactly 2 for chromatic (one
 * per octave repeat across the grid's 2-octave span) and every other
 * 12-degrees-per-octave case, but a shorter scale repeats more often
 * across the same 24-pad span -- e.g. pentatonic (5 notes/octave) has 5
 * root pads (degree 0, 5, 10, 15, 20), whole tone (6) has 4, diminished
 * (8) has 3. Returns false for an out-of-range pad. */
bool tiles_note_map_is_root_pad(uint8_t logical_pad);

/* True if this pad's CURRENTLY MAPPED note (tiles_note_map_get_note())
 * is a natural (white key) rather than sharp/flat (black key) --
 * likewise driven by services/lighting.c's idle pad coloring. Unlike
 * tiles_note_map_is_root_pad() above, this DOES depend on the current
 * key offset: transposing changes which absolute pitch class (and so
 * which natural/sharp classification) each physical pad plays. Returns
 * true (natural) for an out-of-range pad, matching this function's
 * "nothing special about this pad" default. */
bool tiles_note_map_is_natural_pad(uint8_t logical_pad);

/* ---- Guitar/bass fret mode ----------------------------------------------
 * Real feedback: "lets imoplenment for note mode a guitar fret mode for 4
 * stings with the structure of bass shapes, -+ change frets up and down.
 * each row is a string each colum is a fret." A completely different note
 * -mapping shape from the scale system above (absolute string+fret, not
 * scale-degree-relative) -- see note_map.c's own header comment on the
 * standard 4-string bass tuning and the TAB-notation row/string
 * convention this follows. services/op_mode.h's guitar mode (selectable
 * from the mode picker) is the only caller of the setters below; while
 * active, tiles_note_map_get_note() branches to the guitar computation
 * entirely instead of the scale-based one, so services/expression.c's
 * whole touch/velocity/pitch-bend/haptics pipeline works completely
 * unchanged -- it just ends up playing different notes. */
void tiles_note_map_set_guitar_mode(bool active);
bool tiles_note_map_is_guitar_mode_active(void);

/* Which 6-fret window is currently visible (columns 1-6 show frets
 * offset+0 .. offset+5) -- 0 = open position. Clamped to
 * [0, 24-6] = [0, 18]... see note_map.c's own GUITAR_MAX_FRET_OFFSET for
 * the exact derivation, matching a 24-fret neck. Stepped by "-"/"+" in
 * guitar mode (real feedback: "-+ change frets up and down"). */
void tiles_note_map_set_guitar_fret_offset(uint8_t offset);
uint8_t tiles_note_map_get_guitar_fret_offset(void);

/* True if this pad's CURRENT fret (guitar mode only -- meaningless
 * otherwise) is one of the standard inlay-dot marker positions real
 * guitar/bass necks use to help a player find their place without
 * counting frets one by one (3/5/7/9/15/17/19/21, and the octave points
 * 12/24 -- universal convention, not invented here). Marks the WHOLE
 * column (all 4 strings), matching how a real neck's inlay dot spans the
 * width of the fretboard rather than sitting under one specific string.
 * *out_is_octave (if non-NULL) distinguishes the double-dot octave
 * markers (12/24) from the single-dot ones, for a brighter/distinct
 * rendering -- services/lighting.c's own idle pad coloring is the one
 * caller. */
bool tiles_note_map_is_guitar_fret_marker_pad(uint8_t logical_pad, bool *out_is_octave);
