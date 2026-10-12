#include "note_map.h"

#include <stddef.h>

#include "pad_config.h"

#include "pico/platform/sections.h"

/* In __uninitialized_ram (like the crash recorder's ring) so scale, octave
 * and key survive a watchdog reset; ordinary statics are re-initialized
 * on every reset. Such variables can't have initializers, so the
 * fresh-boot defaults are set in tiles_note_map_init() unless this boot is
 * a crash recovery. */
static tiles_scale_mode_t __uninitialized_ram(s_scale);
static int8_t __uninitialized_ram(s_octave_shift);
static int8_t __uninitialized_ram(s_key_offset);

void tiles_note_map_init(bool crash_recovered) {
    if (!crash_recovered) {
        s_scale = TILES_SCALE_CHROMATIC;
        s_octave_shift = 0;
        s_key_offset = 0;
    }
}

/* ---- Bass guitar mode ----------------------------------------------------
 * Standard 4-string bass tuning E1 A1 D2 G2 (MIDI 28/33/38/43), strings a
 * fourth apart. Rows follow TAB notation: highest string on top, so row 1
 * (nearest the buttons) = G2 and row 4 = E1. */
#define GUITAR_NUM_STRINGS 4u
#define GUITAR_VISIBLE_FRETS 6u
#define GUITAR_MAX_FRET 24u /* a common full-size neck */
#define GUITAR_MAX_FRET_OFFSET (GUITAR_MAX_FRET - GUITAR_VISIBLE_FRETS + 1u) /* 19: the window's last column then shows fret 24 */

static const uint8_t GUITAR_STRING_OPEN_NOTE[GUITAR_NUM_STRINGS] = {
    43u, /* row 1 (top) = G2 */
    38u, /* row 2 = D2 */
    33u, /* row 3 = A1 */
    28u, /* row 4 (bottom) = E1 */
};

static bool s_guitar_mode_active;
static uint8_t s_guitar_fret_offset;

/* Standard inlays: single dots at 3/5/7/9 and 15/17/19/21, double dots at
 * 12/24. */
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

/* ---- Chord mode -------------------------------------------------------
 * See note_map.h. Columns 1-2 are the 8-pad chord strip, columns 3-6 the
 * 4x4 melody grid; both use the same bottom-to-top, left-to-right degree
 * sweep as pad_degree(), narrowed to their width. */
#define CHORD_REGION_MAX_COL 2u
#define CHORD_REGION_NUM_COLS 2u
#define CHORD_MELODY_MIN_COL 3u
#define CHORD_MELODY_NUM_COLS 4u
/* Chords sit one octave below the matching melody note. (Two octaves was
 * tried and was too low.) */
#define CHORD_OCTAVE_DOWN_SEMITONES 12
/* Chord tones in SCALE-DEGREE steps (stacked thirds, like a chord organ),
 * extended to the 13th (degree + 8 = the 9th, and so on). Chord quality
 * then follows the selected scale automatically. */
#define CHORD_THIRD_DEGREE_STEP 2u
#define CHORD_FIFTH_DEGREE_STEP 4u
#define CHORD_SEVENTH_DEGREE_STEP 6u
#define CHORD_NINTH_DEGREE_STEP 8u
#define CHORD_ELEVENTH_DEGREE_STEP 10u
#define CHORD_THIRTEENTH_DEGREE_STEP 12u
/* Stacked scale-degree thirds only make real thirds and fifths in a 7-note
 * scale; on chromatic (the boot default) they made "random 3-note groups".
 * So the chord strip always harmonizes against a 7-note scale: see
 * chord_mode_scale_table(). */
#define CHORD_DIATONIC_SCALE_NOTE_COUNT 7u

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

/* Degree within whichever chord-mode region `cfg` is in. Shared by
 * get_note() (melody) and get_chord_notes() (chords). Chord mode only. */
static uint8_t chord_mode_degree(const tiles_pad_config_t *cfg) {
    uint8_t musical_row = (uint8_t)(4u - cfg->row);
    if (cfg->col <= CHORD_REGION_MAX_COL) {
        return (uint8_t)(musical_row * CHORD_REGION_NUM_COLS + (cfg->col - 1u));
    }
    return (uint8_t)(musical_row * CHORD_MELODY_NUM_COLS + (cfg->col - CHORD_MELODY_MIN_COL));
}

