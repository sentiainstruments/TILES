#include "drum_seq.h"

#include "board_layout.h"
#include "board_pins.h"
#include "buttons.h"
#include "drum_pattern.h"
#include "expression.h"
#include "hall.h"
#include "haptics.h"
#include "lighting.h"
#include "midi_channels.h"
#include "midi_out.h"
#include "touch.h"

#include "pico/time.h"

#include <math.h>
#include <stdlib.h>

/* A step held this long opens its chance dial (the sequencer's pitch-pick
 * hold, services/op_mode.c OP_SEQ_PITCH_ASSIGN_HOLD_MS). */
#define DRUM_STEP_HOLD_MS 350u
/* Circle + a drum held this long clears its steps (the pattern bank's
 * delete hold). */
#define DRUM_CLEAR_HOLD_MS 3000u
#define DRUM_CLEAR_FLASH_MS 400u
/* How long a drum pad flashes white when its drum plays. First guess. */
#define DRUM_FIRE_FLASH_MS 90u
/* Depth dials: ~900 is full-scale depth; below the guard (the tail of a
 * lift) the last value stays, as in the sequencer's dials. */
#define DRUM_DIAL_FULL_DEPTH 900.0f
#define DRUM_DIAL_RELEASE_GUARD_DEPTH 60u

/* Levels, all of the mode's lime unless noted. */
#define DRUM_STEP_ARMED_LEVEL 0.35f
#define DRUM_PLAYHEAD_ARMED_LEVEL 0.9f /* white */
#define DRUM_PLAYHEAD_LEVEL 0.15f      /* white, on an empty step */
#define DRUM_PAD_SELECTED_LEVEL 1.0f
#define DRUM_PAD_USED_LEVEL 0.35f
#define DRUM_PAD_EMPTY_LEVEL 0.08f
#define DRUM_FIRE_FLASH_LEVEL 0.8f /* white */
#define DRUM_TRANSPORT_LED_LEVEL 0.8f
#define DRUM_PI 3.14159265358979323846f

typedef enum { DRUM_EDIT_NONE = 0, DRUM_EDIT_CHANCE, DRUM_EDIT_REPEAT } drum_edit_t;

static tiles_drum_pattern_t s_pattern;
static tiles_drum_player_t s_player;
static int8_t s_bank;
static uint8_t s_voice; /* the selected drum, 0-7 */
static bool s_shown;

static bool s_prev_touched[TILES_NUM_PADS];
static uint32_t s_step_touch_ms[TILES_DRUM_STEPS]; /* 0 = not timing a tap/hold */

static drum_edit_t s_edit;
static uint8_t s_edit_step;

/* Drum pads played live (strike detection as in chord mode). */
static bool s_drum_awaiting[TILES_DRUM_VOICES];
static uint32_t s_drum_touch_ms[TILES_DRUM_VOICES];
static float s_drum_peak[TILES_DRUM_VOICES];
static bool s_drum_sounding[TILES_DRUM_VOICES];
static uint8_t s_drum_sounding_note[TILES_DRUM_VOICES];
static uint32_t s_drum_clear_ms[TILES_DRUM_VOICES]; /* circle + hold started, 0 = none */
static uint32_t s_drum_cleared_ms[TILES_DRUM_VOICES];
static uint32_t s_drum_fired_ms[TILES_DRUM_VOICES];
static bool s_drum_fired_any[TILES_DRUM_VOICES];

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

/* ---- the player's MIDI output ---- */

static void out_note_on(uint8_t note, uint8_t velocity, void *ctx) {
    (void)ctx;
    tiles_midi_note_on(TILES_MIDI_CH_DRUMS, note, velocity);
    for (uint8_t v = 0; v < TILES_DRUM_VOICES; v++) {
        if (tiles_drum_note(s_bank, v) == note) {
            s_drum_fired_ms[v] = now_ms();
            s_drum_fired_any[v] = true;
        }
    }
    /* The selected drum's hits pulse their step pad, so a finger resting on
     * the grid feels the beat. */
    if (s_shown && note == tiles_drum_note(s_bank, s_voice) && s_edit == DRUM_EDIT_NONE) {
        tiles_haptics_trigger_touch_pulse(tiles_drum_pad_for_step(s_player.step));
    }
}

