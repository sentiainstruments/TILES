#include "physics.h"

#include "board_layout.h"
#include "board_pins.h"
#include "buttons.h"
#include "expression.h"
#include "hall.h"
#include "haptics.h"
#include "lighting.h"
#include "midi_channels.h"
#include "midi_out.h"
#include "note_map.h"
#include "octave_control.h"
#include "touch.h"

#include "pico/time.h"

#include <math.h>
#include <string.h>

#define PHYS_CHANNEL TILES_MIDI_CH_BOUNCE

/* Circle + hold this long on a pad clears the mode (drums' hold-to-clear
 * gesture, shortened: one second is deliberate enough). */
#define PHYS_CLEAR_HOLD_MS 1000u
/* Orbits: holding a pad this long moves the sun there instead of
 * launching a planet. */
#define PHYS_SUN_HOLD_MS 400u
/* A degree note's octave before the key and octave shift: C3, as Bouncing
 * notes' lanes. */
#define PHYS_DEGREE_BASE_NOTE 48
/* The beat count is pulled onto a running clock's phase over about this
 * long, without ever running backwards. */
#define PHYS_LOCK_MS 100.0
/* A stall longer than this (a game, deep sleep) isn't caught up on. */
#define PHYS_MAX_STEP_MS 100.0

/* Taps at least this strong (0-127) kick the motor, softer ones tick it;
 * a kick is stopped before its sustain starts (haptics.h). */
#define PHYS_KICK_MIN 80u
#define PHYS_KICK_STOP_MS 50u
#define PHYS_MAX_HAPTICS_PER_STEP 6u

/* The circle view: the scene dims, what a tap removes pulses red; a removal
 * or clear flashes red. First guesses. */
#define PHYS_CIRCLE_DIM 0.3f
#define PHYS_CIRCLE_RED_LO 0.45f
#define PHYS_CIRCLE_RED_HI 0.85f
#define PHYS_RED_FLASH_MS 300u
#define PHYS_SQUARE_LED_LEVEL 1.0f
#define PHYS_PAD_WRITES_PER_SCAN 3u

static tiles_ripples_t s_ripples;
static tiles_sand_t s_sand;
static tiles_orbits_t s_orbits;
static tiles_cradle_t s_cradle;

static double s_beats; /* the modes' beat clock */
static uint64_t s_last_us;
static uint32_t s_last_pulse_count;
static uint64_t s_last_pulse_us;
static float s_ms_per_beat = 500.0f;
static int s_shown = TILES_PHYS_NONE;

/* Notes: one voice per note number, ended when its length is up. */
#define PHYS_VOICES 32u
#define PHYS_NO_NOTE 0xFFu
static uint8_t s_voice_note[PHYS_VOICES];
static uint32_t s_voice_off_ms[PHYS_VOICES];

static uint32_t s_haptic_stop_ms[TILES_NUM_PADS]; /* 0 = none due */
static uint32_t s_red_ms[TILES_PHYS_CELLS];       /* 0 = none */

/* Fingers, per pad (index = pad - 1). */
static bool s_touched[TILES_NUM_PADS];
static bool s_struck[TILES_NUM_PADS];
static bool s_circle_touch[TILES_NUM_PADS]; /* began with circle held */
static bool s_consumed[TILES_NUM_PADS];     /* a hold already acted */
static uint32_t s_touch_ms[TILES_NUM_PADS];
static float s_peak[TILES_NUM_PADS];
static uint8_t s_velocity[TILES_NUM_PADS];
static double s_struck_at[TILES_NUM_PADS];

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static uint8_t pad_of(uint8_t cell) {
    return board_pad_for_row_col((uint8_t)(cell / TILES_PHYS_COLS + 1u), (uint8_t)(cell % TILES_PHYS_COLS + 1u));
}

/* ---- notes and haptics ---- */

