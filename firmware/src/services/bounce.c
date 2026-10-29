#include "bounce.h"

#include "board_layout.h"
#include "board_pins.h"
#include "bounce_core.h"
#include "buttons.h"
#include "expression.h"
#include "hall.h"
#include "haptics.h"
#include "lighting.h"
#include "midi_clock.h"
#include "midi_channels.h"
#include "midi_out.h"
#include "note_map.h"
#include "touch.h"

#include "pico/time.h"

#include <math.h>

#define BOUNCE_CHANNEL TILES_MIDI_CH_BOUNCE
#define BOUNCE_VISIBLE_LANES 6u
#define BOUNCE_MAX_OFFSET (TILES_BOUNCE_LANES - BOUNCE_VISIBLE_LANES)
#define BOUNCE_PULSES_PER_STEP 12u /* an eighth at 24 pulses per beat (sixteenths were too busy) */
#define BOUNCE_PULSES_PER_BEAT 24u
/* Lane 1's note before the key and octave: C3. */
#define BOUNCE_BASE_NOTE 48u
#define BOUNCE_PI 3.14159265358979323846f

/* Landing velocity by drop height (index = height): a higher drop lands
 * harder. First guesses. */
static const uint8_t VELOCITY_FOR_HEIGHT[TILES_BOUNCE_MAX_HEIGHT + 1u] = {0u, 84u, 98u, 112u, 127u};

/* Hold a pad this long to pick its lane's pitch (the sequencer's step hold,
 * OP_SEQ_PITCH_ASSIGN_HOLD_MS). */
#define BOUNCE_PICK_HOLD_MS 350u

/* Light levels and times: first guesses. */
#define BOUNCE_HEIGHT_LEVEL 0.35f
#define BOUNCE_EMPTY_LANE_LEVEL 0.1f
#define BOUNCE_LAND_FLASH_MS 140u /* white fading back to the lane's colour */
#define BOUNCE_END_FLASH_MS 220u
#define BOUNCE_TRANSPORT_LED_LEVEL 1.0f
/* The pitch picker: the melodic look (root magenta, naturals white, sharps
 * dark), the lane's note pulsing white, as in the sequencer's pitch edit. */
#define BOUNCE_PICK_AVAILABLE_LEVEL 0.5f
#define BOUNCE_PICK_PULSE_MS 1200.0f

static tiles_bounce_t s_balls;
static bool s_running;
static uint32_t s_step_pulse;   /* the clock pulse the last step fell on */
static uint8_t s_offset;        /* first lane shown, 0..BOUNCE_MAX_OFFSET */
static uint8_t s_sounding[TILES_BOUNCE_LANES]; /* note playing per lane, 0 = none */
static uint32_t s_landed_ms[TILES_BOUNCE_LANES];
static uint32_t s_end_flash_ms;
static bool s_rewound;
/* "+" under an external clock: the balls drop on its next beat. */
static bool s_start_on_beat;
/* On the pads (op_mode.c): landings are felt only then. */
static bool s_shown;
/* The clock as advance() last saw it, for drawing between steps. */
static uint32_t s_last_pulse_count;
static uint32_t s_last_pulse_ms;
static bool s_clock_running;

/* Smooth drawing: the frame wanted, and what each pad was last sent. A pad
 * write is ~0.3 ms (services/lighting.c), so at most
 * BOUNCE_PAD_WRITES_PER_SCAN go out per scan, the biggest changes first:
 * a whole repaint at once would hold the main loop (and the clock's
 * playback ticks) for ~17 ms. Not drawn for BOUNCE_REPAINT_AFTER_MS (a menu
 * was up, another mode drew): every pad is sent again. */
#define BOUNCE_PAD_WRITES_PER_SCAN 2u
#define BOUNCE_PAD_CHANGE_MIN 0.02f
#define BOUNCE_REPAINT_AFTER_MS 20u
static float s_frame[TILES_NUM_PADS][3];
static float s_sent[TILES_NUM_PADS][3];
static uint32_t s_last_render_ms;
static bool s_prev_touched[TILES_NUM_PADS];
static uint32_t s_touch_started_ms[TILES_NUM_PADS]; /* 0 = not a tap in progress */
/* A lane's picked note, 0 = none (it follows the scale, lane_note()). */
static uint8_t s_picked_note[TILES_BOUNCE_LANES];
#define BOUNCE_NOT_PICKING 0xFFu
static uint8_t s_pick_lane = BOUNCE_NOT_PICKING;