static void out_note_off(uint8_t note, void *ctx) {
    (void)ctx;
    tiles_midi_note_off(TILES_MIDI_CH_DRUMS, note, 0u);
}

static uint32_t out_random(void *ctx) {
    (void)ctx;
    return (uint32_t)rand();
}

static const tiles_drum_output_t s_out = {out_note_on, out_note_off, out_random, 0};

/* ---- lifecycle ---- */

static void resync_touches(void) {
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        s_prev_touched[pad - 1u] = tiles_touch_is_touched(pad);
    }
    for (uint8_t s = 0; s < TILES_DRUM_STEPS; s++) {
        s_step_touch_ms[s] = 0u;
    }
    for (uint8_t v = 0; v < TILES_DRUM_VOICES; v++) {
        s_drum_awaiting[v] = false;
        s_drum_clear_ms[v] = 0u;
    }
}

static void end_live_notes(void) {
    for (uint8_t v = 0; v < TILES_DRUM_VOICES; v++) {
        if (s_drum_sounding[v]) {
            tiles_midi_note_off(TILES_MIDI_CH_DRUMS, s_drum_sounding_note[v], 0u);
            tiles_haptics_stop(tiles_drum_pad_for_voice(v));
            s_drum_sounding[v] = false;
        }
        s_drum_awaiting[v] = false;
    }
}

void tiles_drum_seq_init(void) {
    tiles_drum_pattern_clear(&s_pattern);
    tiles_drum_player_init(&s_player);
    s_bank = 0;
    s_voice = 0u;
    s_shown = false;
    s_edit = DRUM_EDIT_NONE;
    for (uint8_t v = 0; v < TILES_DRUM_VOICES; v++) {
        s_drum_sounding[v] = false;
        s_drum_cleared_ms[v] = 0u;
        s_drum_fired_any[v] = false;
    }
    resync_touches();
}

void tiles_drum_seq_enter(void) {
    s_edit = DRUM_EDIT_NONE;
    resync_touches();
}

void tiles_drum_seq_leave(void) {
    s_edit = DRUM_EDIT_NONE;
    end_live_notes();
}

void tiles_drum_seq_end_all_notes(void) {
    end_live_notes();
    tiles_drum_player_end_all(&s_player, &s_out);
}

/* ---- transport and banks ---- */

bool tiles_drum_seq_is_running(void) {
    return s_player.running;
}

void tiles_drum_seq_start(bool restart) {
    tiles_drum_player_start(&s_player, restart);
}

void tiles_drum_seq_pause(void) {
    tiles_drum_player_pause(&s_player);
}

void tiles_drum_seq_rewind(void) {
    tiles_drum_player_rewind(&s_player);
}

void tiles_drum_seq_bank_step(int direction) {
    int8_t bank = tiles_drum_clamp_bank((int)s_bank + (direction < 0 ? -1 : 1));
    if (bank == s_bank) {
        return;
    }
    end_live_notes(); /* a held drum would otherwise end on the wrong note */
    s_bank = bank;
    for (uint8_t v = 0; v < TILES_DRUM_VOICES; v++) {
        s_drum_fired_any[v] = false;
    }
}

void tiles_drum_seq_advance(tiles_midi_clock_state_t clock, bool shown) {
    if (shown && !s_shown) {
        /* Back from a menu: a pad touched or released meanwhile isn't a tap. */
        resync_touches();
    }
    s_shown = shown;
    tiles_drum_player_advance(&s_player, &s_pattern, &s_out, clock.pulse_count, clock.running, clock.start_edge);
}

/* ---- edits: chance and repeats ---- */