static void voice_on(uint8_t note, uint8_t velocity, uint32_t off_ms) {
    uint8_t free_slot = PHYS_VOICES, soonest = 0u;
    for (uint8_t v = 0; v < PHYS_VOICES; v++) {
        if (s_voice_note[v] == note) {
            tiles_midi_note_off(PHYS_CHANNEL, note, 0u); /* the same note again: retrigger */
            tiles_midi_note_on(PHYS_CHANNEL, note, velocity);
            s_voice_off_ms[v] = off_ms;
            return;
        }
        if (s_voice_note[v] == PHYS_NO_NOTE && free_slot == PHYS_VOICES) {
            free_slot = v;
        }
        if ((int32_t)(s_voice_off_ms[v] - s_voice_off_ms[soonest]) < 0) {
            soonest = v;
        }
    }
    if (free_slot == PHYS_VOICES) {
        free_slot = soonest; /* all busy: the one ending soonest ends now */
        tiles_midi_note_off(PHYS_CHANNEL, s_voice_note[free_slot], 0u);
    }
    tiles_midi_note_on(PHYS_CHANNEL, note, velocity);
    s_voice_note[free_slot] = note;
    s_voice_off_ms[free_slot] = off_ms;
}

static void voices_service(uint32_t now) {
    for (uint8_t v = 0; v < PHYS_VOICES; v++) {
        if (s_voice_note[v] != PHYS_NO_NOTE && (int32_t)(now - s_voice_off_ms[v]) >= 0) {
            tiles_midi_note_off(PHYS_CHANNEL, s_voice_note[v], 0u);
            s_voice_note[v] = PHYS_NO_NOTE;
        }
    }
}

void tiles_physics_end_all_notes(void) {
    for (uint8_t v = 0; v < PHYS_VOICES; v++) {
        if (s_voice_note[v] != PHYS_NO_NOTE) {
            tiles_midi_note_off(PHYS_CHANNEL, s_voice_note[v], 0u);
            s_voice_note[v] = PHYS_NO_NOTE;
        }
    }
}

/* Scale degree `degree` counted from the key note at the octave set
 * elsewhere (Sand's melody); 0 if past the top. */
static uint8_t degree_note(uint8_t degree) {
    int base = PHYS_DEGREE_BASE_NOTE + tiles_note_map_get_key_offset() + 12 * tiles_note_map_get_octave_shift();
    uint8_t found = 0u;
    for (int note = base < 0 ? 0 : base; note < 128; note++) {
        if (tiles_note_map_quantize_to_scale((uint8_t)note) == (uint8_t)note) {
            if (found == degree) {
                return (uint8_t)note;
            }
            found++;
        }
    }
    return 0u;
}

/* A cell's own note (degree < 0) or a scale degree. */
static void play_note(uint8_t cell, int8_t degree, uint8_t velocity, float beats, uint32_t now) {
    uint8_t note;
    if (degree >= 0) {
        note = degree_note((uint8_t)degree);
        if (note == 0u) {
            return;
        }
    } else {
        uint8_t pad = pad_of(cell);
        if (!tiles_note_map_pad_has_note(pad)) {
            return;
        }
        note = tiles_note_map_get_note(pad);
    }
    uint32_t len = (uint32_t)fmaxf(beats * s_ms_per_beat, 20.0f);
    voice_on(note, velocity, (now + len) | 1u);
}

static void feel(uint8_t pad, uint8_t strength, uint32_t now) {
    if (strength >= PHYS_KICK_MIN) {
        tiles_haptics_trigger_kick(pad, strength);
        s_haptic_stop_ms[pad - 1u] = (now + PHYS_KICK_STOP_MS) | 1u;
    } else {
        tiles_haptics_trigger_touch_pulse(pad);
    }
}

static void haptics_service(uint32_t now) {
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        if (s_haptic_stop_ms[i] != 0u && (int32_t)(now - s_haptic_stop_ms[i]) >= 0) {
            s_haptic_stop_ms[i] = 0u;
            tiles_haptics_stop((uint8_t)(i + 1u));
        }
    }
}

