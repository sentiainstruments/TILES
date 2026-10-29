#include "harmony.h"

#include "board_layout.h"
#include "board_pins.h"
#include "buttons.h"
#include "expression.h"
#include "hall.h"
#include "harmony_core.h"
#include "haptics.h"
#include "lighting.h"
#include "midi_channels.h"
#include "midi_out.h"
#include "note_map.h"
#include "pedal.h"
#include "strike_settle.h"
#include "touch.h"

#include <stdbool.h>
#include <string.h>

#define HARMONY_CHANNEL TILES_MIDI_CH_CHORD
/* Before the octave shift: the root of the chord in C3-B3 (thirds clear of
 * the muddy low register), the bass the root in C2-B2. */
#define HARMONY_LOW_NOTE 48u
#define HARMONY_BASS_LOW_NOTE 36u
#define HARMONY_MAX_OCTAVE 2
#define HARMONY_MAX_NOTES (TILES_HARMONY_MAX_TONES + 1u) /* + bass */
#define HARMONY_NO_QUALITY 0xFFu
#define HARMONY_NUM_ROOTS 12u

/* Strikes, as on melodic pads: past the strike depth, then a short
 * follow-through so the velocity sees how deep the hit went (firing at the
 * threshold itself read every chord soft), cut short once the key stops
 * going down (strike_settle.h). Expression.c's numbers. */
/* The right-hand pads act on a press (Hall depth), not on touch, so a
 * resting finger picks nothing; they re-arm once the key comes up. */
#define HARMONY_MOD_PRESS_DEPTH TILES_EXPRESSION_MIN_STRIKE_DEPTH_DELTA
/* Strum (pedal down): a touch that stays light this long plucks; a press
 * within it is a chord instead (melodic harmonics' confirm time). A pluck
 * sounds this long, at this share of the chord's velocity. First guesses. */
#define HARMONY_STRUM_CONFIRM_MS 40u
#define HARMONY_STRUM_NOTE_MS 400u
#define HARMONY_STRUM_VELOCITY_SHARE 0.8f
#define HARMONY_STRUM_MIN_VELOCITY 30u

/* Light levels (fractions of full, before the power ceiling): first guesses. */
#define HARMONY_DIM_LEVEL 0.3f
#define HARMONY_DEFAULT_QUALITY_LEVEL 0.6f /* major while no quality is set */
#define HARMONY_CHORD_TONE_LEVEL 0.6f
#define HARMONY_STRUM_PAD_LEVEL 0.12f /* an out-of-key pad that still strums */
#define HARMONY_OCTAVE_LED_STEP 0.5f

/* Columns 4-6: voicing teal, quality amber, extensions coral. */
static const float COLUMN_RGB[3][3] = {
    {0.0f, 0.7f, 0.6f},
    {1.0f, 0.45f, 0.0f},
    {1.0f, 0.25f, 0.15f},
};
/* Column 6's extension per row (row 4 is the bottom; index 0 unused). */
static const uint8_t EXT_FOR_ROW[TILES_GRID_MAX_ROW + 1u] = {0u, TILES_HARMONY_EXT_9, TILES_HARMONY_EXT_MAJ7,
                                                            TILES_HARMONY_EXT_7, TILES_HARMONY_EXT_6};

static int8_t s_octave; /* -2..+2 */
static bool s_prev_touched[TILES_NUM_PADS];
static bool s_prev_minus, s_prev_plus;

/* Root strikes: touch start, peak depth, when the strike depth was crossed. */
static uint32_t s_touch_start_ms[TILES_NUM_PADS];
static float s_peak_depth[TILES_NUM_PADS];
static bool s_awaiting_strike[TILES_NUM_PADS];
static bool s_strike_crossed[TILES_NUM_PADS];
static uint32_t s_crossed_ms[TILES_NUM_PADS];
static tiles_strike_settle_t s_settle[TILES_NUM_PADS];
static uint32_t s_last_sample_ms[TILES_NUM_PADS]; /* the Hall sample last fed to s_settle */