/* ---- Real gravity (bounce.h; the physics is bounce_core.h) ---- */
/* Throw: the pad's row sets the base apex (0 on the floor row .. 3 on the
 * top), the strike adds up to BOUNCE_THROW_FORCE_PADS (a hard tap on the
 * top row flies off the grid and comes back). Landing velocity follows the
 * height it fell from, full at BOUNCE_IMPACT_FULL pads. A note lasts until
 * the lane lands again, an eighth at most. A square click toggles it
 * (expression_control.c sees the click, op_mode.c passes it here). First
 * guesses. */
#define BOUNCE_THROW_FORCE_PADS 2.0f
#define BOUNCE_THROW_MIN_APEX 0.5f
#define BOUNCE_IMPACT_FULL 5.0f
#define BOUNCE_GRAVITY_MIN_VELOCITY 30.0f
static bool s_gravity;
static tiles_gravity_t s_grav;
static float s_last_throw[TILES_BOUNCE_LANES]; /* "+" throws them again */
static uint32_t s_grav_last_ms;
static uint32_t s_note_off_ms[TILES_BOUNCE_LANES]; /* 0 = none due (both modes) */
/* Back to the plain balls: their next step lands on the eighth grid. */
static bool s_step_realign;
/* A throw's strength, per pad: how far it was pressed and how fast it got
 * past the strike depth (expression's velocity curve). */
static float s_peak_depth[TILES_NUM_PADS];
static bool s_crossed[TILES_NUM_PADS];
static uint32_t s_crossed_ms[TILES_NUM_PADS];

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

/* Lane k's note: the k-th note of the scale from the key note up (the key
 * note itself for lane 0), at the octave set elsewhere; or the note picked
 * for it, fitted to the scale as the sequencer's steps are. */
static uint8_t lane_note(uint8_t lane) {
    if (s_picked_note[lane] != 0u) {
        return tiles_note_map_quantize_to_scale(s_picked_note[lane]);
    }
    int base = (int)BOUNCE_BASE_NOTE + tiles_note_map_get_key_offset() + 12 * tiles_note_map_get_octave_shift();
    uint8_t found = 0u;
    for (int note = base; note < 128; note++) {
        if (note < 0) {
            continue;
        }
        if (tiles_note_map_quantize_to_scale((uint8_t)note) == (uint8_t)note) {
            if (found == lane) {
                return (uint8_t)note;
            }
            found++;
        }
    }
    return 0u;
}

static void lane_off(uint8_t lane) {
    if (s_sounding[lane] != 0u) {
        tiles_midi_note_off(BOUNCE_CHANNEL, s_sounding[lane], 0u);
        s_sounding[lane] = 0u;
    }
}

void tiles_bounce_mode_end_all_notes(void) {
    for (uint8_t lane = 0; lane < TILES_BOUNCE_LANES; lane++) {
        lane_off(lane);
    }
}

/* The column's bottom pad, when the lane is on screen (0 if not). */
static uint8_t floor_pad_for(uint8_t lane) {
    if (lane < s_offset || lane >= s_offset + BOUNCE_VISIBLE_LANES) {
        return 0u;
    }
    return board_pad_for_row_col(TILES_GRID_MAX_ROW, (uint8_t)(lane - s_offset + 1u));
}

/* Every note first, then the haptics: a motor's start is an I2C write, and
 * between notes it would hold back the lanes after it. */
static void play_landings(uint16_t landed) {
    uint32_t now = now_ms();
    for (uint8_t lane = 0; lane < TILES_BOUNCE_LANES; lane++) {
        if (!((landed >> lane) & 1u)) {
            continue;
        }
        lane_off(lane);
        uint8_t note = lane_note(lane);
        if (note != 0u) {
            tiles_midi_note_on(BOUNCE_CHANNEL, note, VELOCITY_FOR_HEIGHT[s_balls.height[lane]]);
            s_sounding[lane] = note;
        }
        s_landed_ms[lane] = now | 1u;
    }
    for (uint8_t lane = 0; lane < TILES_BOUNCE_LANES && s_shown; lane++) {
        uint8_t pad = floor_pad_for(lane);
        if (((landed >> lane) & 1u) && pad != 0u) {
            tiles_haptics_trigger_touch_pulse(pad); /* feel the bounce */
        }
    }
}

