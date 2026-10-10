#include "drum_seq.h"

#include "board_layout.h"
#include "board_pins.h"
#include "buttons.h"
#include "drum_pattern.h"
#include "expression.h"
#include "hall.h"
#include "haptics.h"
#include "kv_store.h"
#include "log_store.h"
#include "lighting.h"
#include "midi_channels.h"
#include "midi_out.h"
#include "touch.h"

#include "pico/time.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* A step held this long opens its roll dial (the sequencer's pitch-pick
 * hold, services/op_mode.c OP_SEQ_PITCH_ASSIGN_HOLD_MS). */
#define DRUM_STEP_HOLD_MS 350u
/* Circle + a drum held without pushing: this long clears its part; on to
 * DRUM_CLEAR_ALL_HOLD_MS, the whole pattern. (A push makes it a roll.) */
#define DRUM_CLEAR_HOLD_MS 1000u
#define DRUM_CLEAR_ALL_HOLD_MS 3000u
#define DRUM_CLEAR_FLASH_MS 400u
/* How long a drum pad flashes white when its drum plays. First guess. */
#define DRUM_FIRE_FLASH_MS 90u
/* Depth dials: ~900 is full-scale depth; below the guard (the tail of a
 * lift) the last value stays, as in the sequencer's dials. */
#define DRUM_DIAL_FULL_DEPTH 900.0f
#define DRUM_DIAL_RELEASE_GUARD_DEPTH 60u
/* Roll (circle + push a drum): the rate follows pressure, in MIDI clock
 * pulses per hit: 1/8, 1/16, 1/32, 1/32 triplet as the depth passes these
 * raw Hall depths (900 ~ full scale; first guesses). Velocity follows
 * pressure too, from DRUM_ROLL_VELOCITY_MIN up. */
#define DRUM_ROLL_DEPTH_16TH 300u
#define DRUM_ROLL_DEPTH_32ND 550u
#define DRUM_ROLL_DEPTH_32ND_T 800u
#define DRUM_ROLL_VELOCITY_MIN 30.0f
/* Saving: DRUM_SAVE_QUIET_MS after the last change, no pad touched. While
 * the clock runs only into already-erased flash (log_store.h: page
 * programs, well under a millisecond); an erase waits for the clock to
 * stop, so the beat never stalls. A failed write retries after
 * DRUM_SAVE_RETRY_MS. */
#define DRUM_SAVE_QUIET_MS 2000u
#define DRUM_SAVE_RETRY_MS 30000u

/* Levels; steps in the mode's lime, drum pads in their bank's green. */
#define DRUM_STEP_ARMED_LEVEL 0.35f
#define DRUM_PLAYHEAD_LEVEL 0.15f /* white, on an empty step */
#define DRUM_PAD_USED_LEVEL 0.4f
#define DRUM_PAD_EMPTY_LEVEL 0.12f
#define DRUM_FIRE_FLASH_LEVEL 0.8f /* white */
#define DRUM_TRANSPORT_LED_LEVEL 0.8f
#define DRUM_PAGE2_LED_LEVEL 0.8f
#define DRUM_OTHER_PAGE_LED_LEVEL 0.2f
#define DRUM_PI 3.14159265358979323846f
/* Blue: the selected drum, and the playhead on a step that sounds (the
 * sequencer's armed-playhead blue). */
#define DRUM_BLUE_R 0.0f
#define DRUM_BLUE_G 0.3f
#define DRUM_BLUE_B 1.0f


static tiles_drum_pattern_t s_pattern;
static tiles_drum_player_t s_player;
static int8_t s_bank;
static uint8_t s_voice; /* the selected drum, 0-7 */
static uint8_t s_page;  /* steps shown: 0 = 1-16, 1 = 17-32 */
static bool s_shown;
static uint32_t s_pulse; /* the clock as of this scan's advance */
static bool s_clock_running;

static bool s_prev_touched[TILES_NUM_PADS];
static uint32_t s_step_touch_ms[TILES_DRUM_PAGE_STEPS]; /* 0 = not timing a tap/hold */