/* Every note first, then the haptics (a motor start is an I2C write). */
static void deliver(const tiles_phys_out_t *out, bool felt, uint32_t now) {
    for (uint8_t i = 0; i < out->note_count; i++) {
        play_note(out->note_cell[i], out->note_degree[i], out->note_velocity[i], out->note_beats[i], now);
    }
    if (!felt) {
        return;
    }
    for (uint8_t i = 0; i < out->tap_count && i < PHYS_MAX_HAPTICS_PER_STEP; i++) {
        feel(pad_of(out->tap_cell[i]), out->tap_strength[i], now);
    }
}

/* ---- time ---- */

static float sec_per_beat(void) {
    return s_ms_per_beat / 1000.0f;
}

static void advance_beats(tiles_midi_clock_state_t clock) {
    uint64_t now_us = time_us_64();
    double dt_ms = (double)(now_us - s_last_us) / 1000.0;
    s_last_us = now_us;
    if (dt_ms > PHYS_MAX_STEP_MS) {
        dt_ms = PHYS_MAX_STEP_MS;
    }
    float ms_per_beat = tiles_midi_clock_get_ms_per_beat();
    if (ms_per_beat > 1.0f) {
        s_ms_per_beat = ms_per_beat;
    }
    double step = dt_ms / (double)s_ms_per_beat;
    if (clock.pulse_count != s_last_pulse_count) {
        s_last_pulse_count = clock.pulse_count;
        s_last_pulse_us = now_us;
    }
    if (clock.running) {
        double pulse_ms = (double)s_ms_per_beat / 24.0;
        double into = (double)(now_us - s_last_pulse_us) / 1000.0 / pulse_ms;
        double target = ((double)(clock.song_pulse % 24u) + (into > 1.0 ? 1.0 : into)) / 24.0;
        double err = target - (s_beats - floor(s_beats));
        err -= floor(err + 0.5); /* the short way round */
        double k = dt_ms / PHYS_LOCK_MS;
        step += err * (k > 1.0 ? 1.0 : k);
        if (step < 0.0) {
            step = 0.0;
        }
    }
    s_beats += step;
}

/* ---- the modes ---- */

void tiles_physics_init(void) {
    for (uint8_t v = 0; v < PHYS_VOICES; v++) {
        s_voice_note[v] = PHYS_NO_NOTE;
    }
    s_last_us = time_us_64();
    s_beats = 0.0;
    tiles_ripples_clear(&s_ripples, 0x51A7u, s_beats);
    tiles_sand_clear(&s_sand, s_beats);
    memset(&s_orbits, 0, sizeof(s_orbits));
    s_orbits.running = true;
    tiles_orbits_clear(&s_orbits, s_beats);
    tiles_cradle_clear(&s_cradle);
}

static void resync_touches(void) {
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_touched[i] = tiles_touch_is_touched((uint8_t)(i + 1u));
        s_struck[i] = false;
        s_circle_touch[i] = false;
        s_consumed[i] = true; /* a finger already down does nothing */
    }
}

void tiles_physics_enter(tiles_phys_kind_t kind) {
    (void)kind;
    resync_touches();
}

/* A mode that isn't shown lets go of every finger it thought was down. */
static void let_go(int kind) {
    for (uint8_t cell = 0; cell < TILES_PHYS_CELLS; cell++) {
        if (kind != TILES_PHYS_RIPPLES && s_ripples.held[cell]) {
            tiles_ripples_release(&s_ripples, cell);
        }
        if (kind != TILES_PHYS_SAND && s_sand.held[cell]) {
            tiles_sand_release(&s_sand, cell);
        }
    }
    if (kind != TILES_PHYS_CRADLE) {
        for (uint8_t s = 0; s < 2u; s++) {
            if (s_cradle.lift_cell[s] != TILES_PHYS_NO_CELL) {
                tiles_cradle_release(&s_cradle, s_cradle.lift_cell[s], s_beats);
            }
        }
    }
}

