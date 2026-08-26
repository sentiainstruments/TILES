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

typedef enum {
    TILES_SCALE_CHROMATIC = 0,
    /* TILES_SCALE_MAJOR, TILES_SCALE_MINOR, etc. go here later -- each
     * needs an interval table in note_map.c and a case in
     * tiles_note_map_get_note(). Non-chromatic scales have fewer than
     * 12 notes per octave, so a pad's position-derived "degree" would
     * index into the scale's own interval table (with octave-doubling
     * every N degrees) rather than mapping 1 pad = 1 semitone the way
     * chromatic does -- see the legacy prototype's scaleInterval() in
     * docs/reference/legacy-prototype-v1/ for the shape of that math,
     * not its code. */
} tiles_scale_mode_t;

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
 * be blue"). Purely positional: always exactly 2 pads out of 24 (one per
 * octave repeat across the chromatic grid's 2-octave span), regardless
 * of the current key offset -- transposing the whole grid changes WHICH
 * note the root pads play, never WHICH pads they are, since every pad
 * shifts by the same amount together. Returns false for an
 * out-of-range pad. */
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
