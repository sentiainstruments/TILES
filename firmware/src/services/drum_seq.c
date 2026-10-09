#include "drum_seq.h"

#include "board_layout.h"
#include "board_pins.h"
#include "buttons.h"
#include "drum_pattern.h"
#include "expression.h"
#include "hall.h"
#include "haptics.h"
#include "kv_store.h"
#include "lighting.h"
#include "midi_channels.h"
#include "midi_out.h"
#include "touch.h"

#include "pico/time.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* A step held this long opens its chance dial (the sequencer's pitch-pick
 * hold, services/op_mode.c OP_SEQ_PITCH_ASSIGN_HOLD_MS). */
#define DRUM_STEP_HOLD_MS 350u
/* Circle + a drum held this long clears its steps (the pattern bank's
 * delete hold); held on to DRUM_CLEAR_ALL_HOLD_MS, the whole pattern. */
#define DRUM_CLEAR_HOLD_MS 3000u
#define DRUM_CLEAR_ALL_HOLD_MS 6000u
#define DRUM_CLEAR_FLASH_MS 400u
/* Saving: every change goes to flash on its own, DRUM_SAVE_QUIET_MS after
 * the last one, with no pad touched (a flash write stalls everything for
 * tens of ms). While the beat plays, only right after a step starts, with
 * no repeat due, and only if steps are at least DRUM_SAVE_MIN_STEP_MS long
 * (below 188 BPM), so the stall lands in a gap; faster, it waits for a
 * stop. A failed write retries after DRUM_SAVE_RETRY_MS. */
#define DRUM_SAVE_QUIET_MS 2000u
#define DRUM_SAVE_MIN_STEP_MS 80.0f
#define DRUM_SAVE_RETRY_MS 30000u
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
static uint8_t s_drum_clear_stage[TILES_DRUM_VOICES]; /* 0, 1 = part cleared, 2 = all cleared */
static uint32_t s_all_cleared_ms;

/* Saving (see DRUM_SAVE_QUIET_MS). */
static tiles_kv_t s_kv;
static bool s_storage;
static bool s_dirty;
static uint32_t s_changed_ms;
static uint32_t s_retry_after_ms;
static bool s_step_entered; /* the player started a step this scan */
static uint32_t s_saves;
static uint16_t s_saved_bytes;
static uint8_t s_blob[TILES_KV_MAX_PAYLOAD];

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

static void mark_changed(void) {
    s_dirty = true;
    s_changed_ms = now_ms();
}

void tiles_drum_seq_init(const tiles_kv_ops_t *ops) {
    tiles_drum_pattern_clear(&s_pattern);
    s_storage = ops != 0;
    s_dirty = false;
    s_retry_after_ms = 0u;
    s_saves = 0u;
    s_saved_bytes = 0u;
    tiles_kv_init(&s_kv, ops);
    if (s_storage) {
        uint16_t len = 0u, version = 0u;
        if (tiles_kv_read(&s_kv, s_blob, sizeof(s_blob), &len, &version) && version == TILES_DRUM_BLOB_VERSION) {
            tiles_drum_pattern_decode(&s_pattern, s_blob, len);
            s_saved_bytes = len;
        }
    }
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
    s_all_cleared_ms = 0u;
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
    uint32_t started = s_player.step_started_pulse;
    uint8_t step = s_player.step;
    tiles_drum_player_advance(&s_player, &s_pattern, &s_out, clock.pulse_count, clock.running, clock.start_edge);
    s_step_entered = s_player.step_started_pulse != started || s_player.step != step;
}

static bool any_pad_touched(void) {
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        if (tiles_touch_is_touched(pad)) {
            return true;
        }
    }
    return false;
}

void tiles_drum_seq_persist_service(uint32_t now, bool clock_running) {
    if (!s_dirty || !s_storage || now - s_changed_ms < DRUM_SAVE_QUIET_MS) {
        return;
    }
    if (s_retry_after_ms != 0u && (int32_t)(now - s_retry_after_ms) < 0) {
        return;
    }
    if (any_pad_touched()) {
        return;
    }
    if (s_player.running && clock_running) {
        float step_ms = tiles_midi_clock_get_ms_per_beat() / 4.0f;
        if (!s_step_entered || tiles_drum_player_repeats_pending(&s_player) || step_ms < DRUM_SAVE_MIN_STEP_MS) {
            return;
        }
    }
    bool truncated = false;
    uint16_t len = tiles_drum_pattern_encode(&s_pattern, s_blob, sizeof(s_blob), &truncated);
    if (tiles_kv_write(&s_kv, s_blob, len, TILES_DRUM_BLOB_VERSION) != TILES_KV_OK) {
        s_retry_after_ms = now + DRUM_SAVE_RETRY_MS;
        if (s_retry_after_ms == 0u) {
            s_retry_after_ms = 1u;
        }
        return;
    }
    if (truncated) {
        printf("[drums] pattern too big to save whole: kept %u bytes\n", (unsigned)len);
    }
    s_dirty = false;
    s_retry_after_ms = 0u;
    s_saves++;
    s_saved_bytes = len;
}

tiles_drum_seq_store_info_t tiles_drum_seq_get_store_info(void) {
    tiles_drum_seq_store_info_t info = {s_storage, s_dirty, s_saved_bytes, s_saves};
    return info;
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
        uint8_t percent = (uint8_t)(f * 100.0f + 0.5f);
        if (s_pattern.probability[note][s_edit_step] != percent) {
            tiles_drum_pattern_set_probability(&s_pattern, note, s_edit_step, percent);
            mark_changed();
        }
    } else {
        uint8_t hits = (uint8_t)(1u + (uint32_t)(f * (float)(TILES_DRUM_MAX_RATCHET - 1u) + 0.5f));
        if (s_pattern.ratchet[note][s_edit_step] != hits) {
            tiles_drum_pattern_set_ratchet(&s_pattern, note, s_edit_step, hits);
            mark_changed();
        }
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
            s_drum_clear_stage[voice] = 0u;
            s_drum_awaiting[voice] = false;
        } else {
            s_drum_clear_ms[voice] = 0u;
            s_drum_awaiting[voice] = true;
            s_drum_touch_ms[voice] = now;
            s_drum_peak[voice] = (float)tiles_hall_get_depth(pad);
        }
    } else if (touched) {
        if (s_drum_clear_ms[voice] != 0u) {
            uint32_t held = now - s_drum_clear_ms[voice];
            if (s_drum_clear_stage[voice] == 0u && held >= DRUM_CLEAR_HOLD_MS) {
                /* 3 s: this drum's part. */
                tiles_drum_pattern_clear_note(&s_pattern, tiles_drum_note(s_bank, voice));
                tiles_haptics_trigger_touch_pulse(pad);
                s_drum_cleared_ms[voice] = now;
                s_drum_clear_stage[voice] = 1u;
                mark_changed();
            } else if (s_drum_clear_stage[voice] == 1u && held >= DRUM_CLEAR_ALL_HOLD_MS) {
                /* Held on to 6 s: the whole pattern, every bank. */
                tiles_drum_pattern_clear(&s_pattern);
                tiles_haptics_trigger_kick(pad, 127u);
                s_all_cleared_ms = now;
                s_drum_clear_stage[voice] = 2u;
                s_drum_clear_ms[voice] = 0u;
                mark_changed();
            }
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
                mark_changed();
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
    if (s_all_cleared_ms != 0u && now - s_all_cleared_ms < DRUM_CLEAR_FLASH_MS) {
        for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
            tiles_lighting_set_standby_pad_rgb(pad, 1.0f, 0.0f, 0.0f); /* everything cleared */
        }
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