static float dial_fraction(uint16_t depth) {
    float f = (float)depth / DRUM_DIAL_FULL_DEPTH;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

bool tiles_drum_seq_edit_is_open(void) {
    return s_edit != DRUM_EDIT_NONE;
}

void tiles_drum_seq_edit_cancel(void) {
    s_edit = DRUM_EDIT_NONE;
    resync_touches();
}

static void edit_open(drum_edit_t kind, uint8_t step) {
    s_edit = kind;
    s_edit_step = step;
}

static void handle_edit(void) {
    uint8_t pad = tiles_drum_pad_for_step(s_edit_step);
    if (!tiles_touch_is_touched(pad)) {
        tiles_drum_seq_edit_cancel(); /* release keeps the last value */
        return;
    }
    uint16_t depth = tiles_hall_get_depth(pad);
    if (depth < DRUM_DIAL_RELEASE_GUARD_DEPTH) {
        return;
    }
    uint8_t note = tiles_drum_note(s_bank, s_voice);
    float f = dial_fraction(depth);
    if (s_edit == DRUM_EDIT_CHANCE) {
        tiles_drum_pattern_set_probability(&s_pattern, note, s_edit_step, (uint8_t)(f * 100.0f + 0.5f));
    } else {
        tiles_drum_pattern_set_ratchet(&s_pattern, note, s_edit_step,
                                       (uint8_t)(1u + (uint32_t)(f * (float)(TILES_DRUM_MAX_RATCHET - 1u) + 0.5f)));
    }
}

/* ---- input ---- */

static void handle_drum_pad(uint8_t voice, uint8_t pad, bool touched, bool was, bool circle, uint32_t now) {
    if (touched && !was) {
        s_voice = voice; /* a tap selects */
        tiles_haptics_trigger_touch_pulse(pad);
        if (circle) {
            /* Circle first: a clear-hold, never a hit. */
            s_drum_clear_ms[voice] = now == 0u ? 1u : now;
            s_drum_awaiting[voice] = false;
        } else {
            s_drum_clear_ms[voice] = 0u;
            s_drum_awaiting[voice] = true;
            s_drum_touch_ms[voice] = now;
            s_drum_peak[voice] = (float)tiles_hall_get_depth(pad);
        }
    } else if (touched) {
        if (s_drum_clear_ms[voice] != 0u && now - s_drum_clear_ms[voice] >= DRUM_CLEAR_HOLD_MS) {
            tiles_drum_pattern_clear_note(&s_pattern, tiles_drum_note(s_bank, voice));
            tiles_haptics_trigger_touch_pulse(pad);
            s_drum_cleared_ms[voice] = now;
            s_drum_clear_ms[voice] = 0u;
        }
        if (s_drum_awaiting[voice]) {
            /* A push: past the strike depth (as melodic and chord mode), not a
             * light touch. */
            float depth = (float)tiles_hall_get_depth(pad);
            if (depth > s_drum_peak[voice]) {
                s_drum_peak[voice] = depth;
            }
            if (s_drum_peak[voice] >= TILES_EXPRESSION_MIN_STRIKE_DEPTH_DELTA) {
                uint8_t velocity = tiles_expression_velocity_from_strike(now - s_drum_touch_ms[voice], s_drum_peak[voice]);
                uint8_t note = tiles_drum_note(s_bank, voice);
                tiles_midi_note_on(TILES_MIDI_CH_DRUMS, note, velocity);
                tiles_haptics_trigger_kick(pad, velocity);
                s_drum_sounding[voice] = true;
                s_drum_sounding_note[voice] = note;
                s_drum_fired_ms[voice] = now;
                s_drum_fired_any[voice] = true;
                s_drum_awaiting[voice] = false;
            }
        }
    } else if (was) {
        if (s_drum_sounding[voice]) {
            tiles_midi_note_off(TILES_MIDI_CH_DRUMS, s_drum_sounding_note[voice], 0u);
            tiles_haptics_stop(pad);
            s_drum_sounding[voice] = false;
        }
        s_drum_awaiting[voice] = false;
        s_drum_clear_ms[voice] = 0u;
    }
}

void tiles_drum_seq_handle_input(uint32_t now) {
    if (s_edit != DRUM_EDIT_NONE) {
        handle_edit();
        return;
    }
    bool circle = tiles_button_is_pressed(TILES_CIRCLE_BUTTON_ID);
    uint8_t note = tiles_drum_note(s_bank, s_voice);
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        bool touched = tiles_touch_is_touched(pad);
        bool was = s_prev_touched[pad - 1u];
        s_prev_touched[pad - 1u] = touched;
        uint8_t step, voice;
        if (tiles_drum_voice_for_pad(pad, &voice)) {
            handle_drum_pad(voice, pad, touched, was, circle, now);
            continue;
        }
        if (!tiles_drum_step_for_pad(pad, &step)) {
            continue;
        }
        if (touched && !was) {
            if (circle) {
                edit_open(DRUM_EDIT_REPEAT, step); /* circle + step: repeats */
                return;
            }
            s_step_touch_ms[step] = now == 0u ? 1u : now;
        } else if (touched) {
            if (s_step_touch_ms[step] != 0u && now - s_step_touch_ms[step] >= DRUM_STEP_HOLD_MS) {
                s_step_touch_ms[step] = 0u;
                edit_open(DRUM_EDIT_CHANCE, step); /* hold: chance */
                return;
            }
        } else if (was) {
            /* A tap toggles on release, so a hold never flips the step first. */
            if (s_step_touch_ms[step] != 0u) {
                tiles_drum_pattern_toggle(&s_pattern, note, step);
            }
            s_step_touch_ms[step] = 0u;
        }
    }
}