/* Right-hand pads: pressed (by depth), and when, for "held while striking". */
static bool s_mod_pressed[TILES_NUM_PADS];
static uint32_t s_mod_pressed_ms[TILES_NUM_PADS];

/* Voicing (latched for good): 0 root position, 1 first, 2 second, 3 open.
 * Quality and extensions latch onto the sounding note (s_note_*) until a new
 * note is pressed; set while nothing sounds they wait for the next note
 * (s_armed_*). */
static uint8_t s_voicing;
static uint8_t s_note_quality, s_armed_quality;
static uint8_t s_note_ext, s_armed_ext;

/* The sounding chord. s_sounding_pad is the root pad held down (0 = none);
 * s_chord_ringing covers a chord the pedal holds after its pad lifted. */
static uint8_t s_sounding_pad;
static bool s_chord_ringing;
static uint8_t s_sounding_root_pc;
static uint8_t s_sounding_velocity;
static uint16_t s_sounding_signature;
static uint8_t s_notes[HARMONY_MAX_NOTES];
static uint8_t s_note_count;
static uint8_t s_last_pressure;

/* Strum: the last chord's pitch classes and lowest note, a pending light
 * touch per root pad, and the pluck sounding on each. */
static uint16_t s_strum_pcs;
static uint8_t s_strum_from;
static bool s_strum_pending[TILES_NUM_PADS];
static uint32_t s_strum_pending_ms[TILES_NUM_PADS];
static uint8_t s_pluck_note[TILES_NUM_PADS]; /* 0 = none */
static uint32_t s_pluck_ends_ms[TILES_NUM_PADS];

static bool pad_row_col(uint8_t pad, uint8_t *row, uint8_t *col) {
    for (uint8_t r = 1u; r <= TILES_GRID_MAX_ROW; r++) {
        for (uint8_t c = TILES_GRID_MIN_COL; c <= TILES_GRID_MAX_COL; c++) {
            if (board_pad_for_row_col(r, c) == pad) {
                *row = r;
                *col = c;
                return true;
            }
        }
    }
    return false;
}

/* A root pad's pitch class; also its strum step (both count from the
 * bottom-left, three per row). */
static uint8_t root_pc_for(uint8_t row, uint8_t col) {
    return (uint8_t)((TILES_GRID_MAX_ROW - row) * 3u + (col - 1u));
}

/* The current scale's pitch classes (key included); all twelve when
 * chromatic. A pitch class is in the scale if quantizing leaves it be. */
static uint16_t scale_mask(void) {
    if (tiles_note_map_get_scale() == TILES_SCALE_CHROMATIC) {
        return 0x0FFFu;
    }
    uint16_t mask = 0u;
    for (uint8_t pc = 0; pc < 12u; pc++) {
        if (tiles_note_map_quantize_to_scale((uint8_t)(60u + pc)) == 60u + pc) {
            mask |= (uint16_t)(1u << pc);
        }
    }
    return mask != 0u ? mask : 0x0FFFu;
}

static bool chord_active(void) {
    return s_sounding_pad != 0u || s_chord_ringing;
}

/* What the shown quality and extensions are: the sounding note's, or the
 * ones waiting for the next note. */
static uint8_t shown_quality(void) {
    return chord_active() ? s_note_quality : s_armed_quality;
}

static uint8_t shown_ext(void) {
    return chord_active() ? s_note_ext : s_armed_ext;
}

static uint16_t signature(void) {
    return (uint16_t)((s_note_quality << 8) | (s_note_ext << 4) | s_voicing);
}

/* The chord for root_pc from the sounding note's quality and extensions at
 * the latched voicing, plus the bass. */