void tiles_physics_advance(tiles_midi_clock_state_t clock, int shown) {
    uint32_t now = now_ms();
    advance_beats(clock);
    if (shown != s_shown) {
        let_go(shown);
        s_shown = shown;
    }
    voices_service(now);
    haptics_service(now);
    tiles_phys_out_t out;
    tiles_phys_out_clear(&out);
    tiles_ripples_step(&s_ripples, s_beats, &out);
    deliver(&out, shown == TILES_PHYS_RIPPLES, now);
    tiles_phys_out_clear(&out);
    tiles_sand_step(&s_sand, s_beats, sec_per_beat(), &out);
    deliver(&out, shown == TILES_PHYS_SAND, now);
    tiles_phys_out_clear(&out);
    tiles_orbits_step(&s_orbits, s_beats, &out);
    deliver(&out, shown == TILES_PHYS_ORBITS, now);
    tiles_phys_out_clear(&out);
    tiles_cradle_step(&s_cradle, s_beats, &out);
    deliver(&out, shown == TILES_PHYS_CRADLE, now);
}

static bool erase(tiles_phys_kind_t kind, uint8_t cell) {
    switch (kind) {
    case TILES_PHYS_RIPPLES:
        return tiles_ripples_erase(&s_ripples, cell);
    case TILES_PHYS_SAND:
        return tiles_sand_erase(&s_sand, cell);
    case TILES_PHYS_ORBITS:
        return tiles_orbits_erase(&s_orbits, cell);
    case TILES_PHYS_CRADLE:
        return tiles_cradle_erase(&s_cradle, cell);
    default:
        return false;
    }
}

static void clear_all(tiles_phys_kind_t kind) {
    switch (kind) {
    case TILES_PHYS_RIPPLES:
        tiles_ripples_clear(&s_ripples, s_ripples.rng, s_beats);
        break;
    case TILES_PHYS_SAND:
        tiles_sand_clear(&s_sand, s_beats);
        break;
    case TILES_PHYS_ORBITS:
        tiles_orbits_clear(&s_orbits, s_beats);
        break;
    case TILES_PHYS_CRADLE:
        tiles_cradle_clear(&s_cradle);
        break;
    default:
        break;
    }
}

static uint32_t erasable(tiles_phys_kind_t kind) {
    switch (kind) {
    case TILES_PHYS_RIPPLES:
        return tiles_ripples_erasable(&s_ripples);
    case TILES_PHYS_SAND:
        return tiles_sand_erasable(&s_sand);
    case TILES_PHYS_ORBITS:
        return tiles_orbits_erasable(&s_orbits);
    case TILES_PHYS_CRADLE:
        return tiles_cradle_erasable(&s_cradle);
    default:
        return 0u;
    }
}

static void on_strike(tiles_phys_kind_t kind, uint8_t cell, uint8_t i, uint32_t now) {
    tiles_phys_out_t out;
    tiles_phys_out_clear(&out);
    uint8_t pad = (uint8_t)(i + 1u);
    uint8_t velocity = s_velocity[i];
    s_struck_at[i] = s_beats;
    switch (kind) {
    case TILES_PHYS_RIPPLES:
        if (!tiles_ripples_strike(&s_ripples, cell, (float)velocity / 127.0f, s_beats, &out)) {
            s_consumed[i] = true; /* a stone lifted out */
            s_red_ms[cell] = now | 1u;
        }
        break;
    case TILES_PHYS_SAND:
        tiles_sand_strike(&s_sand, cell, s_beats);
        feel(pad, 40u, now);
        break;
    case TILES_PHYS_ORBITS:
        if (tiles_orbits_is_sun(&s_orbits, cell)) {
            s_consumed[i] = true; /* the sun: hold elsewhere to move it */
            feel(pad, 40u, now);
        } else if (tiles_orbits_remove_planet(&s_orbits, cell)) {
            s_consumed[i] = true; /* a planet's own cell: it goes */
            s_red_ms[cell] = now | 1u;
            feel(pad, 100u, now);
        } else {
            play_note(cell, TILES_PHYS_CELL_NOTE, velocity, 0.5f, now);
            feel(pad, velocity, now);
        }
        break;
    case TILES_PHYS_CRADLE:
        tiles_cradle_strike(&s_cradle, cell, velocity, s_beats, &out);
        break;
    default:
        break;
    }
    deliver(&out, true, now);
}