static void gravity_catch_all(void) {
    tiles_gravity_clear(&s_grav);
    for (uint8_t lane = 0; lane < TILES_BOUNCE_LANES; lane++) {
        lane_off(lane);
        s_note_off_ms[lane] = 0u;
    }
}

static void gravity_throw(uint8_t lane, uint8_t pad, float apex) {
    if (apex < BOUNCE_THROW_MIN_APEX) {
        apex = BOUNCE_THROW_MIN_APEX;
    }
    s_last_throw[lane] = apex;
    tiles_gravity_throw(&s_grav, lane, apex);
    if (pad != 0u) {
        tiles_haptics_trigger_touch_pulse(pad);
    }
}

/* Free time, scaled by the tempo: notes first, then the haptics. */
/* Notes with an end time (gravity's, and any carried across a switch). */
static void note_offs_due(uint32_t now) {
    for (uint8_t lane = 0; lane < TILES_BOUNCE_LANES; lane++) {
        if (s_note_off_ms[lane] != 0u && (int32_t)(now - s_note_off_ms[lane]) >= 0) {
            lane_off(lane);
            s_note_off_ms[lane] = 0u;
        }
    }
}

static void gravity_advance(uint32_t now) {
    uint32_t dt_ms = now - s_grav_last_ms;
    s_grav_last_ms = now;
    float eighth_ms = tiles_midi_clock_get_ms_per_beat() * 0.5f;
    if (eighth_ms <= 0.0f || dt_ms == 0u) {
        return;
    }
    float impact[TILES_BOUNCE_LANES];
    uint16_t landed = tiles_gravity_step(&s_grav, (float)dt_ms / eighth_ms, impact);
    if (landed == 0u) {
        return;
    }
    for (uint8_t lane = 0; lane < TILES_BOUNCE_LANES; lane++) {
        if (!((landed >> lane) & 1u)) {
            continue;
        }
        lane_off(lane);
        uint8_t note = lane_note(lane);
        float hit = sqrtf(fminf(impact[lane], BOUNCE_IMPACT_FULL) / BOUNCE_IMPACT_FULL);
        uint8_t velocity = (uint8_t)(BOUNCE_GRAVITY_MIN_VELOCITY + (127.0f - BOUNCE_GRAVITY_MIN_VELOCITY) * hit);
        if (note != 0u) {
            tiles_midi_note_on(BOUNCE_CHANNEL, note, velocity);
            s_sounding[lane] = note;
            s_note_off_ms[lane] = (now + (uint32_t)eighth_ms) | 1u;
        }
        s_landed_ms[lane] = now | 1u;
    }
    for (uint8_t lane = 0; lane < TILES_BOUNCE_LANES && s_shown; lane++) {
        uint8_t pad = floor_pad_for(lane);
        if (((landed >> lane) & 1u) && pad != 0u) {
            tiles_haptics_trigger_touch_pulse(pad);
        }
    }
}

bool tiles_bounce_mode_is_gravity(void) {
    return s_gravity;
}

static float step_fraction(uint32_t now);

/* Switching keeps every ball where it is in its bounce, and the notes
 * sounding:
 *   - to gravity: a plain ball of height h becomes one with an apex of h-1
 *     pads (a height-1 dribble: the smallest throw), at the same point of
 *     its bounce; from there it loses height as gravity does;
 *   - back: each ball takes the plain height nearest its apex (a resting
 *     one is off), at the step of its bounce it was on, and the plain
 *     balls carry on, the next step on the eighth grid (they move while
 *     the clock runs, as always). */
static void gravity_from_plain(float frac) {
    tiles_gravity_clear(&s_grav);
    for (uint8_t lane = 0; lane < TILES_BOUNCE_LANES; lane++) {
        uint8_t h = s_balls.height[lane];
        if (h == 0u) {
            continue;
        }
        float apex = h > 1u ? (float)(h - 1u) : BOUNCE_THROW_MIN_APEX;
        float u = h > 1u ? ((float)s_balls.phase[lane] + frac) / (2.0f * (float)(h - 1u)) : frac;
        s_grav.apex[lane] = apex;
        s_grav.t[lane] = u * tiles_gravity_period(apex);
        s_last_throw[lane] = apex;
    }
}

