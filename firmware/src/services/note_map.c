#include "note_map.h"

#include <stddef.h>

#include "pad_config.h"

static tiles_scale_mode_t s_scale = TILES_SCALE_CHROMATIC;
static int8_t s_octave_shift = 0;
static int8_t s_key_offset = 0;

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
        /* TILES_SCALE_CUSTOM_1..6 -- no table yet, see this file's
         * header + tiles_note_map_scale_is_defined() below. */
        return (tiles_scale_table_t){NULL, 0u};
    }
}

/* Grid slot 1-24 -> scale, in real feedback's own listed order. Slots
 * 19-24 are the 6 reserved "custom" placeholders. */
static const tiles_scale_mode_t SCALE_GRID_ORDER[TILES_NOTE_MAP_NUM_SCALE_GRID_SLOTS] = {
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

uint8_t tiles_note_map_get_note(uint8_t logical_pad) {
    const tiles_pad_config_t *cfg = board_pad_config(logical_pad);
    if (cfg == NULL) {
        return 0u;
    }

    /* Row 4 (bottom) is musical row 0, row 1 (top) is musical row 3 --
     * i.e. the physical grid is walked bottom-to-top. Within a row,
     * columns 1-6 walk left-to-right. degree 0..23 is this pad's
     * position in that bottom-to-top, left-to-right sweep. */
    uint8_t degree = pad_degree(cfg);

    tiles_scale_table_t table = current_scale_table();
    uint8_t octave_num = (uint8_t)(degree / table.count);
    uint8_t degree_in_octave = (uint8_t)(degree % table.count);
    int interval = (int)octave_num * 12 + (int)table.intervals[degree_in_octave];

    int note = (int)TILES_NOTE_MAP_BASE_NOTE + interval + (int)s_octave_shift * 12 + (int)s_key_offset;
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
     * many pads that works out to per scale. */
    return (pad_degree(cfg) % current_scale_table().count) == 0u;
}

bool tiles_note_map_is_natural_pad(uint8_t logical_pad) {
    /* tiles_note_map_get_note() already returns 0 (pitch class C,
     * natural) for an out-of-range pad, matching this function's own
     * documented default -- no separate out-of-range check needed. */
    uint8_t note = tiles_note_map_get_note(logical_pad);
    return s_pitch_class_is_natural[note % 12u];
}