/* Natural (white key) per absolute pitch class. (octave_control.c's
 * s_key_table looks similar but is indexed by key offset and carries
 * letters; not the same table.) */
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

/* Position in the bottom-to-top, left-to-right sweep (0..23). */
static uint8_t pad_degree(const tiles_pad_config_t *cfg) {
    uint8_t musical_row = (uint8_t)(4u - cfg->row);
    return (uint8_t)(musical_row * 6u + (cfg->col - 1u));
}

/* ---- Scale interval tables ---------------------------------------------
 * Semitones from the tonic, standard definitions. One array per scale plus
 * a {pointer, count} lookup, since lengths vary (5-12 notes) and folding
 * needs the real count. */
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
/* Minor blues: 1, b3, 4, b5, 5, b7 (the "standard" blues scale). */
static const int8_t BLUES_MINOR_INTERVALS[] = {0, 3, 5, 6, 7, 10};
/* Double Harmonic Major, which most music software calls "Arabian":
 * 1, b2, 3, 4, 5, b6, 7. */
static const int8_t ARABIAN_INTERVALS[] = {0, 1, 4, 5, 7, 8, 11};
/* Whole-half (octatonic) diminished. */
static const int8_t DIMINISHED_INTERVALS[] = {0, 2, 3, 5, 6, 8, 9, 11};
/* Half-whole (octatonic) diminished, the other rotation. */
static const int8_t COMBINATION_DIMINISHED_INTERVALS[] = {0, 1, 3, 4, 6, 7, 9, 10};
static const int8_t PENTATONIC_MAJOR_INTERVALS[] = {0, 2, 4, 7, 9};
static const int8_t PENTATONIC_MINOR_INTERVALS[] = {0, 3, 5, 7, 10};
/* "Suspended" pentatonic, the 2nd mode of major pentatonic. */
static const int8_t EGYPTIAN_INTERVALS[] = {0, 2, 5, 7, 10};
static const int8_t WHOLE_TONE_INTERVALS[] = {0, 2, 4, 6, 8, 10};
/* In scale / Miyako-bushi, a common Japanese pentatonic. */
static const int8_t JAPANESE_MIYAKOBUSHI_INTERVALS[] = {0, 1, 5, 7, 8};
/* Hindustani Todi thaat: Sa, komal Re, komal Ga, tivra Ma, Pa, komal Dha,
 * shuddha Ni. */
static const int8_t RAGA_TODI_INTERVALS[] = {0, 1, 3, 6, 7, 8, 11};

typedef struct {
    const int8_t *intervals;
    uint8_t count; /* 0 for a scale with no table (CUSTOM_*) */
} tiles_scale_table_t;

#define SCALE_TABLE(arr) {(arr), (uint8_t)(sizeof(arr) / sizeof((arr)[0]))}

/* CUSTOM_1..9's tables (count 0 = empty), set by the content store. */
static int8_t s_custom_intervals[TILES_NOTE_MAP_NUM_CUSTOM_SCALES][TILES_NOTE_MAP_MAX_SCALE_NOTES];
static uint8_t s_custom_count[TILES_NOTE_MAP_NUM_CUSTOM_SCALES];

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
        /* CUSTOM_1..9: whatever the content store pushed (count 0 if empty).
         * The four scales off the picker still resolve above if called
         * directly. */
        if (scale >= TILES_SCALE_CUSTOM_1 && scale <= TILES_SCALE_CUSTOM_9) {
            uint8_t i = (uint8_t)(scale - TILES_SCALE_CUSTOM_1);
            return (tiles_scale_table_t){s_custom_intervals[i], s_custom_count[i]};
        }
        return (tiles_scale_table_t){NULL, 0u};
    }
}

bool tiles_note_map_custom_scale_valid(const int8_t *intervals, uint8_t count) {
    if (intervals == NULL || count == 0u || count > TILES_NOTE_MAP_MAX_SCALE_NOTES || intervals[0] != 0) {
        return false;
    }
    for (uint8_t i = 1u; i < count; i++) {
        if (intervals[i] <= intervals[i - 1u] || intervals[i] >= 12) {
            return false;
        }
    }
    return true;
}