static void on_hold(tiles_phys_kind_t kind, uint8_t cell, uint8_t i, float pressure, uint32_t now) {
    switch (kind) {
    case TILES_PHYS_RIPPLES:
        tiles_ripples_set_pressure(&s_ripples, cell, pressure);
        break;
    case TILES_PHYS_SAND:
        tiles_sand_set_pressure(&s_sand, cell, pressure);
        break;
    case TILES_PHYS_ORBITS:
        if (!s_consumed[i] && now - s_touch_ms[i] >= PHYS_SUN_HOLD_MS) {
            s_consumed[i] = true; /* a hold: the sun moves here, never a planet */
            tiles_orbits_move_sun(&s_orbits, cell, s_beats);
            feel((uint8_t)(i + 1u), 110u, now);
        }
        break;
    default:
        break;
    }
}

static void on_release(tiles_phys_kind_t kind, uint8_t cell, uint8_t i) {
    switch (kind) {
    case TILES_PHYS_RIPPLES:
        tiles_ripples_release(&s_ripples, cell);
        break;
    case TILES_PHYS_SAND:
        tiles_sand_release(&s_sand, cell);
        break;
    case TILES_PHYS_ORBITS:
        if (!s_consumed[i]) {
            tiles_orbits_launch(&s_orbits, cell, s_velocity[i], s_struck_at[i]);
        }
        break;
    case TILES_PHYS_CRADLE:
        tiles_cradle_release(&s_cradle, cell, s_beats);
        break;
    default:
        break;
    }
}

static float pressure_of(float depth) {
    float full = (float)tiles_expression_get_aftertouch_sensitivity();
    float span = full - TILES_EXPRESSION_MIN_STRIKE_DEPTH_DELTA;
    if (span < 1.0f) {
        span = 1.0f;
    }
    float p = (depth - TILES_EXPRESSION_MIN_STRIKE_DEPTH_DELTA) / span;
    return p < 0.0f ? 0.0f : (p > 1.0f ? 1.0f : p);
}

void tiles_physics_handle_input(tiles_phys_kind_t kind, uint32_t now) {
    bool circle = tiles_button_is_pressed(TILES_CIRCLE_BUTTON_ID);
    for (uint8_t row = 1u; row <= TILES_GRID_MAX_ROW; row++) {
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            uint8_t pad = board_pad_for_row_col(row, col);
            uint8_t i = (uint8_t)(pad - 1u);
            uint8_t cell = (uint8_t)((row - 1u) * TILES_PHYS_COLS + (col - 1u));
            bool touched = tiles_touch_is_touched(pad);
            bool was = s_touched[i];
            s_touched[i] = touched;
            float depth = (float)tiles_hall_get_depth(pad);
            if (touched && !was) {
                s_touch_ms[i] = now;
                s_peak[i] = depth;
                s_struck[i] = false;
                s_consumed[i] = false;
                s_circle_touch[i] = circle;
            }
            if (touched && s_circle_touch[i]) {
                if (!s_consumed[i] && now - s_touch_ms[i] >= PHYS_CLEAR_HOLD_MS) {
                    s_consumed[i] = true;
                    clear_all(kind);
                    for (uint8_t c = 0; c < TILES_PHYS_CELLS; c++) {
                        s_red_ms[c] = now | 1u;
                    }
                    feel(pad, 110u, now);
                }
            } else if (touched) {
                if (depth > s_peak[i]) {
                    s_peak[i] = depth;
                }
                if (!s_struck[i] && s_peak[i] >= TILES_EXPRESSION_MIN_STRIKE_DEPTH_DELTA && !s_consumed[i]) {
                    s_struck[i] = true;
                    s_velocity[i] = tiles_expression_velocity_from_strike(now - s_touch_ms[i], s_peak[i]);
                    on_strike(kind, cell, i, now);
                }
                if (s_struck[i]) {
                    on_hold(kind, cell, i, pressure_of(depth), now);
                }
            } else if (was) {
                if (s_circle_touch[i] && !s_consumed[i]) {
                    if (erase(kind, cell)) {
                        s_red_ms[cell] = now | 1u;
                        feel(pad, 100u, now);
                    } else {
                        feel(pad, 30u, now); /* nothing there */
                    }
                } else if (s_struck[i]) {
                    on_release(kind, cell, i);
                }
                s_struck[i] = false;
                s_circle_touch[i] = false;
            }
        }
    }
}