/* ---- rendering ---- */

static void set_lime(uint8_t pad, float level) {
    tiles_lighting_set_standby_pad_rgb(pad, TILES_DRUM_SEQ_COLOR_R * level, TILES_DRUM_SEQ_COLOR_G * level,
                                       TILES_DRUM_SEQ_COLOR_B * level);
}

static float slow_pulse(uint32_t now, float period_ms, float lo, float hi) {
    float raw = 0.5f + 0.5f * sinf(2.0f * DRUM_PI * (float)now / period_ms);
    return lo + (hi - lo) * raw;
}

static void render_buttons(uint32_t now, float beat_flash, float diamond_led, bool clock_running) {
    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        float level = 0.0f;
        if (col == TILES_CIRCLE_BUTTON_COL) {
            level = beat_flash;
        } else if (col == TILES_DIAMOND_BUTTON_COL) {
            level = diamond_led;
        } else if (col == TILES_MINUS_BUTTON_COL && !s_player.running) {
            /* "-": solid when rewound, slow pulse when paused mid-bar (a second
             * "-" rewinds), as in the sequencer. */
            level = s_player.step == 0u ? DRUM_TRANSPORT_LED_LEVEL : slow_pulse(now, 3000.0f, 0.03f, 0.35f);
        } else if (col == TILES_PLUS_BUTTON_COL && clock_running) {
            /* "+": solid while playing; pulsing on the beat while the clock runs
             * without it. */
            level = s_player.running ? DRUM_TRANSPORT_LED_LEVEL
                                     : slow_pulse(now, tiles_midi_clock_get_ms_per_beat(), 0.0f, 1.0f);
        }
        tiles_buttons_set_standby_led(board_button_for_col(col), level);
    }
}

static void render_underglow(void) {
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        tiles_lighting_set_standby_underglow_rgb(i, TILES_DRUM_SEQ_COLOR_R, TILES_DRUM_SEQ_COLOR_G,
                                                 TILES_DRUM_SEQ_COLOR_B);
    }
}

/* Chance amber, repeats blue: a meter across all 24 pads, as in the
 * sequencer's dials. */
