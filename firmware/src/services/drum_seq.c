#include "drum_seq.h"

#include "board_layout.h"
#include "board_pins.h"
#include "buttons.h"
#include "drum_pattern.h"
#include "expression.h"
#include "hall.h"
#include "haptics.h"
#include "kv_store.h"
#include "light_effects.h"
#include "log_store.h"
#include "lighting.h"
#include "midi_channels.h"
#include "midi_out.h"
#include "touch.h"

#include "pico/time.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* Circle + a drum held: this long clears its part; on to
 * DRUM_CLEAR_ALL_HOLD_MS, the whole pattern. A circle touch never plays. */
#define DRUM_CLEAR_HOLD_MS 1000u
#define DRUM_CLEAR_ALL_HOLD_MS 3000u
#define DRUM_CLEAR_FLASH_MS 400u
/* How long a drum pad flashes white when its drum plays. First guess. */
#define DRUM_FIRE_FLASH_MS 90u
/* A finger resting on a drum pad (touching, not pushed) feels that drum's
 * steps: a full-strength kick on each, cut after DRUM_FEEL_KICK_MS (the
 * kick is 45 ms, then an 8 ms gap, haptics.c) so it never turns into a
 * sustain buzz. */
#define DRUM_FEEL_KICK_VELOCITY 127u
#define DRUM_FEEL_KICK_MS 50u
/* Step rolls (circle + a step): 1 hit per step (a step repeat), then 2, 3,
 * 4 as the depth passes these raw Hall depths (first guesses). Every hit
 * is at the step velocity (pressure sets the rate only). */
#define DRUM_ROLL_DEPTH_16TH 300u
#define DRUM_ROLL_DEPTH_32ND 550u
#define DRUM_ROLL_DEPTH_32ND_T 800u
/* Saving: DRUM_SAVE_QUIET_MS after the last change, no pad touched. While
 * the clock runs only into already-erased flash (log_store.h: page
 * programs, well under a millisecond); an erase waits for the clock to
 * stop, so the beat never stalls. A failed write retries after
 * DRUM_SAVE_RETRY_MS. */
#define DRUM_SAVE_QUIET_MS 2000u
#define DRUM_SAVE_RETRY_MS 30000u

/* Levels; steps in the mode's orange, drum pads in their bank's shade. */
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

/* Step roll (circle + a step), slip style: every drum armed on the step
 * retriggers while the pattern runs on silently (s_player.muted). */
static bool s_rep_active;
static uint8_t s_rep_step; /* 0-31 */
static uint8_t s_rep_pad;
static uint32_t s_rep_next_ms;

/* Drum pads played live (strike detection as in chord mode), and the
 * circle-hold clear. */
static bool s_drum_awaiting[TILES_DRUM_VOICES];
static uint32_t s_drum_touch_ms[TILES_DRUM_VOICES];
static float s_drum_peak[TILES_DRUM_VOICES];
static bool s_drum_sounding[TILES_DRUM_VOICES];
static uint8_t s_drum_sounding_note[TILES_DRUM_VOICES];
static uint32_t s_drum_clear_ms[TILES_DRUM_VOICES]; /* clear-hold started, 0 = none */
static uint8_t s_drum_clear_stage[TILES_DRUM_VOICES]; /* 0, 1 = part cleared, 2 = all cleared */
static uint32_t s_drum_cleared_ms[TILES_DRUM_VOICES];
static uint32_t s_drum_fired_ms[TILES_DRUM_VOICES];
static bool s_drum_fired_any[TILES_DRUM_VOICES];
static uint32_t s_feel_stop_ms[TILES_DRUM_VOICES]; /* 0 = no resting-finger kick to end */
static uint32_t s_all_cleared_ms;

/* Saving (see DRUM_SAVE_QUIET_MS). */
static tiles_log_t s_log;
static bool s_storage;
static bool s_dirty;
static uint32_t s_changed_ms;
static uint32_t s_retry_after_ms;
/* The player started a step since the last persist_service() (advance runs
 * more often than the scan). */
