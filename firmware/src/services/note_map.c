#include "note_map.h"

#include <stddef.h>

#include "pad_config.h"

static tiles_scale_mode_t s_scale = TILES_SCALE_CHROMATIC;
static int8_t s_octave_shift = 0;
static int8_t s_key_offset = 0;

/* ---- Guitar/bass fret mode ----------------------------------------------
 * Real feedback: "lets imoplenment for note mode a guitar fret mode for 4
 * stings with the structure of bass shapes, -+ change frets up and down.
 * each row is a string each colum is a fret." Standard 4-string bass
 * tuning, low to high: E1, A1, D2, G2 -- each string a perfect 4th (5
 * semitones) above the one below it, the real, standard bass/guitar
 * tuning interval, not invented here. MIDI note numbers (middle C = 60):
 * E1=28, A1=33, D2=38, G2=43.
 * Row-to-string assignment follows standard TAB notation -- the
 * near-universal convention for exactly this "row = string, column =
 * fret/time" shape (a real fretboard/tab diagram always draws the
 * HIGHEST-pitched string on the top line and the LOWEST on the bottom,
 * mirroring how the strings sit when the instrument is held in normal
 * playing position and viewed from above). Row 1 (closest to the
 * function buttons) = G2, the highest string; row 4 (farthest) = E1, the
 * lowest. */
#define GUITAR_NUM_STRINGS 4u
#define GUITAR_VISIBLE_FRETS 6u
#define GUITAR_MAX_FRET 24u /* a common high-end real bass/guitar fret count */
#define GUITAR_MAX_FRET_OFFSET (GUITAR_MAX_FRET - GUITAR_VISIBLE_FRETS + 1u) /* 19 -- window's last column then shows fret 24 */

static const uint8_t GUITAR_STRING_OPEN_NOTE[GUITAR_NUM_STRINGS] = {
    43u, /* row 1 (top) = G2 */
    38u, /* row 2 = D2 */
    33u, /* row 3 = A1 */
    28u, /* row 4 (bottom) = E1 */
};

static bool s_guitar_mode_active;
static uint8_t s_guitar_fret_offset;

/* Standard fretboard inlay-dot positions -- single dots at 3/5/7/9 and
 * their next-octave repeats 15/17/19/21, double dots at the octave points
 * 12/24. Universal across virtually every real guitar/bass neck. */
static bool guitar_fret_is_single_marker(uint8_t fret) {
    switch (fret % 12u) {
    case 3u:
    case 5u:
    case 7u:
    case 9u:
        return true;
    default:
        return false;
    }
}

static bool guitar_fret_is_octave_marker(uint8_t fret) {
    return fret != 0u && (fret % 12u) == 0u;
}

void tiles_note_map_set_guitar_mode(bool active) {
    s_guitar_mode_active = active;
}

bool tiles_note_map_is_guitar_mode_active(void) {
    return s_guitar_mode_active;
}

void tiles_note_map_set_guitar_fret_offset(uint8_t offset) {
    if (offset > (uint8_t)GUITAR_MAX_FRET_OFFSET) {
        offset = (uint8_t)GUITAR_MAX_FRET_OFFSET;
    }
    s_guitar_fret_offset = offset;
}

uint8_t tiles_note_map_get_guitar_fret_offset(void) {
    return s_guitar_fret_offset;
}

static uint8_t guitar_note_for_pad(const tiles_pad_config_t *cfg) {
    uint8_t string_index = (uint8_t)(cfg->row - 1u); /* 0..3 */
    uint8_t fret = (uint8_t)(s_guitar_fret_offset + (cfg->col - 1u));
    int note = (int)GUITAR_STRING_OPEN_NOTE[string_index] + (int)fret;
    if (note > 127) {
        note = 127;
    }
    return (uint8_t)note;
}

bool tiles_note_map_is_guitar_fret_marker_pad(uint8_t logical_pad, bool *out_is_octave) {
    const tiles_pad_config_t *cfg = board_pad_config(logical_pad);
    if (cfg == NULL) {
        return false;
    }
    uint8_t fret = (uint8_t)(s_guitar_fret_offset + (cfg->col - 1u));
    if (guitar_fret_is_octave_marker(fret)) {
        if (out_is_octave != NULL) {
            *out_is_octave = true;
        }
        return true;
    }
    if (guitar_fret_is_single_marker(fret)) {
        if (out_is_octave != NULL) {
            *out_is_octave = false;
        }
        return true;
    }
    return false;
}