static void plain_from_gravity(void) {
    bool any = false;
    for (uint8_t lane = 0; lane < TILES_BOUNCE_LANES; lane++) {
        float apex = s_grav.apex[lane];
        if (apex <= 0.0f) {
            s_balls.height[lane] = 0u;
            s_balls.phase[lane] = 0u;
            continue;
        }
        float rounded = roundf(apex) + 1.0f;
        uint8_t h = (uint8_t)fminf(fmaxf(rounded, 1.0f), (float)TILES_BOUNCE_MAX_HEIGHT);
        uint8_t period = tiles_bounce_period(h);
        float u = s_grav.t[lane] / tiles_gravity_period(apex);
        uint8_t phase = (uint8_t)(u * (float)period);
        s_balls.height[lane] = h;
        s_balls.phase[lane] = phase < period ? phase : (uint8_t)(period - 1u);
        any = true;
    }
    if (any) {
        s_running = true;
        s_rewound = false;
        s_step_realign = true;
    }
}

static void set_gravity(bool on) {
    if (on == s_gravity) {
        return;
    }
    uint32_t now = now_ms();
    if (on) {
        gravity_from_plain(step_fraction(now));
        /* Plain notes end on the next step, which won't come: give them an
         * eighth. */
        uint32_t eighth_ms = (uint32_t)(tiles_midi_clock_get_ms_per_beat() * 0.5f);
        for (uint8_t lane = 0; lane < TILES_BOUNCE_LANES; lane++) {
            if (s_sounding[lane] != 0u && s_note_off_ms[lane] == 0u) {
                s_note_off_ms[lane] = (now + eighth_ms) | 1u;
            }
        }
    } else {
        plain_from_gravity();
    }
    s_gravity = on;
    s_grav_last_ms = now;
}

void tiles_bounce_mode_toggle_gravity(void) {
    set_gravity(!s_gravity);
}

void tiles_bounce_mode_init(void) {
    tiles_bounce_clear(&s_balls);
    s_running = false;
    s_offset = 0u;
    s_end_flash_ms = 0u;
    s_rewound = true;
    for (uint8_t lane = 0; lane < TILES_BOUNCE_LANES; lane++) {
        s_sounding[lane] = 0u;
        s_landed_ms[lane] = 0u;
        s_picked_note[lane] = 0u;
    }
    tiles_bounce_mode_enter();
}

/* A finger already down isn't a new touch, nor a tap when it lifts. */
static void resync_touches(void) {
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        s_prev_touched[pad - 1u] = tiles_touch_is_touched(pad);
        s_touch_started_ms[pad - 1u] = 0u;
    }
}

void tiles_bounce_mode_enter(void) {
    s_pick_lane = BOUNCE_NOT_PICKING;
    resync_touches();
}

bool tiles_bounce_mode_is_picking(void) {
    return s_pick_lane != BOUNCE_NOT_PICKING;
}

void tiles_bounce_mode_cancel_pick(void) {
    s_pick_lane = BOUNCE_NOT_PICKING;
    resync_touches();
}

bool tiles_bounce_mode_is_running(void) {
    if (s_gravity) {
        for (uint8_t lane = 0; lane < TILES_BOUNCE_LANES; lane++) {
            if (s_grav.apex[lane] > 0.0f) {
                return true; /* a ball in the air */
            }
        }
        return false;
    }
    return s_running;
}

/* Real gravity's "+": every lane thrown again as it last was. */
static void gravity_rethrow_all(void) {
    for (uint8_t lane = 0; lane < TILES_BOUNCE_LANES; lane++) {
        if (s_last_throw[lane] > 0.0f) {
            gravity_throw(lane, 0u, s_last_throw[lane]);
        }
    }
}

void tiles_bounce_mode_start(bool from_floor) {
    if (s_gravity) {
        gravity_rethrow_all();
        return;
    }
    s_running = true;
    s_rewound = false;
    if (from_floor) {
        /* Beat one: every ball lands now, then bounces from the floor. */
        uint16_t all = 0u;
        for (uint8_t lane = 0; lane < TILES_BOUNCE_LANES; lane++) {
            s_balls.phase[lane] = 0u;
            if (s_balls.height[lane] != 0u) {
                all |= (uint16_t)(1u << lane);
            }
        }
        play_landings(all);
    }
}

void tiles_bounce_mode_start_on_beat(void) {
    if (s_gravity) {
        gravity_rethrow_all(); /* free time: at once */
        return;
    }
    if (!s_running) {
        s_start_on_beat = true;
    }
}

void tiles_bounce_mode_pause(void) {
    if (s_gravity) {
        gravity_catch_all(); /* "-": every ball caught */
        return;
    }
    s_running = false;
    s_start_on_beat = false;
    tiles_bounce_mode_end_all_notes();
}