static bool s_step_entered;
static uint32_t s_saves;
static uint16_t s_saved_bytes;
static uint8_t s_blob[TILES_LOG_MAX_PAYLOAD];

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

/* ---- the player's MIDI output ---- */

/* Drum Rack (channel 10, the drum's own note) or Volca (drum_seq.h). */
static bool s_volca;

/* Volca: the drum's MIDI channel (0-15) from its Drum Rack note; false past
 * channel 16 or below C1. */
static bool volca_channel(uint8_t note, uint8_t *channel) {
    if (note < TILES_DRUM_FIRST_NOTE || note - TILES_DRUM_FIRST_NOTE >= 16u) {
        return false;
    }
    *channel = (uint8_t)(note - TILES_DRUM_FIRST_NOTE);
    return true;
}

/* On the Parts USB port either way (midi/midi_ports.h): Volca's channels
 * 1-16 would otherwise reach a DAW's MPE track on the MIDI port. */
static void drum_midi_on(uint8_t note, uint8_t velocity) {
    uint8_t channel;
    tiles_midi_out_parts_begin();
    if (!s_volca) {
        tiles_midi_note_on(TILES_MIDI_CH_DRUMS, note, velocity);
    } else if (volca_channel(note, &channel)) {
        tiles_midi_note_on(channel, TILES_VOLCA_NOTE, velocity);
    }
    tiles_midi_out_parts_end();
}

static void drum_midi_off(uint8_t note) {
    uint8_t channel;
    tiles_midi_out_parts_begin();
    if (!s_volca) {
        tiles_midi_note_off(TILES_MIDI_CH_DRUMS, note, 0u);
    } else if (volca_channel(note, &channel)) {
        tiles_midi_note_off(channel, TILES_VOLCA_NOTE, 0u);
    }
    tiles_midi_out_parts_end();
}

/* The mode's color: orange, or cyan in Volca mode. */
static void mode_color(float *r, float *g, float *b) {
    *r = s_volca ? TILES_VOLCA_COLOR_R : TILES_DRUM_SEQ_COLOR_R;
    *g = s_volca ? TILES_VOLCA_COLOR_G : TILES_DRUM_SEQ_COLOR_G;
    *b = s_volca ? TILES_VOLCA_COLOR_B : TILES_DRUM_SEQ_COLOR_B;
}

static void out_note_on(uint8_t note, uint8_t velocity, void *ctx) {
    (void)ctx;
    drum_midi_on(note, velocity);
    uint32_t now = now_ms();
    for (uint8_t v = 0; v < TILES_DRUM_VOICES; v++) {
        if (tiles_drum_note(s_bank, v) == note) {
            s_drum_fired_ms[v] = now;
            s_drum_fired_any[v] = true;
            /* A finger resting on this drum feels its step. */
            uint8_t pad = tiles_drum_pad_for_voice(v);
            if (s_shown && !s_drum_sounding[v] && tiles_touch_is_touched(pad)) {
                tiles_haptics_trigger_kick(pad, DRUM_FEEL_KICK_VELOCITY);
                s_feel_stop_ms[v] = (now + DRUM_FEEL_KICK_MS) | 1u;
            }
        }
    }
    /* The selected drum's hits pulse their step pad, so a finger resting on
     * the grid feels the beat, and light it (the light effect's sequencer
     * version). */
    if (s_shown && note == tiles_drum_note(s_bank, s_voice) && !s_rep_active &&
        s_player.step / TILES_DRUM_PAGE_STEPS == s_page) {
        uint8_t step_pad = tiles_drum_pad_for_step(s_player.step);
        tiles_haptics_trigger_touch_pulse(step_pad);
        tiles_fx_playback_hit(step_pad, velocity, now);
    }
}