/* ---- Chord mode -----------------------------------------------------
 * See note_map.h's own "Chord mode" section for the full design. Columns
 * 1-2 are an 8-pad chord strip, columns 3-6 a 16-pad (4x4) melody
 * sub-grid -- both use the SAME bottom-to-top, left-to-right degree
 * sweep every other mode in this file already uses (see pad_degree()
 * above), just narrowed to however many columns that region actually
 * has instead of the full 6. */
#define CHORD_REGION_MAX_COL 2u
#define CHORD_REGION_NUM_COLS 2u
#define CHORD_MELODY_MIN_COL 3u
#define CHORD_MELODY_NUM_COLS 4u
/* Chord octave offset -- real feedback: "the main thing is chords are
 * one octave lower than melodic." */
#define CHORD_OCTAVE_DOWN_SEMITONES 12
/* Diatonic third and fifth, in SCALE-DEGREE space (not semitones) --
 * skip-one, skip-two through the scale's own interval table, same as
 * how a real chord-organ/autoharp harmonizes each scale degree. This is
 * what makes the triad quality (major/minor/diminished) automatically
 * correct for whichever scale is currently selected, rather than always
 * building a plain major triad regardless of scale. */
#define CHORD_THIRD_DEGREE_STEP 2u
#define CHORD_FIFTH_DEGREE_STEP 4u

static bool s_chord_mode_active;

void tiles_note_map_set_chord_mode(bool active) {
    s_chord_mode_active = active;
}

bool tiles_note_map_is_chord_mode_active(void) {
    return s_chord_mode_active;
}

bool tiles_note_map_is_chord_region_pad(uint8_t logical_pad) {
    if (!s_chord_mode_active) {
        return false;
    }
    const tiles_pad_config_t *cfg = board_pad_config(logical_pad);
    if (cfg == NULL) {
        return false;
    }
    return cfg->col <= CHORD_REGION_MAX_COL;
}

/* Degree within whichever of chord mode's two regions `cfg` falls in --
 * shared by tiles_note_map_get_note() (melody region) and
 * tiles_note_map_get_chord_notes() (chord region) below, the same
 * "one shared degree function, several callers" shape pad_degree()
 * above already established. Only meaningful while chord mode is
 * active; callers already gate on that. */
static uint8_t chord_mode_degree(const tiles_pad_config_t *cfg) {
    uint8_t musical_row = (uint8_t)(4u - cfg->row);
    if (cfg->col <= CHORD_REGION_MAX_COL) {
        return (uint8_t)(musical_row * CHORD_REGION_NUM_COLS + (cfg->col - 1u));
    }
    return (uint8_t)(musical_row * CHORD_MELODY_NUM_COLS + (cfg->col - CHORD_MELODY_MIN_COL));
}

/* Natural-note pitch classes (0=C, 1=C#, ... 11=B) -- true = natural
 * (white key), false = sharp/flat (black key). Indexed by ABSOLUTE pitch
 * class (a note number mod 12), not by key-relative scale degree.
 * services/octave_control.c has its own similar-looking table
 * (s_key_table) but that one is indexed by key offset and carries a
 * letter for its transpose-mode display -- a different use case that
 * happens to share the same underlying 12-pitch-class pattern, not
 * something to unify with this one. */
static const bool s_pitch_class_is_natural[12] = {
    true,  /* C */
    false, /* C# */
    true,  /* D */
    false, /* D# */
    true,  /* E */
    true,  /* F */
    false, /* F# */
    true,  /* G */
    false, /* G# */
    true,  /* A */
    false, /* A# */
    true,  /* B */
};

/* Shared by tiles_note_map_get_note() and tiles_note_map_is_root_pad()
 * below -- see tiles_note_map_get_note()'s own comment for the physical
 * layout this derives from. */
static uint8_t pad_degree(const tiles_pad_config_t *cfg) {
    uint8_t musical_row = (uint8_t)(4u - cfg->row);
    return (uint8_t)(musical_row * 6u + (cfg->col - 1u));
}

/* ---- Scale interval tables ---------------------------------------------
 * Semitone offsets from the tonic, standard/documented definitions, not
 * invented here -- see note_map.h's own comment on why these specific 18
 * plus chromatic. One static array + a {pointer, count} lookup per scale
 * rather than one giant padded table, since scale length genuinely varies
 * (5 to 12 notes/octave) and tiles_note_map_get_note() needs the real
 * count to fold degree 0-23 into octaves correctly (see scale_table()
 * below). */