static void render_edit(void) {
    uint8_t note = tiles_drum_note(s_bank, s_voice);
    uint8_t lit;
    float r, g, b;
    if (s_edit == DRUM_EDIT_CHANCE) {
        lit = (uint8_t)((uint32_t)s_pattern.probability[note][s_edit_step] * TILES_NUM_PADS / 100u);
        r = 1.0f; g = 0.8f; b = 0.0f;
    } else {
        lit = (uint8_t)((uint32_t)s_pattern.ratchet[note][s_edit_step] * TILES_NUM_PADS / TILES_DRUM_MAX_RATCHET);
        r = 0.0f; g = 0.4f; b = 1.0f;
    }
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        float on = pad <= lit ? 1.0f : 0.0f;
        tiles_lighting_set_standby_pad_rgb(pad, r * on, g * on, b * on);
    }
}

void tiles_drum_seq_render(uint32_t now, float beat_flash, float diamond_led, bool clock_running) {
    render_buttons(now, beat_flash, diamond_led, clock_running);
    render_underglow();
    if (s_edit != DRUM_EDIT_NONE) {
        render_edit();
        return;
    }
    uint8_t note = tiles_drum_note(s_bank, s_voice);
    for (uint8_t step = 0; step < TILES_DRUM_STEPS; step++) {
        uint8_t pad = tiles_drum_pad_for_step(step);
        bool armed = tiles_drum_pattern_is_armed(&s_pattern, note, step);
        /* The playhead shows playing or parked, as in the sequencer. */
        if (step == s_player.step) {
            float w = armed ? DRUM_PLAYHEAD_ARMED_LEVEL : DRUM_PLAYHEAD_LEVEL;
            tiles_lighting_set_standby_pad_rgb(pad, w, w, w);
        } else if (armed) {
            /* Less likely = fainter; repeats tint it toward blue. */
            float level = DRUM_STEP_ARMED_LEVEL * (0.3f + 0.7f * (float)s_pattern.probability[note][step] / 100.0f);
            float blue = DRUM_STEP_ARMED_LEVEL * (float)(s_pattern.ratchet[note][step] - 1u) /
                         (float)(TILES_DRUM_MAX_RATCHET - 1u);
            tiles_lighting_set_standby_pad_rgb(pad, TILES_DRUM_SEQ_COLOR_R * level, TILES_DRUM_SEQ_COLOR_G * level,
                                               blue);
        } else {
            tiles_lighting_set_standby_pad_rgb(pad, 0.0f, 0.0f, 0.0f);
        }
    }
    for (uint8_t v = 0; v < TILES_DRUM_VOICES; v++) {
        uint8_t pad = tiles_drum_pad_for_voice(v);
        if (now - s_drum_cleared_ms[v] < DRUM_CLEAR_FLASH_MS && s_drum_cleared_ms[v] != 0u) {
            tiles_lighting_set_standby_pad_rgb(pad, 1.0f, 0.0f, 0.0f); /* cleared */
        } else if (s_drum_sounding[v]) {
            tiles_lighting_set_standby_pad_rgb(pad, 1.0f, 1.0f, 1.0f); /* held after a push */
        } else if (s_drum_fired_any[v] && now - s_drum_fired_ms[v] < DRUM_FIRE_FLASH_MS) {
            tiles_lighting_set_standby_pad_rgb(pad, DRUM_FIRE_FLASH_LEVEL, DRUM_FIRE_FLASH_LEVEL, DRUM_FIRE_FLASH_LEVEL);
        } else if (v == s_voice) {
            set_lime(pad, DRUM_PAD_SELECTED_LEVEL);
        } else {
            bool used = tiles_drum_pattern_note_has_steps(&s_pattern, tiles_drum_note(s_bank, v));
            set_lime(pad, used ? DRUM_PAD_USED_LEVEL : DRUM_PAD_EMPTY_LEVEL);
        }
    }
}