void tiles_bounce_mode_rewind(void) {
    if (s_gravity) {
        gravity_catch_all();
        return;
    }
    tiles_bounce_drop_all(&s_balls);
    s_rewound = true;
}

void tiles_bounce_mode_scroll(int8_t direction) {
    int next = (int)s_offset + direction;
    if (next < 0) {
        next = 0;
    }
    if (next > (int)BOUNCE_MAX_OFFSET) {
        next = (int)BOUNCE_MAX_OFFSET;
    }
    s_offset = (uint8_t)next;
    if (s_offset == 0u || s_offset == BOUNCE_MAX_OFFSET) {
        s_end_flash_ms = now_ms() | 1u; /* at an end (or pushing past it) */
    }
}

void tiles_bounce_mode_advance(tiles_midi_clock_state_t clock, bool shown) {
    s_shown = shown;
    if (clock.pulse_count != s_last_pulse_count) {
        s_last_pulse_count = clock.pulse_count;
        s_last_pulse_ms = now_ms();
    }
    s_clock_running = clock.running;
    note_offs_due(now_ms());
    if (s_gravity) {
        /* The plain balls wait (their state kept) while gravity plays. */
        s_step_pulse = clock.pulse_count;
        gravity_advance(now_ms());
        return;
    }
    if (s_start_on_beat && clock.running) {
        /* s_step_pulse follows the clock while stopped (below), so a beat
         * boundary after it has passed since the last scan. */
        uint32_t beat = clock.pulse_count - clock.pulse_count % BOUNCE_PULSES_PER_BEAT;
        if (beat > s_step_pulse) {
            s_start_on_beat = false;
            tiles_bounce_mode_start(true);
            s_step_pulse = beat;
        }
    }
    if (!s_running || !clock.running) {
        s_step_pulse = clock.pulse_count; /* no catching up after a pause */
        return;
    }
    if (s_step_realign) {
        s_step_realign = false;
        s_step_pulse = clock.pulse_count - clock.song_pulse % BOUNCE_PULSES_PER_STEP;
    }
    if (clock.start_edge) {
        /* On the song's eighth-note grid (mid-song after a DAW's position). */
        s_step_pulse = clock.pulse_count - clock.song_pulse % BOUNCE_PULSES_PER_STEP;
        return;
    }
    while (clock.pulse_count - s_step_pulse >= BOUNCE_PULSES_PER_STEP) {
        s_step_pulse += BOUNCE_PULSES_PER_STEP;
        /* A note lasts one step, or until its lane lands again. */
        for (uint8_t lane = 0; lane < TILES_BOUNCE_LANES; lane++) {
            lane_off(lane);
        }
        play_landings(tiles_bounce_step(&s_balls));
    }
}

/* The picker is open: the first new touch, anywhere, is the lane's note. */
static void handle_pick(void) {
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        bool touched = tiles_touch_is_touched(pad);
        bool was = s_prev_touched[pad - 1u];
        s_prev_touched[pad - 1u] = touched;
        if (touched && !was) {
            s_picked_note[s_pick_lane] = tiles_note_map_get_note(pad);
            tiles_haptics_trigger_touch_pulse(pad);
            tiles_bounce_mode_cancel_pick(); /* closes it; this finger is no tap */
            return;
        }
    }
}

/* A tap (lifted before BOUNCE_PICK_HOLD_MS) drops the column's ball from
 * that pad's height, or stops it if it's already there; it acts on the
 * lift, so a hold changes nothing but the pitch. */
/* Real gravity's pads: a tap throws the column's ball when it lifts, as
 * high as the pad's row plus how hard it was hit; a hold picks the pitch
 * and throws nothing. */