static void out_note_off(uint8_t note, void *ctx) {
    (void)ctx;
    drum_midi_off(note);
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
    for (uint8_t v = 0; v < TILES_DRUM_VOICES; v++) {
        s_drum_awaiting[v] = false;
        s_drum_clear_ms[v] = 0u;
    }
}

static void step_roll_end(void) {
    if (s_rep_active) {
        s_rep_active = false;
        s_player.muted = false;
        tiles_drum_player_end_all(&s_player, &s_out);
    }
}

static void end_live_notes(void) {
    step_roll_end();
    for (uint8_t v = 0; v < TILES_DRUM_VOICES; v++) {
        if (s_drum_sounding[v]) {
            drum_midi_off(s_drum_sounding_note[v]);
            tiles_haptics_stop(tiles_drum_pad_for_voice(v));
            tiles_fx_release(tiles_drum_pad_for_voice(v), now_ms());
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
    s_rep_active = false;
    for (uint8_t v = 0; v < TILES_DRUM_VOICES; v++) {
        s_drum_sounding[v] = false;
        s_drum_cleared_ms[v] = 0u;
        s_drum_fired_any[v] = false;
    }
    s_all_cleared_ms = 0u;
    resync_touches();
}

void tiles_drum_seq_set_volca(bool volca) {
    if (volca == s_volca) {
        return;
    }
    end_live_notes();
    tiles_drum_player_end_all(&s_player, &s_out);
    s_volca = volca;
}

bool tiles_drum_seq_is_volca(void) {
    return s_volca;
}

void tiles_drum_seq_enter(void) {
    resync_touches();
}

void tiles_drum_seq_leave(void) {
    end_live_notes();
}

void tiles_drum_seq_end_all_notes(void) {
    end_live_notes();
    tiles_drum_player_end_all(&s_player, &s_out);
}

/* ---- transport and banks ---- */

void tiles_drum_seq_set_phase_lock(bool on) {
    s_player.phase_lock = on;
}

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
    /* End resting-finger kicks before they reach their sustain. A pad pushed
     * meanwhile owns its haptic (drum_hit()). */
    uint32_t now = now_ms();
    for (uint8_t v = 0; v < TILES_DRUM_VOICES; v++) {
        if (s_feel_stop_ms[v] != 0u && (int32_t)(now - s_feel_stop_ms[v]) >= 0) {
            if (!s_drum_sounding[v]) {
                tiles_haptics_stop(tiles_drum_pad_for_voice(v));
            }
            s_feel_stop_ms[v] = 0u;
        }
    }
    s_pulse = clock.pulse_count;
    s_clock_running = clock.running;
    uint32_t started = s_player.step_started_pulse;
    uint8_t step = s_player.step;
    if (clock.start_edge && s_player.running) {
        /* At the song position, so a DAW playing from mid-song lands right. */
        tiles_drum_player_start_at(&s_player, &s_pattern, &s_out, clock.pulse_count, clock.song_pulse);
    } else {
        tiles_drum_player_advance(&s_player, &s_pattern, &s_out, clock.pulse_count, clock.song_pulse, clock.running,
                                  clock.start_edge);
    }
    if (s_player.step_started_pulse != started || s_player.step != step) {
        s_step_entered = true;
    }
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
    bool step_entered = s_step_entered;
    s_step_entered = false;
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
    if (clock_running && s_player.running && (!step_entered || tiles_drum_player_repeats_pending(&s_player))) {
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

/* ---- drum pads: hits, rolls, clears ---- */

static void drum_hit(uint8_t voice, uint8_t pad, uint8_t velocity) {
    if (s_drum_sounding[voice]) {
        drum_midi_off(s_drum_sounding_note[voice]);
    }
    uint8_t note = tiles_drum_note(s_bank, voice);
    drum_midi_on(note, velocity);
    tiles_haptics_trigger_kick(pad, velocity);
    tiles_fx_hit(pad, velocity, now_ms()); /* the light effect, as in play */
    s_drum_sounding[voice] = true;
    s_drum_sounding_note[voice] = note;
    s_drum_fired_ms[voice] = now_ms();
    s_drum_fired_any[voice] = true;
}

static void handle_drum_pad(uint8_t voice, uint8_t pad, bool touched, bool was, bool circle, uint32_t now) {
    if (touched && !was) {
        s_voice = voice; /* a tap selects */
        tiles_haptics_trigger_touch_pulse(pad);
        /* Circle first: a clear-hold, never a hit. */
        s_drum_awaiting[voice] = !circle;
        s_drum_touch_ms[voice] = now;
        s_drum_peak[voice] = (float)tiles_hall_get_depth(pad);
        s_drum_clear_ms[voice] = circle ? (now == 0u ? 1u : now) : 0u;
        s_drum_clear_stage[voice] = 0u;
    } else if (touched) {
        if (s_drum_awaiting[voice]) {
            /* A push: past the strike depth (as melodic and chord mode), not a
             * light touch. */
            float depth = (float)tiles_hall_get_depth(pad);
            if (depth > s_drum_peak[voice]) {
                s_drum_peak[voice] = depth;
            }
            if (s_drum_peak[voice] >= TILES_EXPRESSION_MIN_STRIKE_DEPTH_DELTA) {
                drum_hit(voice, pad,
                         tiles_expression_velocity_from_strike(now - s_drum_touch_ms[voice], s_drum_peak[voice]));
                s_drum_awaiting[voice] = false;
            }
        }
        if (s_drum_clear_ms[voice] != 0u) {
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
            drum_midi_off(s_drum_sounding_note[voice]);
            tiles_haptics_stop(pad);
            tiles_fx_release(pad, now);
            s_drum_sounding[voice] = false;
        }
        s_drum_awaiting[voice] = false;
        s_drum_clear_ms[voice] = 0u;
    }
}

/* ---- step roll: circle + a step ---- */

static uint8_t step_roll_hits_for(uint16_t depth) {
    return depth >= DRUM_ROLL_DEPTH_32ND_T ? 4u : depth >= DRUM_ROLL_DEPTH_32ND ? 3u : depth >= DRUM_ROLL_DEPTH_16TH ? 2u : 1u;
}

static uint32_t step_roll_interval_ms(uint16_t depth) {
    float step_ms = tiles_midi_clock_get_ms_per_beat() / 4.0f;
    uint32_t ms = (uint32_t)(step_ms / (float)step_roll_hits_for(depth) + 0.5f);
    return ms < 1u ? 1u : ms;
}

static void step_roll_hit(void) {
    tiles_drum_player_fire_step(&s_player, &s_pattern, &s_out, s_rep_step, TILES_DRUM_STEP_VELOCITY);
    tiles_haptics_trigger_touch_pulse(s_rep_pad);
}

/* Hits `step` at once and keeps hitting while held (step_roll_service());
 * the pattern runs on silently underneath and plays on from wherever it has
 * got to on release. */
static void step_roll_begin(uint8_t step, uint8_t pad, uint32_t now) {
    s_rep_active = true;
    s_rep_step = step;
    s_rep_pad = pad;
    s_player.muted = true;
    step_roll_hit();
    s_rep_next_ms = now + step_roll_interval_ms(tiles_hall_get_depth(pad));
}

static void step_roll_service(uint32_t now) {
    if (!tiles_touch_is_touched(s_rep_pad)) {
        step_roll_end();
        return;
    }
    if ((int32_t)(now - s_rep_next_ms) >= 0) {
        step_roll_hit();
        uint32_t interval = step_roll_interval_ms(tiles_hall_get_depth(s_rep_pad));
        s_rep_next_ms += interval;
        if ((int32_t)(now - s_rep_next_ms) >= 0) {
            s_rep_next_ms = now + interval; /* fell behind: don't burst */
        }
    }
}

/* ---- input ---- */

void tiles_drum_seq_flip_page(void) {
    s_page ^= 1u;
}

void tiles_drum_seq_handle_input(uint32_t now) {
    if (s_rep_active) {
        step_roll_service(now);
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
            if (circle) {
                if (!s_rep_active) {
                    step_roll_begin(step, pad, now); /* circle + step: roll it */
                }
            } else {
                /* A touch toggles at once (no hold gesture on steps). */
                tiles_drum_pattern_toggle(&s_pattern, note, step);
                mark_changed();
            }
        }
    }
}

/* ---- rendering ---- */

/* Each bank of 8 drums has its own shade of the mode's color on the drum
 * pads, so a new set reads as new. Drum mode: orange (the first bank), a
 * redder one, amber, rose. Volca mode: cyan, a darker cyan, a paler one, a
 * bluer one. Then around again. */
static void bank_shade(float level, float *r, float *g, float *b) {
    static const float WARMS[4][3] = {
        {TILES_DRUM_SEQ_COLOR_R, TILES_DRUM_SEQ_COLOR_G, TILES_DRUM_SEQ_COLOR_B},
        {1.0f, 0.1f, 0.0f},
        {1.0f, 0.6f, 0.0f},
        {1.0f, 0.05f, 0.25f},
    };
    static const float CYANS[4][3] = {
        {TILES_VOLCA_COLOR_R, TILES_VOLCA_COLOR_G, TILES_VOLCA_COLOR_B},
        {0.0f, 0.4f, 0.55f},
        {0.35f, 0.9f, 1.0f},
        {0.0f, 0.45f, 1.0f},
    };
    const float (*shades)[3] = s_volca ? CYANS : WARMS;
    uint8_t i = (uint8_t)((((int)s_bank % 4) + 4) % 4);
    *r = shades[i][0] * level;
    *g = shades[i][1] * level;
    *b = shades[i][2] * level;
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
        float r, g, b;
        mode_color(&r, &g, &b);
        tiles_lighting_set_standby_underglow_rgb(i, r, g, b);
    }
}

void tiles_drum_seq_render(uint32_t now, float beat_flash, float diamond_led, bool clock_running) {
    render_buttons(now, beat_flash, diamond_led, clock_running);
    render_underglow();
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
        /* A rolling step is white; the playhead shows playing or parked, as
         * in the sequencer: blue on a step that sounds, dim white on an empty
         * one. */
        if (s_rep_active && step == s_rep_step) {
            tiles_lighting_set_standby_pad_rgb(pad, 1.0f, 1.0f, 1.0f);
        } else if (step == s_player.step) {
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
            float r, g, b;
            mode_color(&r, &g, &b);
            tiles_lighting_set_standby_pad_rgb(pad, r * level, g * level, fmaxf(b * level, blue));
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
            /* Held after a push: white, or Touch colour's tint. */
            float tint[3];
            if (tiles_fx_pressed_colour(pad, tint)) {
                r = tint[0]; g = tint[1]; b = tint[2];
            } else {
                r = g = b = 1.0f;
            }
        } else if (s_drum_fired_any[v] && now - s_drum_fired_ms[v] < DRUM_FIRE_FLASH_MS) {
            r = g = b = DRUM_FIRE_FLASH_LEVEL;
        } else if (v == s_voice) {
            r = DRUM_BLUE_R; g = DRUM_BLUE_G; b = DRUM_BLUE_B;
        } else {
            bool used = tiles_drum_pattern_note_has_steps(&s_pattern, tiles_drum_note(s_bank, v));
            bank_shade(used ? DRUM_PAD_USED_LEVEL : DRUM_PAD_EMPTY_LEVEL, &r, &g, &b);
        }
        tiles_lighting_set_standby_pad_rgb(pad, r, g, b);
    }
}