bool tiles_note_map_set_custom_scale(uint8_t slot_1_to_9, const int8_t *intervals, uint8_t count) {
    if (slot_1_to_9 < 1u || slot_1_to_9 > TILES_NOTE_MAP_NUM_CUSTOM_SCALES) {
        return false;
    }
    if (count != 0u && !tiles_note_map_custom_scale_valid(intervals, count)) {
        return false;
    }
    uint8_t i = (uint8_t)(slot_1_to_9 - 1u);
    for (uint8_t n = 0u; n < count; n++) {
        s_custom_intervals[i][n] = intervals[n];
    }
    s_custom_count[i] = count;
    return true;
}

uint8_t tiles_note_map_get_custom_scale(uint8_t slot_1_to_9, int8_t *out) {
    if (slot_1_to_9 < 1u || slot_1_to_9 > TILES_NOTE_MAP_NUM_CUSTOM_SCALES) {
        return 0u;
    }
    uint8_t i = (uint8_t)(slot_1_to_9 - 1u);
    for (uint8_t n = 0u; n < s_custom_count[i]; n++) {
        out[n] = s_custom_intervals[i][n];
    }
    return s_custom_count[i];
}

/* Picker order: chromatic (the way back), major, minor, then the rest.
 * Phrygian, Locrian, Combination Diminished and Raga Todi were trimmed;
 * the freed slots are custom placeholders, keeping all 24 pads. */
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

/* Selected scale's table, with the CUSTOM_* chromatic fallback. Shared by
 * get_note(), the root check and quantize, so they always agree. */
static tiles_scale_table_t scale_table_with_fallback(tiles_scale_mode_t scale) {
    tiles_scale_table_t table = scale_table(scale);
    if (table.count == 0u) {
        /* No table (a custom slot): fall back to chromatic. */
        table = scale_table(TILES_SCALE_CHROMATIC);
    }
    return table;
}

static tiles_scale_table_t current_scale_table(void) {
    return scale_table_with_fallback(s_scale);
}

/* Folds `degree` through an explicit table (chord mode needs a table other
 * than the selected one). */
static int note_for_scale_degree_using(tiles_scale_table_t table, uint8_t degree) {
    uint8_t octave_num = (uint8_t)(degree / table.count);
    uint8_t degree_in_octave = (uint8_t)(degree % table.count);
    int interval = (int)octave_num * 12 + (int)table.intervals[degree_in_octave];
    return (int)TILES_NOTE_MAP_BASE_NOTE + interval + (int)s_octave_shift * 12 + (int)s_key_offset;
}

/* Normal play uses the selected scale. */
static int note_for_scale_degree(uint8_t degree) {
    return note_for_scale_degree_using(current_scale_table(), degree);
}

/* The chord strip's table: the selected scale if it has 7 notes (so Dorian,
 * Lydian etc. color the chords), else Ionian. Checked by note count, so a
 * future 7-note custom scale qualifies too. */
static tiles_scale_table_t chord_mode_scale_table(void) {
    tiles_scale_table_t table = current_scale_table();
    if (table.count == CHORD_DIATONIC_SCALE_NOTE_COUNT) {
        return table;
    }
    return scale_table(TILES_SCALE_IONIAN);
}

