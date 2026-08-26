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

    int interval;
    switch (s_scale) {
        case TILES_SCALE_CHROMATIC:
        default:
            /* 1 pad = 1 semitone; no scale-table folding needed. */
            interval = (int)degree;
            break;
    }

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
    /* note(pad) mod 12 == (BASE_NOTE + degree + key_offset) mod 12, and
     * the root's own pitch class is (BASE_NOTE + key_offset) mod 12 --
     * the two are equal exactly when degree mod 12 == 0, independent of
     * key_offset (it cancels out, since transposing shifts every pad's
     * note by the same amount). Degree spans 0-23 across the grid's two
     * octaves, so this is true for exactly 2 of the 24 pads. */
    return (pad_degree(cfg) % 12u) == 0u;
}

bool tiles_note_map_is_natural_pad(uint8_t logical_pad) {
    /* tiles_note_map_get_note() already returns 0 (pitch class C,
     * natural) for an out-of-range pad, matching this function's own
     * documented default -- no separate out-of-range check needed. */
    uint8_t note = tiles_note_map_get_note(logical_pad);
    return s_pitch_class_is_natural[note % 12u];
}