static const int8_t CHROMATIC_INTERVALS[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static const int8_t IONIAN_INTERVALS[] = {0, 2, 4, 5, 7, 9, 11};
static const int8_t DORIAN_INTERVALS[] = {0, 2, 3, 5, 7, 9, 10};
static const int8_t PHRYGIAN_INTERVALS[] = {0, 1, 3, 5, 7, 8, 10};
static const int8_t LYDIAN_INTERVALS[] = {0, 2, 4, 6, 7, 9, 11};
static const int8_t MIXOLYDIAN_INTERVALS[] = {0, 2, 4, 5, 7, 9, 10};
static const int8_t AEOLIAN_INTERVALS[] = {0, 2, 3, 5, 7, 8, 10};
static const int8_t LOCRIAN_INTERVALS[] = {0, 1, 3, 5, 6, 8, 10};
/* Major blues: 1, 2, b3, 3, 5, 6. */
static const int8_t BLUES_MAJOR_INTERVALS[] = {0, 2, 3, 4, 7, 9};
/* Minor blues: 1, b3, 4, b5, 5, b7 -- the "standard" blues scale. */
static const int8_t BLUES_MINOR_INTERVALS[] = {0, 3, 5, 6, 7, 10};
/* Double Harmonic Major, the scale most music software labels "Arabian"/
 * "Arabic": 1, b2, 3, 4, 5, b6, 7. */
static const int8_t ARABIAN_INTERVALS[] = {0, 1, 4, 5, 7, 8, 11};
/* Whole-half (octatonic) diminished. */
static const int8_t DIMINISHED_INTERVALS[] = {0, 2, 3, 5, 6, 8, 9, 11};
/* Half-whole (octatonic) diminished -- the other starting rotation,
 * hence "combination." */
static const int8_t COMBINATION_DIMINISHED_INTERVALS[] = {0, 1, 3, 4, 6, 7, 9, 10};
static const int8_t PENTATONIC_MAJOR_INTERVALS[] = {0, 2, 4, 7, 9};
static const int8_t PENTATONIC_MINOR_INTERVALS[] = {0, 3, 5, 7, 10};
/* "Suspended" pentatonic -- the 2nd mode of the major pentatonic above. */
static const int8_t EGYPTIAN_INTERVALS[] = {0, 2, 5, 7, 10};
static const int8_t WHOLE_TONE_INTERVALS[] = {0, 2, 4, 6, 8, 10};
/* In scale / Miyako-bushi, a common Japanese pentatonic. */
static const int8_t JAPANESE_MIYAKOBUSHI_INTERVALS[] = {0, 1, 5, 7, 8};
/* Hindustani Todi thaat: Sa, komal Re, komal Ga, tivra Ma, Pa, komal Dha,
 * shuddha Ni. */
static const int8_t RAGA_TODI_INTERVALS[] = {0, 1, 3, 6, 7, 8, 11};

typedef struct {
    const int8_t *intervals;
    uint8_t count; /* 0 for a scale with no table yet (the CUSTOM_* placeholders) */
} tiles_scale_table_t;

#define SCALE_TABLE(arr) {(arr), (uint8_t)(sizeof(arr) / sizeof((arr)[0]))}

static tiles_scale_table_t scale_table(tiles_scale_mode_t scale) {
    switch (scale) {
    case TILES_SCALE_IONIAN:
        return (tiles_scale_table_t)SCALE_TABLE(IONIAN_INTERVALS);
    case TILES_SCALE_DORIAN:
        return (tiles_scale_table_t)SCALE_TABLE(DORIAN_INTERVALS);
    case TILES_SCALE_PHRYGIAN:
        return (tiles_scale_table_t)SCALE_TABLE(PHRYGIAN_INTERVALS);
    case TILES_SCALE_LYDIAN:
        return (tiles_scale_table_t)SCALE_TABLE(LYDIAN_INTERVALS);
    case TILES_SCALE_MIXOLYDIAN:
        return (tiles_scale_table_t)SCALE_TABLE(MIXOLYDIAN_INTERVALS);
    case TILES_SCALE_AEOLIAN:
        return (tiles_scale_table_t)SCALE_TABLE(AEOLIAN_INTERVALS);
    case TILES_SCALE_LOCRIAN:
        return (tiles_scale_table_t)SCALE_TABLE(LOCRIAN_INTERVALS);
    case TILES_SCALE_BLUES_MAJOR:
        return (tiles_scale_table_t)SCALE_TABLE(BLUES_MAJOR_INTERVALS);
    case TILES_SCALE_BLUES_MINOR:
        return (tiles_scale_table_t)SCALE_TABLE(BLUES_MINOR_INTERVALS);
    case TILES_SCALE_ARABIAN:
        return (tiles_scale_table_t)SCALE_TABLE(ARABIAN_INTERVALS);
    case TILES_SCALE_DIMINISHED:
        return (tiles_scale_table_t)SCALE_TABLE(DIMINISHED_INTERVALS);
    case TILES_SCALE_COMBINATION_DIMINISHED:
        return (tiles_scale_table_t)SCALE_TABLE(COMBINATION_DIMINISHED_INTERVALS);
    case TILES_SCALE_PENTATONIC_MAJOR:
        return (tiles_scale_table_t)SCALE_TABLE(PENTATONIC_MAJOR_INTERVALS);
    case TILES_SCALE_PENTATONIC_MINOR:
        return (tiles_scale_table_t)SCALE_TABLE(PENTATONIC_MINOR_INTERVALS);
    case TILES_SCALE_EGYPTIAN:
        return (tiles_scale_table_t)SCALE_TABLE(EGYPTIAN_INTERVALS);
    case TILES_SCALE_WHOLE_TONE:
        return (tiles_scale_table_t)SCALE_TABLE(WHOLE_TONE_INTERVALS);
    case TILES_SCALE_JAPANESE_MIYAKOBUSHI:
        return (tiles_scale_table_t)SCALE_TABLE(JAPANESE_MIYAKOBUSHI_INTERVALS);
    case TILES_SCALE_RAGA_TODI:
        return (tiles_scale_table_t)SCALE_TABLE(RAGA_TODI_INTERVALS);
    case TILES_SCALE_CHROMATIC:
        return (tiles_scale_table_t)SCALE_TABLE(CHROMATIC_INTERVALS);
    default:
        /* TILES_SCALE_CUSTOM_1..9 -- no table yet, see this file's
         * header + tiles_note_map_scale_is_defined() below. (PHRYGIAN/
         * LOCRIAN/COMBINATION_DIMINISHED/RAGA_TODI still have their own
         * real cases above and still resolve correctly if ever called
         * with directly -- they're just no longer placed anywhere in
         * SCALE_GRID_ORDER below, so the picker itself can't reach them
         * -- see note_map.h's own enum comment for why their cases
         * were kept rather than deleted.) */
        return (tiles_scale_table_t){NULL, 0u};
    }
}

/* Grid slot 1-24 -> scale. Real feedback, this round: "we have to many
 * scales on the scale selector and its kinda overwhelming... rearange so
 * we start with chrommatic, major, minor and then the rest. remove the
 * one on pad: 13, 19, the 2 modes you sugested as well" -- "the 2 modes
 * you suggested" being Locrian and Phrygian (see this file's own header
 * enum comment for the full "attractive vs not" reasoning), and pads 13/
 * 19 (in the PREVIOUS order) being Combination Diminished and Raga Todi.
 * Chromatic stays slot 1 (from an earlier round's own fix, real
 * feedback: "add the first mode as chromatic... so we can return to
 * chromatic mode"), then Ionian ("major") and Aeolian ("minor") lead --
 * the two most commonly reached-for scales -- before the remaining 12
 * named scales in their previous relative order. Dropping 4 named scales
 * frees 4 grid slots; rather than shrink the grid or leave gaps, those
 * went to 3 new reserved custom placeholders (CUSTOM_7/8/9, alongside
 * the existing 6) so the grid stays a full 24 slots -- see
 * TILES_NOTE_MAP_NUM_SCALE_GRID_SLOTS. */
static const tiles_scale_mode_t SCALE_GRID_ORDER[TILES_NOTE_MAP_NUM_SCALE_GRID_SLOTS] = {
    TILES_SCALE_CHROMATIC,
    TILES_SCALE_IONIAN,
    TILES_SCALE_AEOLIAN,
    TILES_SCALE_DORIAN,
    TILES_SCALE_LYDIAN,
    TILES_SCALE_MIXOLYDIAN,
    TILES_SCALE_BLUES_MAJOR,
    TILES_SCALE_BLUES_MINOR,
    TILES_SCALE_ARABIAN,
    TILES_SCALE_DIMINISHED,
    TILES_SCALE_PENTATONIC_MAJOR,
    TILES_SCALE_PENTATONIC_MINOR,
    TILES_SCALE_EGYPTIAN,
    TILES_SCALE_WHOLE_TONE,
    TILES_SCALE_JAPANESE_MIYAKOBUSHI,
    TILES_SCALE_CUSTOM_1,
    TILES_SCALE_CUSTOM_2,
    TILES_SCALE_CUSTOM_3,
    TILES_SCALE_CUSTOM_4,
    TILES_SCALE_CUSTOM_5,
    TILES_SCALE_CUSTOM_6,
    TILES_SCALE_CUSTOM_7,
    TILES_SCALE_CUSTOM_8,
    TILES_SCALE_CUSTOM_9,
};

tiles_scale_mode_t tiles_note_map_scale_for_grid_slot(uint8_t slot_1_to_24) {
    if (slot_1_to_24 < 1u || slot_1_to_24 > TILES_NOTE_MAP_NUM_SCALE_GRID_SLOTS) {
        return TILES_SCALE_CHROMATIC;
    }
    return SCALE_GRID_ORDER[slot_1_to_24 - 1u];
}

bool tiles_note_map_scale_is_defined(tiles_scale_mode_t scale) {
    return scale_table(scale).count > 0u;
}

void tiles_note_map_set_scale(tiles_scale_mode_t scale) {
    s_scale = scale;
}

tiles_scale_mode_t tiles_note_map_get_scale(void) {
    return s_scale;
}

void tiles_note_map_set_octave_shift(int8_t octaves) {
    if (octaves > (int8_t)TILES_NOTE_MAP_MAX_OCTAVE_SHIFT) {
        octaves = (int8_t)TILES_NOTE_MAP_MAX_OCTAVE_SHIFT;
    }
    if (octaves < -(int8_t)TILES_NOTE_MAP_MAX_OCTAVE_SHIFT) {
        octaves = -(int8_t)TILES_NOTE_MAP_MAX_OCTAVE_SHIFT;
    }
    s_octave_shift = octaves;
}

int8_t tiles_note_map_get_octave_shift(void) {
    return s_octave_shift;
}

void tiles_note_map_set_key_offset(int8_t offset) {
    int wrapped = (int)offset % 12;
    if (wrapped < 0) {
        wrapped += 12;
    }
    s_key_offset = (int8_t)wrapped;
}

int8_t tiles_note_map_get_key_offset(void) {
    return s_key_offset;
}

/* Folds a pad's linear 0-23 degree into the currently selected scale's
 * own interval table, octave-doubling every `table.count` degrees --
 * shared by tiles_note_map_get_note() and tiles_note_map_is_root_pad()
 * below so both always agree on exactly the same scale, including the
 * CUSTOM_* fallback. */
static tiles_scale_table_t current_scale_table(void) {
    tiles_scale_table_t table = scale_table(s_scale);
    if (table.count == 0u) {
        /* Selected scale has no real table (a reserved custom slot --
         * see note_map.h's own comment) -- fall back to chromatic rather
         * than dividing by zero or producing garbage. */
        table = scale_table(TILES_SCALE_CHROMATIC);
    }
    return table;
}

/* Folds `degree` through the currently selected scale's own interval
 * table, exactly like tiles_note_map_get_note()'s own tail end -- shared
 * so chord mode's melody notes and its chord triads (which need this
 * done up to 3 times, once per chord tone) both agree with normal
 * scale-based play on what a given degree actually sounds like. */
static int note_for_scale_degree(uint8_t degree) {
    tiles_scale_table_t table = current_scale_table();
    uint8_t octave_num = (uint8_t)(degree / table.count);
    uint8_t degree_in_octave = (uint8_t)(degree % table.count);
    int interval = (int)octave_num * 12 + (int)table.intervals[degree_in_octave];
    return (int)TILES_NOTE_MAP_BASE_NOTE + interval + (int)s_octave_shift * 12 + (int)s_key_offset;
}

void tiles_note_map_get_chord_notes(uint8_t logical_pad, uint8_t out_notes[TILES_NOTE_MAP_CHORD_NUM_NOTES]) {
    const tiles_pad_config_t *cfg = board_pad_config(logical_pad);
    if (!s_chord_mode_active || cfg == NULL || cfg->col > CHORD_REGION_MAX_COL) {
        for (uint8_t i = 0; i < TILES_NOTE_MAP_CHORD_NUM_NOTES; i++) {
            out_notes[i] = 0u;
        }
        return;
    }
    uint8_t root_degree = chord_mode_degree(cfg);
    const uint8_t degree_steps[TILES_NOTE_MAP_CHORD_NUM_NOTES] = {0u, CHORD_THIRD_DEGREE_STEP, CHORD_FIFTH_DEGREE_STEP};
    for (uint8_t i = 0; i < TILES_NOTE_MAP_CHORD_NUM_NOTES; i++) {
        int note = note_for_scale_degree((uint8_t)(root_degree + degree_steps[i])) - CHORD_OCTAVE_DOWN_SEMITONES;
        if (note < 0) {
            note = 0;
        }
        if (note > 127) {
            note = 127;
        }
        out_notes[i] = (uint8_t)note;
    }
}

uint8_t tiles_note_map_get_note(uint8_t logical_pad) {
    const tiles_pad_config_t *cfg = board_pad_config(logical_pad);
    if (cfg == NULL) {
        return 0u;
    }

    if (s_guitar_mode_active) {
        /* A completely different note-mapping shape from the scale system
         * below -- see this file's own "Guitar/bass fret mode" section. */
        return guitar_note_for_pad(cfg);
    }

    if (s_chord_mode_active) {
        /* Chord-region pads (col 1-2) don't really have a single "note"
         * -- see note_map.h's own "Chord mode" section, real play there
         * goes through tiles_note_map_get_chord_notes() instead, never
         * this function. This still returns something sane (this
         * region's own root, at melodic pitch, no octave-down) purely so
         * a stray call here can't return garbage; nothing that actually
         * plays a sound is expected to use it for a chord-region pad. */
        uint8_t degree = chord_mode_degree(cfg);
        int note = note_for_scale_degree(degree);
        if (note < 0) {
            note = 0;
        }
        if (note > 127) {
            note = 127;
        }
        return (uint8_t)note;
    }

    /* Row 4 (bottom) is musical row 0, row 1 (top) is musical row 3 --
     * i.e. the physical grid is walked bottom-to-top. Within a row,
     * columns 1-6 walk left-to-right. degree 0..23 is this pad's
     * position in that bottom-to-top, left-to-right sweep. */
    uint8_t degree = pad_degree(cfg);
    int note = note_for_scale_degree(degree);
    if (note < 0) {
        note = 0;
    }
    if (note > 127) {
        note = 127;
    }
    return (uint8_t)note;
}

bool tiles_note_map_is_root_pad(uint8_t logical_pad) {
    const tiles_pad_config_t *cfg = board_pad_config(logical_pad);
    if (cfg == NULL) {
        return false;
    }
    /* Generalizes the old "degree mod 12 == 0" chromatic-only check to
     * any scale length: a scale's own interval table always starts at
     * 0 (the tonic, by construction -- see the tables above), so the
     * root repeats exactly every table.count degrees, independent of
     * key_offset (it cancels out, since transposing shifts every pad's
     * note by the same amount) -- see note_map.h's own comment on how
     * many pads that works out to per scale. Chord mode's melody
     * sub-grid uses its own narrower degree sweep (chord_mode_degree())
     * -- pad_degree()'s 6-wide formula would give a wrong answer for a
     * 4-wide region, same reason tiles_note_map_get_note() branches
     * here too. Chord-region pads don't really have a "root pad" of
     * their own (the whole strip renders one solid color regardless --
     * see tiles_note_map_is_chord_region_pad()), so which specific
     * formula runs for them doesn't matter in practice; using the same
     * one keeps this function total and simple rather than adding a
     * third branch for a case nothing ever looks at. */
    uint8_t degree = s_chord_mode_active ? chord_mode_degree(cfg) : pad_degree(cfg);
    return (degree % current_scale_table().count) == 0u;
}

bool tiles_note_map_is_natural_pad(uint8_t logical_pad) {
    /* tiles_note_map_get_note() already returns 0 (pitch class C,
     * natural) for an out-of-range pad, matching this function's own
     * documented default -- no separate out-of-range check needed. */
    uint8_t note = tiles_note_map_get_note(logical_pad);
    return s_pitch_class_is_natural[note % 12u];
}