static uint8_t build_chord(uint8_t root_pc, uint8_t *out) {
    int8_t intervals[TILES_HARMONY_MAX_TONES];
    uint8_t n = tiles_harmony_built_chord(s_note_quality == HARMONY_NO_QUALITY ? 0u : s_note_quality, s_note_ext,
                                          intervals);
    int shift = 12 * s_octave;
    tiles_harmony_voicing_t v;
    tiles_harmony_voice_fixed(root_pc, intervals, n, s_voicing < 3u ? s_voicing : 0u, s_voicing == 3u,
                              (uint8_t)((int)HARMONY_LOW_NOTE + shift), &v);
    s_sounding_signature = signature();
    if (v.count == 0u) {
        return 0u;
    }
    uint8_t count = 0u;
    out[count++] = (uint8_t)((int)HARMONY_BASS_LOW_NOTE + shift + root_pc);
    for (uint8_t i = 0; i < v.count; i++) {
        out[count++] = v.notes[i];
    }
    return count;
}

/* The strum follows the last chord built. */
static void remember_for_strum(void) {
    s_strum_pcs = 0u;
    for (uint8_t i = 0; i < s_note_count; i++) {
        s_strum_pcs |= (uint16_t)(1u << (s_notes[i] % 12u));
    }
    s_strum_from = s_note_count > 0u ? s_notes[0] : 0u;
}

static void notes_off(void) {
    for (uint8_t i = 0; i < s_note_count; i++) {
        tiles_midi_note_off(HARMONY_CHANNEL, s_notes[i], 0u);
    }
    s_note_count = 0u;
}

static void chord_off(void) {
    notes_off();
    if (s_last_pressure != 0u) {
        tiles_midi_send_channel_pressure(HARMONY_CHANNEL, 0u);
        s_last_pressure = 0u;
    }
    if (s_sounding_pad != 0u) {
        tiles_haptics_stop(s_sounding_pad);
    }
    s_sounding_pad = 0u;
    s_chord_ringing = false;
}

/* The newest held pad of a right-hand column (by press time), or 0. */
static uint8_t newest_held(uint8_t col) {
    uint8_t best = 0u;
    for (uint8_t row = 1u; row <= TILES_GRID_MAX_ROW; row++) {
        uint8_t pad = board_pad_for_row_col(row, col);
        if (s_mod_pressed[pad - 1u] &&
            (best == 0u || (int32_t)(s_mod_pressed_ms[pad - 1u] - s_mod_pressed_ms[best - 1u]) > 0)) {
            best = pad;
        }
    }
    return best;
}

static void chord_strike(uint8_t pad, uint8_t root_pc, uint8_t velocity) {
    chord_off();
    /* A new note: the last note's quality and extensions let go. It takes
     * the ones waiting for it, plus any held down right now. */
    uint8_t held_quality = newest_held(5u);
    uint8_t row, col;
    s_note_quality = s_armed_quality;
    if (held_quality != 0u && pad_row_col(held_quality, &row, &col)) {
        s_note_quality = (uint8_t)(TILES_GRID_MAX_ROW - row);
    }
    s_note_ext = s_armed_ext;
    for (uint8_t r = 1u; r <= TILES_GRID_MAX_ROW; r++) {
        if (s_mod_pressed[board_pad_for_row_col(r, 6u) - 1u]) {
            s_note_ext |= EXT_FOR_ROW[r];
        }
    }
    s_armed_quality = HARMONY_NO_QUALITY;
    s_armed_ext = 0u;

    s_note_count = build_chord(root_pc, s_notes);
    tiles_midi_out_batch_begin(); /* the whole chord in one USB transfer */
    for (uint8_t i = 0; i < s_note_count; i++) {
        tiles_midi_note_on(HARMONY_CHANNEL, s_notes[i], velocity);
    }
    tiles_midi_out_batch_end();
    remember_for_strum();
    s_sounding_pad = pad;
    s_sounding_root_pc = root_pc;
    s_sounding_velocity = velocity;
    s_last_pressure = 0u;
    tiles_haptics_trigger_kick(pad, velocity);
}

/* A change under a sounding chord: notes the two share keep sounding, the
 * rest change. */
