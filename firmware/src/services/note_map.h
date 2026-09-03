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
 * the shape it's modeled on (not its code).
 *
 * PHRYGIAN, LOCRIAN, COMBINATION_DIMINISHED, and RAGA_TODI were dropped
 * from the grid (see SCALE_GRID_ORDER in note_map.c) -- real feedback:
 * "we have to many scales on the scale selector and its kinda
 * overwhelming... cut the ones that have less than 5 notes" led to
 * checking every scale's real note count (none were actually below 5,
 * see services/README.md's own entry for the full table), then a
 * follow-up redirected the criterion entirely: "we should get rid of
 * non atractive experimental ones not experiemntal easy to get into" --
 * Locrian (the one mode most musicians avoid, its flattened 5th over
 * the root reads as unresolved rather than musical) and Phrygian (a
 * harder, tenser sound for a lot of ears) were the two flagged as
 * "simple but not attractive"; Combination Diminished and Raga Todi were
 * cut as the least load-bearing of the "theory/exotic" scales once the
 * list needed shortening, while the genuinely fun exotic ones (Arabian,
 * Egyptian, Japanese Miyakobushi, Diminished, Whole Tone) were kept on
 * purpose, real feedback: "they sound fun." Their enum values are kept
 * (removing them outright would be needless churn for four scales that
 * still have real, correct interval tables -- see scale_table() in
 * note_map.c, which still handles all four) -- they're just no longer
 * placed in SCALE_GRID_ORDER, so they're unreachable from the picker
 * without also restoring their grid slot. Freed 4 grid slots, used to
 * add TILES_SCALE_CUSTOM_7/8/9 below (real feedback's own removals
 * happened to be named scales, not custom placeholders, so the total
 * custom-slot count only grew as a side effect of keeping the grid at a
 * full 24 -- not a deliberate request to add more custom slots).
 *
 * The remaining CUSTOM_* values are real, valid enum values (so the grid
 * has something to reference for those slots) but have no interval
 * table yet -- see tiles_note_map_scale_is_defined() below; selecting
 * one from the menu is a UI no-op (services/op_mode.h treats undefined
 * slots as unselectable, matching "unavailable" in the standardized menu
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
    TILES_SCALE_CUSTOM_7,
    TILES_SCALE_CUSTOM_8,
    TILES_SCALE_CUSTOM_9,
    TILES_NUM_SCALE_VALUES, /* sentinel */
} tiles_scale_mode_t;

/* The scale-picker grid is exactly this many pads -- matches
 * TILES_NUM_PADS (board/board_pins.h) 1:1, one scale per pad. */
#define TILES_NOTE_MAP_NUM_SCALE_GRID_SLOTS 24u

/* The scale assigned to grid slot 1-24 (services/op_mode.h's melodic
 * scale-picker, one pad per slot) -- see SCALE_GRID_ORDER in note_map.c
 * for the exact, real-feedback-driven order (chromatic, then major,
 * then minor, then the rest, per "we start with chrommatic, major,
 * minor and then the rest") and this header's own enum comment for why
 * 4 named scales are no longer placed on it. Returns TILES_SCALE_CHROMATIC
 * (never actually placed on the grid) for an out-of-range slot. */
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

/* ---- Chord mode -----------------------------------------------------
 * Real feedback: "lets create a mode that does chords on one side colum
 * 1 and 2 (pad19 c chord, pad20 d chord, pad13 chord e and loke that.)
 * and melody in columns 3456 in a 4x4 grid starting with c in pad 21...
 * the main thing is chords are one octave lower than melodic." A hybrid
 * of the two shapes above: columns 1-2 (8 pads) are a self-contained
 * chord strip using the SAME bottom-to-top, left-to-right reading order
 * every other mode in this file already uses, just narrowed to 2
 * columns instead of 6; columns 3-6 (16 pads, a real 4x4 grid) are a
 * self-contained MELODY sub-grid using the identical scale-degree
 * folding tiles_note_map_get_note() already does for normal play, just
 * narrowed to 4 columns instead of 6, so it still reads as chromatic/
 * scale-aware single-note melodic play, root/natural/sharp coloring and
 * all -- see note_map.c's own chord_mode_degree() for the exact
 * row/column math both regions share.
 *
 * Chord region pads don't go through tiles_note_map_get_note() at all
 * during real play -- a single MIDI note can't represent a full chord,
 * so services/op_mode.h claims those 8 pads directly (see
 * tiles_op_mode_owns_pad() in op_mode.h) and calls
 * tiles_note_map_get_chord_notes() below instead, exactly the same
 * "claim the grid, drive MIDI directly" pattern services/op_mode.h's
 * sequencer already uses, just for 8 specific pads instead of all 24.
 * Melody region pads DON'T get claimed -- they fall through to
 * services/expression.c's completely unchanged normal touch/velocity/
 * aftertouch/haptics pipeline, the same "reuse the existing pipeline,
 * only remap notes" approach guitar mode above already established. */