static void handle_gravity_pads(uint32_t now) {
    for (uint8_t row = 1u; row <= TILES_GRID_MAX_ROW; row++) {
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            uint8_t pad = board_pad_for_row_col(row, col);
            uint8_t i = (uint8_t)(pad - 1u);
            bool touched = tiles_touch_is_touched(pad);
            bool was = s_prev_touched[i];
            s_prev_touched[i] = touched;
            uint8_t lane = (uint8_t)(s_offset + col - 1u);
            float depth = (float)tiles_hall_get_depth(pad);
            if (touched && !was) {
                s_touch_started_ms[i] = now | 1u;
                s_peak_depth[i] = depth;
                s_crossed[i] = depth >= TILES_EXPRESSION_MIN_STRIKE_DEPTH_DELTA;
                s_crossed_ms[i] = now;
            } else if (touched) {
                if (depth > s_peak_depth[i]) {
                    s_peak_depth[i] = depth;
                }
                if (!s_crossed[i] && s_peak_depth[i] >= TILES_EXPRESSION_MIN_STRIKE_DEPTH_DELTA) {
                    s_crossed[i] = true;
                    s_crossed_ms[i] = now;
                }
                if (s_touch_started_ms[i] != 0u && now - s_touch_started_ms[i] >= BOUNCE_PICK_HOLD_MS) {
                    s_pick_lane = lane;
                    resync_touches(); /* the held finger lifts without a throw */
                    tiles_haptics_trigger_touch_pulse(pad);
                    return;
                }
            } else if (was && s_touch_started_ms[i] != 0u) {
                float force = 0.0f;
                if (s_crossed[i]) {
                    uint32_t strike_ms = s_crossed_ms[i] - (s_touch_started_ms[i] & ~1u);
                    force = (float)tiles_expression_velocity_from_strike(strike_ms, s_peak_depth[i]) / 127.0f;
                }
                float base = (float)(TILES_GRID_MAX_ROW - row); /* pads above the floor */
                gravity_throw(lane, pad, base + BOUNCE_THROW_FORCE_PADS * force);
                s_touch_started_ms[i] = 0u;
            }
        }
    }
}

void tiles_bounce_mode_handle_input(uint32_t now) {
    if (tiles_bounce_mode_is_picking()) {
        handle_pick();
        return;
    }
    if (s_gravity) {
        handle_gravity_pads(now);
        return;
    }
    for (uint8_t row = 1u; row <= TILES_GRID_MAX_ROW; row++) {
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            uint8_t pad = board_pad_for_row_col(row, col);
            uint8_t i = (uint8_t)(pad - 1u);
            bool touched = tiles_touch_is_touched(pad);
            bool was = s_prev_touched[i];
            s_prev_touched[i] = touched;
            uint8_t lane = (uint8_t)(s_offset + col - 1u);
            if (touched && !was) {
                s_touch_started_ms[i] = now | 1u;
            } else if (touched) {
                if (s_touch_started_ms[i] != 0u && now - s_touch_started_ms[i] >= BOUNCE_PICK_HOLD_MS) {
                    s_pick_lane = lane;
                    resync_touches(); /* the held finger lifts without a tap */
                    tiles_haptics_trigger_touch_pulse(pad);
                    return;
                }
            } else if (was && s_touch_started_ms[i] != 0u) {
                s_touch_started_ms[i] = 0u;
                uint8_t height = (uint8_t)(TILES_GRID_MAX_ROW + 1u - row); /* bottom row = 1 */
                if (s_balls.height[lane] == height) {
                    tiles_bounce_set_height(&s_balls, lane, 0u); /* the same height again: off */
                    lane_off(lane);
                } else {
                    tiles_bounce_set_height(&s_balls, lane, height);
                }
                tiles_haptics_trigger_touch_pulse(pad);
            }
        }
    }
}

static float slow_pulse(uint32_t now, float period_ms, float lo, float hi) {
    float raw = 0.5f + 0.5f * sinf(2.0f * BOUNCE_PI * (float)now / period_ms);
    return lo + (hi - lo) * raw;
}

/* A lane's colour: the hue sweeps from deep blue on lane 1 through violet
 * and magenta to hot pink on lane 12, about 9 degrees a lane, so the
 * page's colours show where you've scrolled to. Full saturation. */
#define BOUNCE_LANE_HUE_FIRST 235.0f
#define BOUNCE_LANE_HUE_LAST 335.0f
static void lane_rgb(uint8_t lane, float *r, float *g, float *b) {
    float t = (float)lane / (float)(TILES_BOUNCE_LANES - 1u);
    float h = (BOUNCE_LANE_HUE_FIRST + t * (BOUNCE_LANE_HUE_LAST - BOUNCE_LANE_HUE_FIRST)) / 60.0f;
    float x = 1.0f - fabsf(fmodf(h, 2.0f) - 1.0f);
    *r = 0.0f;
    *g = 0.0f;
    *b = 0.0f;
    switch ((int)h % 6) { /* the hue's sixth of the circle */
    case 0:
        *r = 1.0f;
        *g = x;
        break;
    case 1:
        *r = x;
        *g = 1.0f;
        break;
    case 2:
        *g = 1.0f;
        *b = x;
        break;
    case 3:
        *g = x;
        *b = 1.0f;
        break;
    case 4:
        *r = x;
        *b = 1.0f;
        break;
    default:
        *r = 1.0f;
        *b = x;
        break;
    }
}