static const float MODE_RGB[TILES_PHYS_KIND_COUNT][3] = {
    {TILES_RIPPLES_COLOR_R, TILES_RIPPLES_COLOR_G, TILES_RIPPLES_COLOR_B},
    {TILES_SAND_COLOR_R, TILES_SAND_COLOR_G, TILES_SAND_COLOR_B},
    {TILES_ORBITS_COLOR_R, TILES_ORBITS_COLOR_G, TILES_ORBITS_COLOR_B},
    {TILES_CRADLE_COLOR_R, TILES_CRADLE_COLOR_G, TILES_CRADLE_COLOR_B},
};

void tiles_physics_render(tiles_phys_kind_t kind, uint32_t now, float beat_flash, float diamond_led) {
    tiles_phys_frame_t frame;
    switch (kind) {
    case TILES_PHYS_RIPPLES:
        tiles_ripples_render(&s_ripples, s_beats, frame);
        break;
    case TILES_PHYS_SAND:
        tiles_sand_render(&s_sand, s_beats, frame);
        break;
    case TILES_PHYS_ORBITS:
        tiles_orbits_render(&s_orbits, s_beats, frame);
        break;
    default:
        tiles_cradle_render(&s_cradle, s_beats, frame);
        break;
    }
    if (tiles_button_is_pressed(TILES_CIRCLE_BUTTON_ID)) {
        /* The delete view: what a tap would remove pulses red. */
        uint32_t bits = erasable(kind);
        float pulse = PHYS_CIRCLE_RED_LO +
                      (PHYS_CIRCLE_RED_HI - PHYS_CIRCLE_RED_LO) * (0.5f + 0.5f * sinf((float)now * 0.012f));
        for (uint8_t cell = 0; cell < TILES_PHYS_CELLS; cell++) {
            float *f = frame[cell];
            bool red = ((bits >> cell) & 1u) != 0u;
            float k = red ? PHYS_CIRCLE_DIM * 0.5f : PHYS_CIRCLE_DIM;
            f[0] *= k;
            f[1] *= k;
            f[2] *= k;
            if (red) {
                f[0] = fmaxf(f[0], pulse);
                f[1] = fmaxf(f[1], pulse * 0.08f);
                f[2] = fmaxf(f[2], pulse * 0.12f);
            }
        }
    }
    float pads[TILES_NUM_PADS][3];
    for (uint8_t cell = 0; cell < TILES_PHYS_CELLS; cell++) {
        float *f = frame[cell];
        if (s_red_ms[cell] != 0u) {
            uint32_t age = now - s_red_ms[cell];
            if (age < PHYS_RED_FLASH_MS) {
                float k = 1.0f - (float)age / (float)PHYS_RED_FLASH_MS;
                f[0] = fmaxf(f[0], k);
                f[1] *= 1.0f - k;
                f[2] *= 1.0f - k;
            } else {
                s_red_ms[cell] = 0u;
            }
        }
        uint8_t pad = pad_of(cell);
        pads[pad - 1u][0] = f[0];
        pads[pad - 1u][1] = f[1];
        pads[pad - 1u][2] = f[2];
    }
    tiles_lighting_set_standby_frame(pads, PHYS_PAD_WRITES_PER_SCAN);

    float minus = 0.0f, plus = 0.0f;
    if (kind == TILES_PHYS_ORBITS) {
        /* Transport: "+" lit while they move, "-" while stopped. */
        plus = tiles_orbits_is_running(&s_orbits) ? 1.0f : 0.0f;
        minus = tiles_orbits_is_running(&s_orbits) ? 0.0f : 0.35f;
    } else {
        tiles_octave_control_led_levels(now, &minus, &plus);
    }
    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        float level = 0.0f;
        if (col == TILES_SQUARE_BUTTON_COL) {
            level = tiles_physics_square_on(kind) ? PHYS_SQUARE_LED_LEVEL : 0.0f;
        } else if (col == TILES_CIRCLE_BUTTON_COL) {
            level = beat_flash;
        } else if (col == TILES_DIAMOND_BUTTON_COL) {
            level = diamond_led;
        } else if (col == TILES_MINUS_BUTTON_COL) {
            level = minus;
        } else if (col == TILES_PLUS_BUTTON_COL) {
            level = plus;
        }
        tiles_buttons_set_standby_led(board_button_for_col(col), level);
    }
    /* What moves on the pads lights the underglow from above. */
    const float *rgb = MODE_RGB[kind < TILES_PHYS_KIND_COUNT ? kind : TILES_PHYS_RIPPLES];
    tiles_lighting_set_standby_underglow_from_pads((const float(*)[3])pads, rgb[0], rgb[1], rgb[2]);
}