static void chord_rebuild(void) {
    uint8_t next[HARMONY_MAX_NOTES];
    uint8_t n = build_chord(s_sounding_root_pc, next);
    tiles_midi_out_batch_begin();
    for (uint8_t i = 0; i < s_note_count; i++) {
        if (memchr(next, s_notes[i], n) == NULL) {
            tiles_midi_note_off(HARMONY_CHANNEL, s_notes[i], 0u);
        }
    }
    for (uint8_t i = 0; i < n; i++) {
        if (memchr(s_notes, next[i], s_note_count) == NULL) {
            tiles_midi_note_on(HARMONY_CHANNEL, next[i], s_sounding_velocity);
        }
    }
    tiles_midi_out_batch_end();
    memcpy(s_notes, next, n);
    s_note_count = n;
    remember_for_strum();
}

/* The root pad lifted: the chord ends, unless the pedal holds notes (HOLD
 * style; in SYNTH style the synth sustains on its own CC64). */
static void chord_release(void) {
    if (tiles_pedal_is_holding_notes()) {
        if (s_sounding_pad != 0u) {
            tiles_haptics_stop(s_sounding_pad);
        }
        s_sounding_pad = 0u;
        s_chord_ringing = true;
        return;
    }
    chord_off();
}

static void pluck_off(uint8_t i) {
    if (s_pluck_note[i] != 0u) {
        tiles_midi_note_off(HARMONY_CHANNEL, s_pluck_note[i], 0u);
        s_pluck_note[i] = 0u;
    }
}

static void pluck(uint8_t pad, uint8_t step, uint32_t now_ms) {
    uint8_t i = (uint8_t)(pad - 1u);
    uint8_t note = tiles_harmony_strum_note(s_strum_pcs, s_strum_from, step);
    if (note == 0u) {
        return;
    }
    pluck_off(i);
    uint32_t v = (uint32_t)((float)s_sounding_velocity * HARMONY_STRUM_VELOCITY_SHARE);
    uint8_t velocity = (uint8_t)(v < HARMONY_STRUM_MIN_VELOCITY ? HARMONY_STRUM_MIN_VELOCITY : (v > 127u ? 127u : v));
    tiles_midi_note_on(HARMONY_CHANNEL, note, velocity);
    s_pluck_note[i] = note;
    s_pluck_ends_ms[i] = now_ms + HARMONY_STRUM_NOTE_MS;
}

void tiles_harmony_init(void) {
    s_octave = 0;
    s_voicing = 0u;
    s_note_quality = s_armed_quality = HARMONY_NO_QUALITY;
    s_note_ext = s_armed_ext = 0u;
    s_sounding_pad = 0u;
    s_chord_ringing = false;
    s_note_count = 0u;
    s_last_pressure = 0u;
    s_strum_pcs = 0u;
    tiles_harmony_enter();
}

void tiles_harmony_enter(void) {
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        uint8_t i = (uint8_t)(pad - 1u);
        s_prev_touched[i] = tiles_touch_is_touched(pad);
        s_awaiting_strike[i] = false;
        s_mod_pressed[i] = s_prev_touched[i]; /* a finger already down isn't a new press */
        s_strum_pending[i] = false;
    }
    s_prev_minus = tiles_button_is_pressed(TILES_MINUS_BUTTON_ID);
    s_prev_plus = tiles_button_is_pressed(TILES_PLUS_BUTTON_ID);
}

void tiles_harmony_leave(void) {
    chord_off();
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        pluck_off(i);
    }
}

/* A right-hand pad pressed (not just touched): voicing for good; quality
 * and extensions onto the sounding note, else waiting for the next one.
 * The same quality or extension again takes it off. */
static void mod_press(uint8_t row, uint8_t col) {
    uint8_t index = (uint8_t)(TILES_GRID_MAX_ROW - row);
    bool on_note = chord_active();
    if (col == 4u) {
        s_voicing = index;
    } else if (col == 5u) {
        uint8_t *q = on_note ? &s_note_quality : &s_armed_quality;
        *q = (*q == index) ? HARMONY_NO_QUALITY : index;
    } else {
        uint8_t *e = on_note ? &s_note_ext : &s_armed_ext;
        *e ^= EXT_FOR_ROW[row];
    }
}