static void frame_set(uint8_t pad, float r, float g, float b) {
    s_frame[pad - 1u][0] = r;
    s_frame[pad - 1u][1] = g;
    s_frame[pad - 1u][2] = b;
}

static void render_pick(uint32_t now) {
    uint8_t current = lane_note(s_pick_lane);
    float pulse = slow_pulse(now, BOUNCE_PICK_PULSE_MS, 0.25f, 1.0f);
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        float r = 0.0f, g = 0.0f, b = 0.0f;
        if (tiles_note_map_get_note(pad) == current) {
            r = g = b = pulse;
        } else if (tiles_note_map_is_root_pad(pad)) {
            r = b = BOUNCE_PICK_AVAILABLE_LEVEL;
        } else if (tiles_note_map_is_natural_pad(pad)) {
            r = g = b = BOUNCE_PICK_AVAILABLE_LEVEL;
        }
        frame_set(pad, r, g, b);
    }
}

/* How far the current step has got, 0..1: whole pulses since it began plus
 * the time into the pulse, so a ball moves between Clocks too. 0 while
 * stopped. */
static float step_fraction(uint32_t now) {
    if (!s_running || !s_clock_running) {
        return 0.0f;
    }
    float pulse_ms = tiles_midi_clock_get_ms_per_beat() / 24.0f;
    float into_pulse = pulse_ms > 0.0f ? (float)(now - s_last_pulse_ms) / pulse_ms : 0.0f;
    if (into_pulse > 1.0f) {
        into_pulse = 1.0f;
    }
    float f = ((float)(s_last_pulse_count - s_step_pulse) + into_pulse) / (float)BOUNCE_PULSES_PER_STEP;
    return f < 0.0f ? 0.0f : (f > 0.999f ? 0.999f : f);
}

/* A ball's height above the floor, in pads (0 .. its drop height - 1): a
 * parabola over its bounce, fast at the floor and slow at the top like a
 * real one, landing exactly on the step that plays its note. */
static float ball_y(uint8_t lane, float frac) {
    uint8_t h = s_balls.height[lane];
    if (h <= 1u) {
        return 0.0f;
    }
    float period = 2.0f * (float)(h - 1u);
    float u = ((float)s_balls.phase[lane] + frac) / period;
    if (u > 1.0f) {
        u = 1.0f;
    }
    return (float)(h - 1u) * 4.0f * u * (1.0f - u);
}

/* Each lane: its drop height dim, the ball drawn across the two pads it's
 * between (a constant-power crossfade, so it doesn't dim halfway), a
 * landing flashing the floor white and fading back, an empty lane a faint
 * floor; all pads pink at the end of the lanes. */
static void render_lanes(uint32_t now) {
    bool end_flash = s_end_flash_ms != 0u && now - s_end_flash_ms < BOUNCE_END_FLASH_MS;
    float frac = step_fraction(now);
    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        uint8_t lane = (uint8_t)(s_offset + col - 1u);
        /* The plain mode: the drop height's pad marked. Real gravity: the
         * apex it will reach next (it sinks bounce by bounce); a ball above
         * the grid isn't drawn. */
        uint8_t height = s_balls.height[lane];
        float y = ball_y(lane, frac);
        if (s_gravity) {
            float apex = s_grav.apex[lane];
            height = apex <= 0.0f ? 0u : (uint8_t)fminf(1.0f + apex + 0.5f, (float)TILES_GRID_MAX_ROW + 1.0f);
            y = tiles_gravity_height(&s_grav, lane);
        }
        float lr, lg, lb;
        lane_rgb(lane, &lr, &lg, &lb);
        float ball[TILES_GRID_MAX_ROW + 8u] = {0}; /* by level, 1 = floor */
        if (height != 0u) {
            float pos = 1.0f + fminf(y, (float)TILES_GRID_MAX_ROW + 4.0f);
            uint8_t lo = (uint8_t)pos;
            float w = pos - (float)lo;
            ball[lo] += cosf(w * BOUNCE_PI * 0.5f);
            ball[lo + 1u] += sinf(w * BOUNCE_PI * 0.5f);
        }
        float flash = 0.0f;
        if (height != 0u && s_landed_ms[lane] != 0u && now - s_landed_ms[lane] < BOUNCE_LAND_FLASH_MS) {
            flash = 1.0f - (float)(now - s_landed_ms[lane]) / (float)BOUNCE_LAND_FLASH_MS;
        }
        for (uint8_t row = 1u; row <= TILES_GRID_MAX_ROW; row++) {
            uint8_t pad = board_pad_for_row_col(row, col);
            uint8_t level_h = (uint8_t)(TILES_GRID_MAX_ROW + 1u - row);
            if (end_flash) {
                frame_set(pad, 1.0f, 0.0f, 1.0f); /* the end of the lanes: pink */
                continue;
            }
            float level = 0.0f;
            if (height != 0u && level_h == height) {
                level = BOUNCE_HEIGHT_LEVEL;
            } else if (height == 0u && level_h == 1u) {
                level = BOUNCE_EMPTY_LANE_LEVEL;
            }
            level = fmaxf(level, fminf(ball[level_h], 1.0f));
            float r = lr * level, g = lg * level, b = lb * level;
            if (level_h == 1u && flash > 0.0f) {
                r += (1.0f - r) * flash;
                g += (1.0f - g) * flash;
                b += (1.0f - b) * flash;
            }
            frame_set(pad, r, g, b);
        }
    }
}