void tiles_physics_toggle_square(tiles_phys_kind_t kind) {
    switch (kind) {
    case TILES_PHYS_RIPPLES:
        s_ripples.splash = !s_ripples.splash;
        break;
    case TILES_PHYS_SAND:
        tiles_sand_flip(&s_sand, s_sand.gdir > 0);
        break;
    case TILES_PHYS_ORBITS:
        tiles_orbits_set_pull(&s_orbits, !s_orbits.pull, s_beats);
        break;
    case TILES_PHYS_CRADLE:
        tiles_cradle_set_wave(&s_cradle, !s_cradle.wave, s_beats);
        break;
    default:
        break;
    }
}

bool tiles_physics_square_on(tiles_phys_kind_t kind) {
    switch (kind) {
    case TILES_PHYS_RIPPLES:
        return s_ripples.splash;
    case TILES_PHYS_SAND:
        return s_sand.gdir < 0;
    case TILES_PHYS_ORBITS:
        return s_orbits.pull;
    case TILES_PHYS_CRADLE:
        return s_cradle.wave;
    default:
        return false;
    }
}

bool tiles_physics_is_playing(tiles_phys_kind_t kind) {
    switch (kind) {
    case TILES_PHYS_RIPPLES:
        return tiles_ripples_active(&s_ripples);
    case TILES_PHYS_SAND:
        return tiles_sand_active(&s_sand);
    case TILES_PHYS_ORBITS:
        return tiles_orbits_active(&s_orbits);
    case TILES_PHYS_CRADLE:
        return tiles_cradle_active(&s_cradle);
    default:
        return false;
    }
}

void tiles_physics_orbits_play(void) {
    tiles_orbits_play(&s_orbits, s_beats);
}

void tiles_physics_orbits_stop(void) {
    tiles_orbits_stop(&s_orbits, s_beats);
}

bool tiles_physics_orbits_is_running(void) {
    return tiles_orbits_is_running(&s_orbits);
}