static void handle_root(uint8_t pad, uint8_t root_pc, bool in_key, bool touched, bool was, bool strum,
                        uint32_t now_ms) {
    uint8_t i = (uint8_t)(pad - 1u);
    float depth = (float)tiles_hall_get_depth(pad);
    if (touched && !was) {
        s_touch_start_ms[i] = now_ms;
        s_peak_depth[i] = depth;
        s_awaiting_strike[i] = in_key;
        s_strike_crossed[i] = false;
        s_strum_pending[i] = strum && pad != s_sounding_pad;
        s_strum_pending_ms[i] = now_ms;
        if (pad != s_sounding_pad) {
            tiles_haptics_trigger_touch_pulse(pad);
        }
    }

    /* Strike: past the strike depth, then the follow-through; a lift inside
     * it commits with the depth reached. */
    if (s_awaiting_strike[i]) {
        if (touched && depth > s_peak_depth[i]) {
            s_peak_depth[i] = depth;
        }
        uint32_t sample_ms = tiles_hall_get_sample(pad).sample_time_ms;
        if (!s_strike_crossed[i] && s_peak_depth[i] >= TILES_EXPRESSION_MIN_STRIKE_DEPTH_DELTA) {
            s_strike_crossed[i] = true;
            s_crossed_ms[i] = now_ms;
            s_strum_pending[i] = false; /* a real press: a chord, not a pluck */
            tiles_strike_settle_begin(&s_settle[i], s_peak_depth[i], now_ms);
            s_last_sample_ms[i] = sample_ms;
        } else if (s_strike_crossed[i] && touched && sample_ms != s_last_sample_ms[i]) {
            s_last_sample_ms[i] = sample_ms;
            tiles_strike_settle_sample(&s_settle[i], depth, sample_ms);
        }
        if (s_strike_crossed[i] &&
            (!touched || tiles_strike_settle_ready(&s_settle[i], s_peak_depth[i], TILES_EXPRESSION_FULL_STRIKE_DEPTH, now_ms))) {
            uint8_t velocity =
                tiles_expression_velocity_from_strike(s_crossed_ms[i] - s_touch_start_ms[i], s_peak_depth[i]);
            chord_strike(pad, root_pc, velocity);
            s_awaiting_strike[i] = false;
        } else if (!touched) {
            s_awaiting_strike[i] = false; /* a light tap: no chord */
        }
    }

    /* Strum: a touch that stays light for the confirm time (or lifts early)
     * plucks. */
    if (s_strum_pending[i]) {
        if (!strum) {
            s_strum_pending[i] = false;
        } else if (!touched || now_ms - s_strum_pending_ms[i] >= HARMONY_STRUM_CONFIRM_MS) {
            s_strum_pending[i] = false;
            pluck(pad, root_pc, now_ms);
        }
    }

    if (!touched && was && s_sounding_pad == pad) {
        chord_release();
    }
}

void tiles_harmony_track_releases(uint32_t now_ms) {
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        uint8_t i = (uint8_t)(pad - 1u);
        bool touched = tiles_touch_is_touched(pad);
        bool was = s_prev_touched[i];
        s_prev_touched[i] = touched;
        s_awaiting_strike[i] = false; /* a strike in progress is dropped */
        s_strum_pending[i] = false;
        if (!touched) {
            s_mod_pressed[i] = false;
        }
        if (!touched && was && s_sounding_pad == pad) {
            chord_release();
        }
    }
    if (s_chord_ringing && !tiles_pedal_is_holding_notes()) {
        chord_off();
    }
    bool pedal = tiles_pedal_is_sustained();
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        if (s_pluck_note[i] != 0u && (!pedal || (int32_t)(now_ms - s_pluck_ends_ms[i]) >= 0)) {
            pluck_off(i);
        }
    }
}