/* Sends the frame's biggest changes (see BOUNCE_PAD_WRITES_PER_SCAN). */
static void commit_frame(uint32_t now) {
    if (now - s_last_render_ms > BOUNCE_REPAINT_AFTER_MS) {
        for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
            s_sent[i][0] = s_sent[i][1] = s_sent[i][2] = -1.0f;
        }
    }
    s_last_render_ms = now;
    for (uint8_t n = 0; n < BOUNCE_PAD_WRITES_PER_SCAN; n++) {
        uint8_t best = TILES_NUM_PADS;
        float best_change = BOUNCE_PAD_CHANGE_MIN;
        for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
            float change = fmaxf(fabsf(s_frame[i][0] - s_sent[i][0]),
                                 fmaxf(fabsf(s_frame[i][1] - s_sent[i][1]), fabsf(s_frame[i][2] - s_sent[i][2])));
            if (change >= best_change) {
                best_change = change;
                best = i;
            }
        }
        if (best == TILES_NUM_PADS) {
            break;
        }
        tiles_lighting_set_standby_pad_rgb((uint8_t)(best + 1u), s_frame[best][0], s_frame[best][1], s_frame[best][2]);
        s_sent[best][0] = s_frame[best][0];
        s_sent[best][1] = s_frame[best][1];
        s_sent[best][2] = s_frame[best][2];
    }
}

void tiles_bounce_mode_render(uint32_t now, float beat_flash, float diamond_led, bool clock_running) {
    if (tiles_bounce_mode_is_picking()) {
        render_pick(now);
    } else {
        render_lanes(now);
    }
    commit_frame(now);
    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        float level = 0.0f;
        if (col == TILES_SQUARE_BUTTON_COL) {
            level = s_gravity ? BOUNCE_TRANSPORT_LED_LEVEL : 0.0f; /* Real gravity on */
        } else if (col == TILES_CIRCLE_BUTTON_COL) {
            level = beat_flash;
        } else if (col == TILES_DIAMOND_BUTTON_COL) {
            level = diamond_led;
        } else if (col == TILES_MINUS_BUTTON_COL && !s_running) {
            /* "-": solid when rewound, slow pulse when paused, as in the sequencer. */
            level = s_rewound ? BOUNCE_TRANSPORT_LED_LEVEL : slow_pulse(now, 3000.0f, 0.03f, 0.35f);
        } else if (col == TILES_PLUS_BUTTON_COL && (s_running || clock_running)) {
            level = s_running ? BOUNCE_TRANSPORT_LED_LEVEL
                              : slow_pulse(now, tiles_midi_clock_get_ms_per_beat(), 0.0f, 1.0f);
        }
        tiles_buttons_set_standby_led(board_button_for_col(col), level);
    }
    /* The balls light the underglow from above as they land. */
    tiles_lighting_set_standby_underglow_from_pads((const float(*)[3])s_frame, TILES_BOUNCE_COLOR_R, TILES_BOUNCE_COLOR_G,
                                                   TILES_BOUNCE_COLOR_B);
}
