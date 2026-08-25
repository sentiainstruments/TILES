#include "note_map.h"

#include <stddef.h>

#include "pad_config.h"

static tiles_scale_mode_t s_scale = TILES_SCALE_CHROMATIC;
static int8_t s_octave_shift = 0;
static int8_t s_key_offset = 0;

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
    uint8_t musical_row = (uint8_t)(4u - cfg->row);
    uint8_t degree = (uint8_t)(musical_row * 6u + (cfg->col - 1u));

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