void tiles_harmony_handle_input(uint32_t now_ms) {
    uint16_t mask = scale_mask();
    bool pedal = tiles_pedal_is_sustained();
    bool strum = pedal && s_strum_pcs != 0u;

    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        uint8_t i = (uint8_t)(pad - 1u), row, col;
        bool touched = tiles_touch_is_touched(pad);
        bool was = s_prev_touched[i];
        s_prev_touched[i] = touched;
        if (!pad_row_col(pad, &row, &col)) {
            continue;
        }
        if (col >= 4u) {
            /* Pressed by depth; re-armed when the key comes up or the finger
             * leaves. */
            uint16_t depth = tiles_hall_get_depth(pad);
            if (!s_mod_pressed[i] && touched && depth >= HARMONY_MOD_PRESS_DEPTH) {
                s_mod_pressed[i] = true;
                s_mod_pressed_ms[i] = now_ms;
                mod_press(row, col);
                tiles_haptics_trigger_touch_pulse(pad);
            } else if (s_mod_pressed[i] && (!touched || depth < TILES_HALL_KEY_UP_DEPTH)) {
                s_mod_pressed[i] = false;
            }
            continue;
        }
        uint8_t root_pc = root_pc_for(row, col);
        handle_root(pad, root_pc, (mask >> root_pc) & 1u, touched, was, strum, now_ms);
    }

    /* A sounding chord follows its latched quality, extensions and the voicing. */
    if (chord_active() && signature() != s_sounding_signature) {
        chord_rebuild();
    }

    /* The pedal let go of a chord it was holding. */
    if (s_chord_ringing && !tiles_pedal_is_holding_notes()) {
        chord_off();
    }

    /* Plucks end on their own, or at once when the pedal comes up. */
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        if (s_pluck_note[i] != 0u && (!pedal || (int32_t)(now_ms - s_pluck_ends_ms[i]) >= 0)) {
            pluck_off(i);
        }
    }

    /* Pressure on the held root: channel pressure, scaled like aftertouch,
     * and the pad's buzz follows it. */
    if (s_sounding_pad != 0u) {
        uint32_t full = tiles_expression_get_aftertouch_sensitivity();
        uint32_t scaled = full != 0u ? (uint32_t)tiles_hall_get_depth(s_sounding_pad) * 127u / full : 0u;
        uint8_t pressure = (uint8_t)(scaled > 127u ? 127u : scaled);
        if (pressure != s_last_pressure) {
            s_last_pressure = pressure;
            tiles_midi_send_channel_pressure(HARMONY_CHANNEL, pressure);
            tiles_haptics_set_sustain_level(s_sounding_pad, pressure);
        }
    }

    /* "-" / "+": the chords an octave down / up. */
    bool minus = tiles_button_is_pressed(TILES_MINUS_BUTTON_ID);
    bool plus = tiles_button_is_pressed(TILES_PLUS_BUTTON_ID);
    if (minus && !s_prev_minus && s_octave > -HARMONY_MAX_OCTAVE) {
        s_octave--;
    }
    if (plus && !s_prev_plus && s_octave < HARMONY_MAX_OCTAVE) {
        s_octave++;
    }
    s_prev_minus = minus;
    s_prev_plus = plus;
}

static void scheme_rgb(tiles_scheme_role_t role, float *r, float *g, float *b) {
    uint32_t c = tiles_lighting_get_scheme_color(role);
    if (c == TILES_LIGHTING_COLOR_NONE) {
        c = tiles_lighting_get_scheme_color(TILES_SCHEME_NOTE);
    }
    if (c == TILES_LIGHTING_COLOR_NONE) {
        c = 0x363636u;
    }
    *r = (float)((c >> 16) & 0xFFu) / 255.0f;
    *g = (float)((c >> 8) & 0xFFu) / 255.0f;
    *b = (float)(c & 0xFFu) / 255.0f;
}