void tiles_note_map_set_chord_mode(bool active);
bool tiles_note_map_is_chord_mode_active(void);

/* True if this pad is in chord mode's own chord strip (columns 1-2) --
 * meaningless (always false) unless chord mode is active. The one
 * caller is services/lighting.c, to render the whole strip one solid
 * color instead of per-pad note-role coloring -- real feedback: "leds
 * for chords are color blue all of them together." */
bool tiles_note_map_is_chord_region_pad(uint8_t logical_pad);

/* Computes the 3 notes (root, diatonic third, diatonic fifth -- built
 * from a real 7-note diatonic scale's own degree spacing, so the triad
 * quality (major/minor/diminished) automatically matches whichever
 * diatonic scale is active, exactly like a real "auto-chord"/chord-organ
 * instrument harmonizes each scale degree) for a chord-region pad, each
 * ALREADY shifted two octaves down from where the equivalent melody note
 * would sit -- real feedback: "the main thing is chords are one octave
 * lower than melodic," then "make chords an octave loower." Uses the
 * globally selected scale if it's genuinely diatonic (7 notes), else
 * falls back to Ionian (major) -- real feedback, once heard on real
 * hardware: "chords are not structured propperly. they should all be
 * the chords on a same key and real chords not random 3 note group";
 * the skip-one/skip-two harmonization below only produces a real triad
 * against a real diatonic scale (see note_map.c's own chord_mode_
 * scale_table() for the full reasoning). Writes exactly 3 notes into
 * out_notes, always in root position (root, third, fifth ascending) --
 * a pad outside the chord region (or chord mode not active) writes all
 * zeros -- callers are expected to only call this for pads
 * tiles_note_map_is_chord_region_pad() already confirmed. Callers that
 * want smoother voice leading between successive chords (see op_mode.c's
 * chord_pad_note_on()) are expected to re-voice these 3 notes themselves
 * via tiles_note_map_nearest_pitch_class() below -- this function always
 * returns the same root-position triad for a given pad regardless of
 * what played before it, so a caller with no voice-leading state (or
 * anything just wanting the plain triad) still gets a musically correct
 * answer. */
#define TILES_NOTE_MAP_CHORD_NUM_NOTES 3u
void tiles_note_map_get_chord_notes(uint8_t logical_pad, uint8_t out_notes[TILES_NOTE_MAP_CHORD_NUM_NOTES]);

/* Nearest instance of `note`'s own pitch class to `anchor` (e.g. pitch
 * class G folds to 5 semitones BELOW an anchor of C, not 7 above, since
 * |-5| < |+7|). Real feedback, chord mode heard on real hardware: "make
 * the chords with inversions to make them feel more musical" -- the
 * intended caller is op_mode.c's chord playback, re-voicing each new
 * chord's raw root-position triad (from tiles_note_map_get_chord_notes()
 * above) toward wherever the previous chord actually sounded, one note
 * at a time, rather than every chord always stacking upward fresh from
 * its own root -- the same "keep the voicing compact, in the same
 * register" quality a real chord organ/autoharp's auto-chord has, and
 * automatically produces real chord INVERSIONS wherever that's what
 * keeps a chord tone closest to where the music already was. Exposed
 * here rather than kept internal because the "last chord played" state
 * that supplies `anchor` belongs in op_mode.c alongside this file's
 * other chord-playback bookkeeping, not in this otherwise-stateless
 * note-mapping file. */
uint8_t tiles_note_map_nearest_pitch_class(uint8_t note, uint8_t anchor);