static bool s_edit;          /* a step's roll dial is open */
static uint8_t s_edit_step; /* 0-31 */

/* Drum pads played live: strike detection as in chord mode, rolls, and the
 * circle-hold clear. */
static bool s_drum_awaiting[TILES_DRUM_VOICES];
static uint32_t s_drum_touch_ms[TILES_DRUM_VOICES];
static float s_drum_peak[TILES_DRUM_VOICES];
static bool s_drum_circle_touch[TILES_DRUM_VOICES]; /* circle was held at touch-down */
static bool s_drum_sounding[TILES_DRUM_VOICES];
static uint8_t s_drum_sounding_note[TILES_DRUM_VOICES];
static bool s_roll_active[TILES_DRUM_VOICES];
static uint8_t s_roll_pulses[TILES_DRUM_VOICES];
static uint32_t s_roll_last_grid[TILES_DRUM_VOICES];
static uint32_t s_roll_next_ms[TILES_DRUM_VOICES];
static uint32_t s_drum_clear_ms[TILES_DRUM_VOICES]; /* clear-hold started, 0 = none */
static uint8_t s_drum_clear_stage[TILES_DRUM_VOICES]; /* 0, 1 = part cleared, 2 = all cleared */
static uint32_t s_drum_cleared_ms[TILES_DRUM_VOICES];
static uint32_t s_drum_fired_ms[TILES_DRUM_VOICES];
static bool s_drum_fired_any[TILES_DRUM_VOICES];
static uint32_t s_all_cleared_ms;

/* Saving (see DRUM_SAVE_QUIET_MS). */
static tiles_log_t s_log;
static bool s_storage;
static bool s_dirty;
static uint32_t s_changed_ms;
static uint32_t s_retry_after_ms;
static bool s_step_entered; /* the player started a step this scan */
static uint32_t s_saves;
static uint16_t s_saved_bytes;
static uint8_t s_blob[TILES_LOG_MAX_PAYLOAD];

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
    if (s_shown && note == tiles_drum_note(s_bank, s_voice) && !s_edit &&
        s_player.step / TILES_DRUM_PAGE_STEPS == s_page) {
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
    for (uint8_t s = 0; s < TILES_DRUM_PAGE_STEPS; s++) {
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
        s_roll_active[v] = false;
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
    tiles_log_init(&s_log, ops);
    if (s_storage) {
        uint16_t len = 0u, version = 0u;
        if (tiles_log_read(&s_log, s_blob, sizeof(s_blob), &len, &version)) {
            (void)tiles_drum_pattern_decode(&s_pattern, s_blob, len, version);
            s_saved_bytes = len;
        } else {
            /* Firmware 0.2.8 kept the pattern in kv_store's format in these same
             * sectors: carry it over, re-saved in the log soon after boot. */
            tiles_kv_t kv;
            tiles_kv_init(&kv, ops);
            if (tiles_kv_read(&kv, s_blob, TILES_KV_MAX_PAYLOAD, &len, &version) &&
                tiles_drum_pattern_decode(&s_pattern, s_blob, len, version)) {
                s_dirty = true;
                s_changed_ms = 0u;
            }
        }
    }
    tiles_drum_player_init(&s_player);
    s_bank = 0;
    s_voice = 0u;
    s_page = 0u;
    s_shown = false;
    s_edit = false;
    for (uint8_t v = 0; v < TILES_DRUM_VOICES; v++) {
        s_drum_sounding[v] = false;
        s_roll_active[v] = false;
        s_drum_cleared_ms[v] = 0u;
        s_drum_fired_any[v] = false;
    }
    s_all_cleared_ms = 0u;
    resync_touches();
}

void tiles_drum_seq_enter(void) {
    s_edit = false;
    resync_touches();
}

void tiles_drum_seq_leave(void) {
    s_edit = false;
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
    s_pulse = clock.pulse_count;
    s_clock_running = clock.running;
    uint32_t started = s_player.step_started_pulse;
    uint8_t step = s_player.step;
    tiles_drum_player_advance(&s_player, &s_pattern, &s_out, clock.pulse_count, clock.running, clock.start_edge);
    s_step_entered = s_player.step_started_pulse != started || s_player.step != step;
}

/* ---- saving ---- */

static bool any_pad_touched(void) {
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        if (tiles_touch_is_touched(pad)) {
            return true;
        }
    }
    return false;
}