void tiles_harmony_render(uint32_t now_ms, float beat_flash, float diamond_led) {
    (void)now_ms;
    uint16_t mask = scale_mask();
    uint8_t key_pc = (uint8_t)tiles_note_map_get_key_offset();
    uint8_t quality = shown_quality();
    uint8_t ext = shown_ext();
    bool strum = tiles_pedal_is_sustained() && s_strum_pcs != 0u;
    /* The sounding chord's pitch classes, to light its notes on the roots. */
    uint16_t chord_pcs = 0u;
    for (uint8_t i = 0; i < s_note_count; i++) {
        chord_pcs |= (uint16_t)(1u << (s_notes[i] % 12u));
    }

    for (uint8_t row = 1u; row <= TILES_GRID_MAX_ROW; row++) {
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            uint8_t pad = board_pad_for_row_col(row, col);
            uint8_t index = (uint8_t)(TILES_GRID_MAX_ROW - row);
            float r = 0.0f, g = 0.0f, b = 0.0f;
            if (col >= 4u) {
                float level = HARMONY_DIM_LEVEL;
                if ((col == 4u && index == s_voicing) || (col == 5u && index == quality) ||
                    (col == 6u && (ext & EXT_FOR_ROW[row]))) {
                    level = 1.0f;
                } else if (col == 5u && index == 0u && quality == HARMONY_NO_QUALITY) {
                    level = HARMONY_DEFAULT_QUALITY_LEVEL;
                }
                r = COLUMN_RGB[col - 4u][0] * level;
                g = COLUMN_RGB[col - 4u][1] * level;
                b = COLUMN_RGB[col - 4u][2] * level;
            } else {
                uint8_t pc = root_pc_for(row, col);
                bool in_key = (mask >> pc) & 1u;
                if (s_pluck_note[pad - 1u] != 0u) {
                    r = COLUMN_RGB[1][0];
                    g = COLUMN_RGB[1][1];
                    b = COLUMN_RGB[1][2]; /* plucked */
                } else if (s_sounding_pad == pad) {
                    r = g = b = 1.0f;
                } else if (!in_key) {
                    /* outside the key: dark; just visible while it strums */
                    if (strum) {
                        r = COLUMN_RGB[1][0] * HARMONY_STRUM_PAD_LEVEL;
                        g = COLUMN_RGB[1][1] * HARMONY_STRUM_PAD_LEVEL;
                        b = COLUMN_RGB[1][2] * HARMONY_STRUM_PAD_LEVEL;
                    }
                } else if (chord_active() && ((chord_pcs >> pc) & 1u)) {
                    r = COLUMN_RGB[1][0] * HARMONY_CHORD_TONE_LEVEL;
                    g = COLUMN_RGB[1][1] * HARMONY_CHORD_TONE_LEVEL;
                    b = COLUMN_RGB[1][2] * HARMONY_CHORD_TONE_LEVEL;
                } else if (pc == key_pc) {
                    scheme_rgb(TILES_SCHEME_ROOT, &r, &g, &b);
                } else if (pc == (key_pc + 7u) % 12u) {
                    scheme_rgb(TILES_SCHEME_FIFTH, &r, &g, &b);
                } else {
                    scheme_rgb(TILES_SCHEME_NOTE, &r, &g, &b);
                }
            }
            tiles_lighting_set_standby_pad_rgb(pad, r, g, b);
        }
    }

    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        float level = 0.0f;
        if (col == TILES_CIRCLE_BUTTON_COL) {
            level = beat_flash;
        } else if (col == TILES_DIAMOND_BUTTON_COL) {
            level = diamond_led;
        } else if (col == TILES_MINUS_BUTTON_COL && s_octave < 0) {
            level = HARMONY_OCTAVE_LED_STEP * (float)(-s_octave);
        } else if (col == TILES_PLUS_BUTTON_COL && s_octave > 0) {
            level = HARMONY_OCTAVE_LED_STEP * (float)s_octave;
        }
        tiles_buttons_set_standby_led(board_button_for_col(col), level);
    }
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        tiles_lighting_set_standby_underglow_rgb(i, TILES_HARMONY_COLOR_R, TILES_HARMONY_COLOR_G, TILES_HARMONY_COLOR_B);
    }
}