void tiles_note_map_get_chord_notes(uint8_t logical_pad, uint8_t out_notes[TILES_NOTE_MAP_CHORD_NUM_NOTES]) {
    const tiles_pad_config_t *cfg = board_pad_config(logical_pad);
    if (!s_chord_mode_active || cfg == NULL || cfg->col > CHORD_REGION_MAX_COL) {
        for (uint8_t i = 0; i < TILES_NOTE_MAP_CHORD_NUM_NOTES; i++) {
            out_notes[i] = 0u;
        }
        return;
    }
    tiles_scale_table_t table = chord_mode_scale_table();
    uint8_t root_degree = chord_mode_degree(cfg);
    const uint8_t degree_steps[TILES_NOTE_MAP_CHORD_NUM_NOTES] = {
        0u,
        CHORD_THIRD_DEGREE_STEP,
        CHORD_FIFTH_DEGREE_STEP,
        CHORD_SEVENTH_DEGREE_STEP,
        CHORD_NINTH_DEGREE_STEP,
        CHORD_ELEVENTH_DEGREE_STEP,
        CHORD_THIRTEENTH_DEGREE_STEP};
    for (uint8_t i = 0; i < TILES_NOTE_MAP_CHORD_NUM_NOTES; i++) {
        int note = note_for_scale_degree_using(table, (uint8_t)(root_degree + degree_steps[i])) - CHORD_OCTAVE_DOWN_SEMITONES;
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
        /* Bass guitar layout (see above). */
        return guitar_note_for_pad(cfg);
    }

    if (s_chord_mode_active) {
        /* Chord-strip pads never play through here (they use
         * get_chord_notes()); this returns their root at melodic pitch only so a
         * stray call gets something sane.
         *
         * Melody-grid pads (col 3-6) play here with the SELECTED scale, like a
         * small melodic mode. Only the chord strip needs the 7-note table. */
        uint8_t degree = chord_mode_degree(cfg);
        int note = note_for_scale_degree_using(current_scale_table(), degree);
        if (note < 0) {
            note = 0;
        }
        if (note > 127) {
            note = 127;
        }
        return (uint8_t)note;
    }

    /* Degree = position in the bottom-to-top (row 4 first), left-to-right
     * sweep. */
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

/* True if `note`'s pitch class is in `table` under the current key. */
static bool note_in_scale_table(uint8_t note, tiles_scale_table_t table) {
    int relative = (int)note - (int)TILES_NOTE_MAP_BASE_NOTE - (int)s_key_offset;
    int pitch_class = ((relative % 12) + 12) % 12;
    for (uint8_t i = 0; i < table.count; i++) {
        if ((int)(table.intervals[i] % 12u) == pitch_class) {
            return true;
        }
    }
    return false;
}

/* Outward search by semitone (0, then +-1, +-2, ...), lower neighbor wins
 * a tie (arbitrary but consistent), capped at an octave. The chromatic
 * fallback guarantees a match. */
uint8_t tiles_note_map_quantize_to_scale(uint8_t note) {
    tiles_scale_table_t table = current_scale_table();
    if (note_in_scale_table(note, table)) {
        return note;
    }
    for (int distance = 1; distance <= 12; distance++) {
        int down = (int)note - distance;
        int up = (int)note + distance;
        if (down >= 0 && note_in_scale_table((uint8_t)down, table)) {
            return (uint8_t)down;
        }
        if (up <= 127 && note_in_scale_table((uint8_t)up, table)) {
            return (uint8_t)up;
        }
    }
    return note;
}

bool tiles_note_map_is_root_pad(uint8_t logical_pad) {
    const tiles_pad_config_t *cfg = board_pad_config(logical_pad);
    if (cfg == NULL) {
        return false;
    }
    /* The root repeats every table.count degrees (every table starts at 0);
     * key offset cancels out. Chord mode uses its own degree sweep and the
     * selected scale's count, matching what its melody grid plays. The chord
     * strip is painted one color anyway, so its result doesn't matter. */
    uint8_t degree = s_chord_mode_active ? chord_mode_degree(cfg) : pad_degree(cfg);
    uint8_t scale_note_count = current_scale_table().count;
    return (degree % scale_note_count) == 0u;
}

/* Interval-table check (not degree position), so a scale without a perfect
 * fifth gets no fifth pads. Same degree-to-interval math as
 * note_for_scale_degree_using(). */
bool tiles_note_map_is_fifth_pad(uint8_t logical_pad) {
    const tiles_pad_config_t *cfg = board_pad_config(logical_pad);
    if (cfg == NULL) {
        return false;
    }
    uint8_t degree = s_chord_mode_active ? chord_mode_degree(cfg) : pad_degree(cfg);
    tiles_scale_table_t table = current_scale_table();
    return table.intervals[degree % table.count] == 7;
}

bool tiles_note_map_is_third_pad(uint8_t logical_pad) {
    const tiles_pad_config_t *cfg = board_pad_config(logical_pad);
    if (cfg == NULL) {
        return false;
    }
    uint8_t degree = s_chord_mode_active ? chord_mode_degree(cfg) : pad_degree(cfg);
    tiles_scale_table_t table = current_scale_table();
    bool has_major = false;
    for (uint8_t i = 0u; i < table.count; i++) {
        if (table.intervals[i] == 4) {
            has_major = true;
        }
    }
    return table.intervals[degree % table.count] == (has_major ? 4 : 3);
}

bool tiles_note_map_is_natural_pad(uint8_t logical_pad) {
    /* get_note() returns 0 (C, natural) out of range: the documented default. */
    uint8_t note = tiles_note_map_get_note(logical_pad);
    return s_pitch_class_is_natural[note % 12u];
}