void tiles_drum_seq_persist_service(uint32_t now, bool clock_running) {
    if (!s_storage) {
        return;
    }
    if (!s_dirty) {
        /* Idle and stopped: erase the spare sector now, so saves while playing
         * never need to. */
        if (!clock_running && s_log.have_newest && !s_log.blank[1u - s_log.newest_slot] && !any_pad_touched()) {
            (void)tiles_log_prepare(&s_log);
        }
        return;
    }
    if (now - s_changed_ms < DRUM_SAVE_QUIET_MS ||
        (s_retry_after_ms != 0u && (int32_t)(now - s_retry_after_ms) < 0) || any_pad_touched()) {
        return;
    }
    if (clock_running && s_player.running && (!s_step_entered || tiles_drum_player_repeats_pending(&s_player))) {
        return; /* playing: right after a step starts, nothing else due */
    }
    bool truncated = false;
    uint16_t len = tiles_drum_pattern_encode(&s_pattern, s_blob, sizeof(s_blob), &truncated);
    tiles_log_result_t r = tiles_log_append(&s_log, s_blob, len, TILES_DRUM_BLOB_VERSION, !clock_running);
    if (r == TILES_LOG_ERR_NEEDS_ERASE) {
        return; /* saved once the clock stops */
    }
    if (r != TILES_LOG_OK) {
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

/* ---- the roll dial: hold a step ---- */

static float dial_fraction(uint16_t depth) {
    float f = (float)depth / DRUM_DIAL_FULL_DEPTH;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

bool tiles_drum_seq_edit_is_open(void) {
    return s_edit;
}

void tiles_drum_seq_edit_cancel(void) {
    s_edit = false;
    resync_touches();
}

/* Opens the roll dial on `step`, arming it first: a roll on a silent step
 * would do nothing. */
static void edit_open(uint8_t step) {
    uint8_t note = tiles_drum_note(s_bank, s_voice);
    if (!tiles_drum_pattern_is_armed(&s_pattern, note, step)) {
        tiles_drum_pattern_toggle(&s_pattern, note, step);
        mark_changed();
    }
    s_edit = true;
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
    /* Push harder for more hits within the step (1-4). */
    uint8_t note = tiles_drum_note(s_bank, s_voice);
    uint8_t hits = (uint8_t)(1u + (uint32_t)(dial_fraction(depth) * (float)(TILES_DRUM_MAX_RATCHET - 1u) + 0.5f));
    if (s_pattern.ratchet[note][s_edit_step] != hits) {
        tiles_drum_pattern_set_ratchet(&s_pattern, note, s_edit_step, hits);
        mark_changed();
    }
}

/* ---- drum pads: hits, rolls, clears ---- */

static void drum_hit(uint8_t voice, uint8_t pad, uint8_t velocity, bool first) {
    if (s_drum_sounding[voice]) {
        tiles_midi_note_off(TILES_MIDI_CH_DRUMS, s_drum_sounding_note[voice], 0u);
    }
    uint8_t note = tiles_drum_note(s_bank, voice);
    tiles_midi_note_on(TILES_MIDI_CH_DRUMS, note, velocity);
    if (first) {
        tiles_haptics_trigger_kick(pad, velocity);
    } else {
        tiles_haptics_trigger_touch_pulse(pad); /* each roll hit, felt */
    }
    s_drum_sounding[voice] = true;
    s_drum_sounding_note[voice] = note;
    s_drum_fired_ms[voice] = now_ms();
    s_drum_fired_any[voice] = true;
}

static uint8_t roll_pulses_for(uint16_t depth) {
    return depth >= DRUM_ROLL_DEPTH_32ND_T ? 2u : depth >= DRUM_ROLL_DEPTH_32ND ? 3u : depth >= DRUM_ROLL_DEPTH_16TH ? 6u : 12u;
}

static uint32_t roll_interval_ms(uint8_t pulses) {
    return (uint32_t)(tiles_midi_clock_get_ms_per_beat() * (float)pulses / (float)TILES_DRUM_CLOCKS_PER_BEAT);
}

static void roll_set_rate(uint8_t voice, uint8_t pulses, uint32_t now) {
    s_roll_pulses[voice] = pulses;
    s_roll_last_grid[voice] = s_pulse / pulses;
    s_roll_next_ms[voice] = now + roll_interval_ms(pulses);
}

/* Repeats on the clock's grid while it runs (in time with the beat), else
 * at the tempo's interval from the first hit. */
static void roll_service(uint8_t voice, uint8_t pad, uint16_t depth, uint32_t now) {
    uint8_t pulses = roll_pulses_for(depth);
    if (pulses != s_roll_pulses[voice]) {
        roll_set_rate(voice, pulses, now);
    }
    float v = DRUM_ROLL_VELOCITY_MIN + (127.0f - DRUM_ROLL_VELOCITY_MIN) * dial_fraction(depth);
    uint8_t velocity = (uint8_t)(v > 127.0f ? 127.0f : v);
    if (s_clock_running) {
        uint32_t grid = s_pulse / pulses;
        if (grid != s_roll_last_grid[voice]) {
            s_roll_last_grid[voice] = grid;
            drum_hit(voice, pad, velocity, false);
        }
    } else if ((int32_t)(now - s_roll_next_ms[voice]) >= 0) {
        drum_hit(voice, pad, velocity, false);
        uint32_t interval = roll_interval_ms(pulses);
        s_roll_next_ms[voice] += interval;
        if ((int32_t)(now - s_roll_next_ms[voice]) >= 0) {
            s_roll_next_ms[voice] = now + interval; /* fell behind: don't burst */
        }
    }
}

static void handle_drum_pad(uint8_t voice, uint8_t pad, bool touched, bool was, bool circle, uint32_t now) {
    if (touched && !was) {
        s_voice = voice; /* a tap selects */
        tiles_haptics_trigger_touch_pulse(pad);
        s_drum_circle_touch[voice] = circle;
        s_drum_awaiting[voice] = true;
        s_drum_touch_ms[voice] = now;
        s_drum_peak[voice] = (float)tiles_hall_get_depth(pad);
        s_drum_clear_ms[voice] = circle ? (now == 0u ? 1u : now) : 0u;
        s_drum_clear_stage[voice] = 0u;
        s_roll_active[voice] = false;
    } else if (touched) {
        uint16_t depth = tiles_hall_get_depth(pad);
        if (s_drum_awaiting[voice]) {
            /* A push: past the strike depth (as melodic and chord mode), not a
             * light touch. */
            if ((float)depth > s_drum_peak[voice]) {
                s_drum_peak[voice] = (float)depth;
            }
            if (s_drum_peak[voice] >= TILES_EXPRESSION_MIN_STRIKE_DEPTH_DELTA) {
                uint8_t velocity = tiles_expression_velocity_from_strike(now - s_drum_touch_ms[voice], s_drum_peak[voice]);
                drum_hit(voice, pad, velocity, true);
                s_drum_awaiting[voice] = false;
                if (s_drum_circle_touch[voice]) {
                    /* Circle + push: a roll (and no clear). */
                    s_roll_active[voice] = true;
                    s_drum_clear_ms[voice] = 0u;
                    roll_set_rate(voice, roll_pulses_for(depth), now);
                }
            }
        }
        if (s_roll_active[voice]) {
            roll_service(voice, pad, depth, now);
        } else if (s_drum_clear_ms[voice] != 0u) {
            uint32_t held = now - s_drum_clear_ms[voice];
            if (s_drum_clear_stage[voice] == 0u && held >= DRUM_CLEAR_HOLD_MS) {
                /* This drum's part. */
                tiles_drum_pattern_clear_note(&s_pattern, tiles_drum_note(s_bank, voice));
                tiles_haptics_trigger_touch_pulse(pad);
                s_drum_cleared_ms[voice] = now;
                s_drum_clear_stage[voice] = 1u;
                mark_changed();
            } else if (s_drum_clear_stage[voice] == 1u && held >= DRUM_CLEAR_ALL_HOLD_MS) {
                /* Held on: the whole pattern, every bank and page. */
                tiles_drum_pattern_clear(&s_pattern);
                tiles_haptics_trigger_kick(pad, 127u);
                s_all_cleared_ms = now;
                s_drum_clear_stage[voice] = 2u;
                s_drum_clear_ms[voice] = 0u;
                mark_changed();
            }
        }
    } else if (was) {
        if (s_drum_sounding[voice]) {
            tiles_midi_note_off(TILES_MIDI_CH_DRUMS, s_drum_sounding_note[voice], 0u);
            tiles_haptics_stop(pad);
            s_drum_sounding[voice] = false;
        }
        s_drum_awaiting[voice] = false;
        s_roll_active[voice] = false;
        s_drum_clear_ms[voice] = 0u;
    }
}

/* ---- input ---- */

void tiles_drum_seq_flip_page(void) {
    s_page ^= 1u;
    for (uint8_t s = 0; s < TILES_DRUM_PAGE_STEPS; s++) {
        s_step_touch_ms[s] = 0u; /* a finger resting on a step isn't a tap on the new page */
    }
}

void tiles_drum_seq_handle_input(uint32_t now) {
    if (s_edit) {
        handle_edit();
        return;
    }
    bool circle = tiles_button_is_pressed(TILES_CIRCLE_BUTTON_ID);
    uint8_t note = tiles_drum_note(s_bank, s_voice);
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        bool touched = tiles_touch_is_touched(pad);
        bool was = s_prev_touched[pad - 1u];
        s_prev_touched[pad - 1u] = touched;
        uint8_t local, voice;
        if (tiles_drum_voice_for_pad(pad, &voice)) {
            handle_drum_pad(voice, pad, touched, was, circle, now);
            continue;
        }
        if (!tiles_drum_step_for_pad(pad, &local)) {
            continue;
        }
        uint8_t step = (uint8_t)(s_page * TILES_DRUM_PAGE_STEPS + local);
        if (touched && !was) {
            s_step_touch_ms[local] = now == 0u ? 1u : now;
        } else if (touched) {
            if (s_step_touch_ms[local] != 0u && now - s_step_touch_ms[local] >= DRUM_STEP_HOLD_MS) {
                s_step_touch_ms[local] = 0u;
                edit_open(step); /* hold: roll */
                return;
            }
        } else if (was) {
            /* A tap toggles on release, so a hold never flips the step first. */
            if (s_step_touch_ms[local] != 0u) {
                tiles_drum_pattern_toggle(&s_pattern, note, step);
                mark_changed();
            }
            s_step_touch_ms[local] = 0u;
        }
    }
}

/* ---- rendering ---- */

/* Each bank of 8 drums has its own green on the drum pads, so a new set
 * reads as new: lime (the first bank, the mode's color), a darker green,
 * a cooler green, olive, then around again. */
static void bank_green(float level, float *r, float *g, float *b) {
    static const float GREENS[4][3] = {
        {TILES_DRUM_SEQ_COLOR_R, TILES_DRUM_SEQ_COLOR_G, TILES_DRUM_SEQ_COLOR_B},
        {0.0f, 0.5f, 0.05f},
        {0.0f, 1.0f, 0.45f},
        {0.25f, 0.45f, 0.0f},
    };
    uint8_t i = (uint8_t)((((int)s_bank % 4) + 4) % 4);
    *r = GREENS[i][0] * level;
    *g = GREENS[i][1] * level;
    *b = GREENS[i][2] * level;
}

static float slow_pulse(uint32_t now, float period_ms, float lo, float hi) {
    float raw = 0.5f + 0.5f * sinf(2.0f * DRUM_PI * (float)now / period_ms);
    return lo + (hi - lo) * raw;
}

static void render_buttons(uint32_t now, float beat_flash, float diamond_led, bool clock_running) {
    bool playhead_elsewhere = tiles_drum_pattern_length(&s_pattern) > TILES_DRUM_PAGE_STEPS && s_player.running &&
                              s_player.step / TILES_DRUM_PAGE_STEPS != s_page;
    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        float level = 0.0f;
        if (col == TILES_CIRCLE_BUTTON_COL) {
            level = beat_flash;
        } else if (col == TILES_DIAMOND_BUTTON_COL) {
            level = diamond_led;
        } else if (col == TILES_TRIANGLE_BUTTON_COL) {
            /* Triangle (with circle, the page): lit on page 2; faint while the
             * playhead is on the page not shown. */
            level = s_page == 1u ? DRUM_PAGE2_LED_LEVEL : (playhead_elsewhere ? DRUM_OTHER_PAGE_LED_LEVEL : 0.0f);
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

/* The roll dial: a blue meter across all 24 pads, as in the sequencer's
 * ratchet dial. */
static void render_edit(void) {
    uint8_t note = tiles_drum_note(s_bank, s_voice);
    uint8_t lit = (uint8_t)((uint32_t)s_pattern.ratchet[note][s_edit_step] * TILES_NUM_PADS / TILES_DRUM_MAX_RATCHET);
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        float on = pad <= lit ? 1.0f : 0.0f;
        tiles_lighting_set_standby_pad_rgb(pad, 0.0f, 0.4f * on, 1.0f * on);
    }
}

void tiles_drum_seq_render(uint32_t now, float beat_flash, float diamond_led, bool clock_running) {
    render_buttons(now, beat_flash, diamond_led, clock_running);
    render_underglow();
    if (s_edit) {
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
    for (uint8_t local = 0; local < TILES_DRUM_PAGE_STEPS; local++) {
        uint8_t step = (uint8_t)(s_page * TILES_DRUM_PAGE_STEPS + local);
        uint8_t pad = tiles_drum_pad_for_step(local);
        bool armed = tiles_drum_pattern_is_armed(&s_pattern, note, step);
        /* The playhead shows playing or parked, as in the sequencer: blue on a
         * step that sounds, dim white on an empty one. */
        if (step == s_player.step) {
            if (armed) {
                tiles_lighting_set_standby_pad_rgb(pad, DRUM_BLUE_R, DRUM_BLUE_G, DRUM_BLUE_B);
            } else {
                tiles_lighting_set_standby_pad_rgb(pad, DRUM_PLAYHEAD_LEVEL, DRUM_PLAYHEAD_LEVEL, DRUM_PLAYHEAD_LEVEL);
            }
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
        float r, g, b;
        if (now - s_drum_cleared_ms[v] < DRUM_CLEAR_FLASH_MS && s_drum_cleared_ms[v] != 0u) {
            r = 1.0f; g = 0.0f; b = 0.0f; /* cleared */
        } else if (s_drum_sounding[v]) {
            r = g = b = 1.0f; /* held after a push, or rolling */
        } else if (s_drum_fired_any[v] && now - s_drum_fired_ms[v] < DRUM_FIRE_FLASH_MS) {
            r = g = b = DRUM_FIRE_FLASH_LEVEL;
        } else if (v == s_voice) {
            r = DRUM_BLUE_R; g = DRUM_BLUE_G; b = DRUM_BLUE_B;
        } else {
            bool used = tiles_drum_pattern_note_has_steps(&s_pattern, tiles_drum_note(s_bank, v));
            bank_green(used ? DRUM_PAD_USED_LEVEL : DRUM_PAD_EMPTY_LEVEL, &r, &g, &b);
        }
        tiles_lighting_set_standby_pad_rgb(pad, r, g, b);
    }
}
