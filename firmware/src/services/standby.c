#include "standby.h"

#include "board_layout.h"
#include "board_pins.h"
#include "buttons.h"
#include "haptics.h"
#include "hall.h"
#include "lighting.h"
#include "midi_in.h"
#include "midi_out.h"
#include "op_mode.h"
#include "pedal.h"
#include "pixel_font.h"
#include "touch.h"

#include "pico/rand.h"
#include "pico/time.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* 1 minute to standby; a new animation every 2 minutes (starting guesses). */
#define TILES_STANDBY_IDLE_TIMEOUT_MS 60000u
#define TILES_STANDBY_ANIMATION_CYCLE_MS 120000u

/* Sequencer mode: 20 min to standby, 30 to deep sleep. A pattern can run
 * unattended; 1 minute would blank the board while it plays. See
 * enter_standby() and current_deep_sleep_timeout_ms(). */
#define TILES_STANDBY_SEQUENCER_IDLE_TIMEOUT_MS 1200000u        /* 20 * 60 * 1000 */
#define TILES_STANDBY_SEQUENCER_DEEP_SLEEP_TIMEOUT_MS 1800000u /* 30 * 60 * 1000 (20 idle + 10 more) */

/* Deep sleep after 20 minutes of TOTAL inactivity (the same
 * s_last_activity_ms clock that starts standby). Holding circle 8 s
 * reaches the same state. See enter_deep_sleep(). */
#define TILES_STANDBY_DEEP_SLEEP_TIMEOUT_MS 1200000u /* 20 * 60 * 1000 */

/* ~25 fps. Every pad write is a mux sequence on I2C; if a full frame ever
 * competes with other bus traffic, raise this first. */
#define TILES_STANDBY_FRAME_INTERVAL_MS 40u

/* Hall-depth wake: a second wake path, because touch didn't reliably wake
 * standby while the pad animation ran (cause unconfirmed). Only checked
 * while already in standby (hall_depth_wake_triggered()), never to decide
 * whether to enter it: raw depth can look permanently "active" and kept
 * standby from ever starting.
 *
 * DISABLED until TILES_STANDBY_HALL_WAKE_DEPTH is set from real
 * rest-vs-light-press numbers (the earlier guess, taken before the magnets
 * were seated, made standby bounce straight back out). */
#define TILES_STANDBY_HALL_WAKE_ENABLED 0
#define TILES_STANDBY_HALL_WAKE_DEPTH 1000u

#define TILES_STANDBY_PI 3.14159265358979323846f

/* Grid bounds and the pad/button/underglow mapping live in
 * board/board_layout.h (shared with the boot animation). */

static float clamp01(float v) {
    if (v < 0.0f) {
        return 0.0f;
    }
    if (v > 1.0f) {
        return 1.0f;
    }
    return v;
}

static float rand01(void) {
    return (float)rand() / (float)RAND_MAX;
}

/* Animations return RGB (0.0-1.0 per channel); white ones use white(). */
typedef struct {
    float r;
    float g;
    float b;
} tiles_standby_color_t;

static tiles_standby_color_t white(float v) {
    v = clamp01(v);
    tiles_standby_color_t c = {v, v, v};
    return c;
}

/* ---- Animation: wave --------------------------------------------------
 * A diagonal band travelling across the grid (phase depends on row + col).
 * WAVE_CONTRAST_GAMMA > 1 keeps most of the cycle dark with a brighter
 * band passing through. */

#define WAVE_LENGTH_DIAG 3.0f
#define WAVE_PERIOD_MS 3000.0f
#define WAVE_CONTRAST_GAMMA 2.2f

static tiles_standby_color_t anim_wave(uint8_t row, uint8_t col, uint32_t now_ms) {
    float diag = (float)row + (float)col;
    float phase = diag / WAVE_LENGTH_DIAG - (float)now_ms / WAVE_PERIOD_MS;
    float raw = 0.5f + 0.5f * sinf(2.0f * TILES_STANDBY_PI * phase);
    return white(powf(clamp01(raw), WAVE_CONTRAST_GAMMA));
}

/* ---- Animation: glow ------------------------------------------------------
 * A sharp ring pulsing outward from the grid's center, on dark. */

#define GLOW_CENTER_ROW 2.0f
#define GLOW_CENTER_COL 3.5f
#define GLOW_MAX_RADIUS 4.2f
#define GLOW_PULSE_PERIOD_MS 2500.0f
#define GLOW_RING_WIDTH 0.5f

static tiles_standby_color_t anim_glow(uint8_t row, uint8_t col, uint32_t now_ms) {
    float dr = (float)row - GLOW_CENTER_ROW;
    float dc = (float)col - GLOW_CENTER_COL;
    float dist = sqrtf(dr * dr + dc * dc);
    float ring = fmodf((float)now_ms / GLOW_PULSE_PERIOD_MS * GLOW_MAX_RADIUS, GLOW_MAX_RADIUS);
    float delta = dist - ring;
    float raw = expf(-(delta * delta) / (2.0f * GLOW_RING_WIDTH * GLOW_RING_WIDTH));
    return white(clamp01(raw * raw));
}

/* ---- Animation: shooting stars -----------------------------------------
 * Comet-tailed points falling top to bottom, each respawning at a random
 * column, with randomized speed and tail length, exponential tail decay
 * and a slight twinkle. Keeps a small particle array. */

#define NUM_STARS 5u
#define STAR_TAIL_ROWS_MIN 2.0f
#define STAR_TAIL_ROWS_MAX 4.5f
/* Slowed to about half the original speed. */
#define STAR_SPEED_ROWS_PER_MS_MIN (1.0f / 600.0f)
#define STAR_SPEED_ROWS_PER_MS_MAX (1.0f / 280.0f)
#define STAR_TWINKLE_PERIOD_MS 90.0f
#define STAR_TWINKLE_DEPTH 0.25f
#define STAR_DECAY_SHARPNESS 2.2f

typedef struct {
    uint8_t col;
    uint32_t spawn_ms;
    float tail_rows;
    float speed_rows_per_ms;
    float twinkle_phase;
} star_t;

static star_t s_stars[NUM_STARS];
static bool s_stars_inited;

static void star_respawn(star_t *star, uint32_t spawn_ms) {
    star->col = (uint8_t)(TILES_GRID_MIN_COL + (uint8_t)(rand() % (TILES_GRID_MAX_COL - TILES_GRID_MIN_COL + 1u)));
    star->spawn_ms = spawn_ms;
    star->tail_rows = STAR_TAIL_ROWS_MIN + rand01() * (STAR_TAIL_ROWS_MAX - STAR_TAIL_ROWS_MIN);
    star->speed_rows_per_ms =
        STAR_SPEED_ROWS_PER_MS_MIN + rand01() * (STAR_SPEED_ROWS_PER_MS_MAX - STAR_SPEED_ROWS_PER_MS_MIN);
    star->twinkle_phase = rand01() * 6.2831853f;
}

static tiles_standby_color_t anim_shooting_stars(uint8_t row, uint8_t col, uint32_t now_ms) {
    if (!s_stars_inited) {
        for (uint8_t i = 0; i < NUM_STARS; i++) {
            /* Stagger the first spawns so the stars don't fall in lockstep. */
            star_respawn(&s_stars[i], now_ms - (uint32_t)i * 700u);
        }
        s_stars_inited = true;
    }

    float brightness = 0.0f;
    for (uint8_t i = 0; i < NUM_STARS; i++) {
        star_t *star = &s_stars[i];
        float head_row = (float)(now_ms - star->spawn_ms) * star->speed_rows_per_ms - star->tail_rows;

        if (head_row > (float)TILES_GRID_MAX_ROW + star->tail_rows) {
            star_respawn(star, now_ms);
            continue;
        }
        if (star->col != col) {
            continue;
        }

        float behind = head_row - (float)row;
        if (behind >= 0.0f && behind <= star->tail_rows) {
            float decay = expf(-STAR_DECAY_SHARPNESS * behind / star->tail_rows);
            float twinkle = 1.0f - STAR_TWINKLE_DEPTH * (0.5f + 0.5f * sinf((float)now_ms / STAR_TWINKLE_PERIOD_MS +
                                                                             star->twinkle_phase));
            float b = decay * twinkle;
            if (b > brightness) {
                brightness = b;
            }
        }
    }
    return white(clamp01(brightness));
}

/* ---- Animation: snake ----------------------------------------------------
 * A self-playing snake: a pulsing red food dot, the snake steers toward it
 * greedily with random perturbation (so paths vary), eats it and grows.
 * It restarts short at a random place when it gets too long or boxed in. */

#define SNAKE_STEP_MS 300u
#define SNAKE_MAX_LENGTH 14u /* reset length, well under the 30-cell grid */
/* How far each step's choice is nudged away from the greedy direction, in
 * cell-distance units: higher = more wandering. Starting guess. */
#define SNAKE_RANDOM_TURN_WEIGHT 1.5f

typedef struct {
    int8_t row;
    int8_t col;
} snake_cell_t;

static snake_cell_t s_snake_body[SNAKE_MAX_LENGTH]; /* [0] = head */
static uint8_t s_snake_length;
static snake_cell_t s_snake_food;
static uint32_t s_snake_last_step_ms;
static bool s_snake_inited;

static bool snake_cell_in_body(int8_t row, int8_t col) {
    for (uint8_t i = 0; i < s_snake_length; i++) {
        if (s_snake_body[i].row == row && s_snake_body[i].col == col) {
            return true;
        }
    }
    return false;
}

static void snake_place_food(void) {
    /* Bounded rejection sampling for an empty cell; finds one almost at once.
     * The fallback only matters when the snake nearly fills the board, and a
     * reset is imminent then anyway. */
    for (uint8_t attempt = 0; attempt < 50u; attempt++) {
        int8_t r = (int8_t)(TILES_GRID_MIN_ROW + (rand() % (TILES_GRID_MAX_ROW - TILES_GRID_MIN_ROW + 1u)));
        int8_t c = (int8_t)(TILES_GRID_MIN_COL + (rand() % (TILES_GRID_MAX_COL - TILES_GRID_MIN_COL + 1u)));
        if (!snake_cell_in_body(r, c)) {
            s_snake_food.row = r;
            s_snake_food.col = c;
            return;
        }
    }
    s_snake_food.row = (int8_t)TILES_GRID_MIN_ROW;
    s_snake_food.col = (int8_t)TILES_GRID_MIN_COL;
}

/* Random start position and direction, short length. Used for game over
 * and the first call, so every run looks different. */
static void snake_reset(uint32_t now_ms) {
    static const int8_t dir_row[4] = {-1, 1, 0, 0};
    static const int8_t dir_col[4] = {0, 0, -1, 1};

    int8_t start_row = (int8_t)(1 + rand() % (int)TILES_GRID_MAX_ROW); /* rows 1-4 */
    int8_t start_col = (int8_t)(TILES_GRID_MIN_COL + rand() % (TILES_GRID_MAX_COL - TILES_GRID_MIN_COL + 1u));
    uint8_t dir_index = (uint8_t)(rand() % 4u);
    int8_t dr = dir_row[dir_index];
    int8_t dc = dir_col[dir_index];

    /* Start with length 2 (small board; a longer start bunches at edges). */
    s_snake_length = 2u;
    for (uint8_t i = 0; i < s_snake_length; i++) {
        int8_t r = (int8_t)(start_row - dr * (int8_t)i);
        int8_t c = (int8_t)(start_col - dc * (int8_t)i);
        if (r < (int8_t)TILES_GRID_MIN_ROW) {
            r = (int8_t)TILES_GRID_MIN_ROW;
        }
        if (r > (int8_t)TILES_GRID_MAX_ROW) {
            r = (int8_t)TILES_GRID_MAX_ROW;
        }
        if (c < (int8_t)TILES_GRID_MIN_COL) {
            c = (int8_t)TILES_GRID_MIN_COL;
        }
        if (c > (int8_t)TILES_GRID_MAX_COL) {
            c = (int8_t)TILES_GRID_MAX_COL;
        }
        s_snake_body[i].row = r;
        s_snake_body[i].col = c;
    }

    snake_place_food();
    s_snake_last_step_ms = now_ms;
}

static void snake_step(uint32_t now_ms) {
    static const int8_t dir_row[4] = {-1, 1, 0, 0};
    static const int8_t dir_col[4] = {0, 0, -1, 1};

    snake_cell_t head = s_snake_body[0];
    int8_t best_dir = -1;
    float best_score = -1.0e9f;

    for (uint8_t i = 0; i < 4u; i++) {
        int8_t nr = (int8_t)(head.row + dir_row[i]);
        int8_t nc = (int8_t)(head.col + dir_col[i]);
        if (nr < (int8_t)TILES_GRID_MIN_ROW || nr > (int8_t)TILES_GRID_MAX_ROW) {
            continue;
        }
        if (nc < (int8_t)TILES_GRID_MIN_COL || nc > (int8_t)TILES_GRID_MAX_COL) {
            continue;
        }
        if (snake_cell_in_body(nr, nc)) {
            continue;
        }

        float dr = (float)(s_snake_food.row - nr);
        float dc = (float)(s_snake_food.col - nc);
        float dist = sqrtf(dr * dr + dc * dc);
        float score = -dist + (rand01() - 0.5f) * 2.0f * SNAKE_RANDOM_TURN_WEIGHT;
        if (score > best_score) {
            best_score = score;
            best_dir = (int8_t)i;
        }
    }

    if (best_dir < 0) {
        /* Boxed in: start over. */
        snake_reset(now_ms);
        return;
    }

    snake_cell_t new_head = {(int8_t)(head.row + dir_row[best_dir]), (int8_t)(head.col + dir_col[best_dir])};
    bool ate = (new_head.row == s_snake_food.row && new_head.col == s_snake_food.col);

    uint8_t new_length = ate ? (uint8_t)(s_snake_length + 1u) : s_snake_length;
    if (new_length > SNAKE_MAX_LENGTH) {
        snake_reset(now_ms);
        return;
    }

    for (uint8_t i = (uint8_t)(new_length - 1u); i > 0u; i--) {
        s_snake_body[i] = s_snake_body[i - 1u];
    }
    s_snake_body[0] = new_head;
    s_snake_length = new_length;

    if (ate) {
        snake_place_food();
    }
}

static void snake_update(uint32_t now_ms) {
    if (!s_snake_inited) {
        snake_reset(now_ms);
        s_snake_inited = true;
        return;
    }
    if (now_ms - s_snake_last_step_ms >= SNAKE_STEP_MS) {
        snake_step(now_ms);
        s_snake_last_step_ms = now_ms;
    }
}

#define SNAKE_HEAD_LEVEL 1.0f
#define SNAKE_BODY_LEVEL 0.75f
#define SNAKE_FOOD_PULSE_PERIOD_MS 700.0f
#define SNAKE_FOOD_MIN_LEVEL 0.6f
#define SNAKE_FOOD_MAX_LEVEL 1.0f

static tiles_standby_color_t anim_snake(uint8_t row, uint8_t col, uint32_t now_ms) {
    snake_update(now_ms);

    if ((int8_t)row == s_snake_food.row && (int8_t)col == s_snake_food.col) {
        float raw = 0.5f + 0.5f * sinf(2.0f * TILES_STANDBY_PI * (float)now_ms / SNAKE_FOOD_PULSE_PERIOD_MS);
        float level = SNAKE_FOOD_MIN_LEVEL + (SNAKE_FOOD_MAX_LEVEL - SNAKE_FOOD_MIN_LEVEL) * raw;
        tiles_standby_color_t c = {level, 0.0f, 0.0f};
        return c;
    }

    for (uint8_t i = 0; i < s_snake_length; i++) {
        if (s_snake_body[i].row == (int8_t)row && s_snake_body[i].col == (int8_t)col) {
            float level = (i == 0u) ? SNAKE_HEAD_LEVEL : SNAKE_BODY_LEVEL;
            tiles_standby_color_t c = {0.0f, level, 0.0f};
            return c;
        }
    }

    return white(0.0f);
}

/* ---- Animation: RGB showcase -------------------------------------------
 * The one colorful ambient animation: a diagonal wave (like wave) whose
 * hue moves within blue to violet. Underglow samples the same field. The
 * button row sits at a low, steady glow (buttons can't show color, and
 * fully dark looked like a glitch). */

#define RGB_WAVE_LENGTH_DIAG 3.0f
#define RGB_WAVE_PERIOD_MS 3500.0f
#define RGB_CONTRAST_GAMMA 1.8f
#define RGB_HUE_CYCLE_MS 6000.0f
#define RGB_HUE_MIN_DEG 220.0f /* blue */
#define RGB_HUE_MAX_DEG 285.0f /* violet */

/* Button-row level before render_frame()'s
 * BUTTON_STANDBY_BRIGHTNESS_SCALE (0.35), so the result is low. Unmeasured. */
#define RGB_SHOWCASE_BUTTON_ROW_LEVEL 0.35f

/* Standard HSV -> RGB, full saturation. */
static tiles_standby_color_t hsv_to_rgb(float h_deg, float v) {
    float h = fmodf(h_deg, 360.0f);
    if (h < 0.0f) {
        h += 360.0f;
    }
    float c = v;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float r1, g1, b1;
    if (h < 60.0f) {
        r1 = c;
        g1 = x;
        b1 = 0.0f;
    } else if (h < 120.0f) {
        r1 = x;
        g1 = c;
        b1 = 0.0f;
    } else if (h < 180.0f) {
        r1 = 0.0f;
        g1 = c;
        b1 = x;
    } else if (h < 240.0f) {
        r1 = 0.0f;
        g1 = x;
        b1 = c;
    } else if (h < 300.0f) {
        r1 = x;
        g1 = 0.0f;
        b1 = c;
    } else {
        r1 = c;
        g1 = 0.0f;
        b1 = x;
    }
    tiles_standby_color_t out = {r1, g1, b1};
    return out;
}

static tiles_standby_color_t anim_rgb_showcase(uint8_t row, uint8_t col, uint32_t now_ms) {
    if (row == 0u) {
        return white(RGB_SHOWCASE_BUTTON_ROW_LEVEL);
    }

    float diag = (float)row + (float)col;
    float phase = diag / RGB_WAVE_LENGTH_DIAG - (float)now_ms / RGB_WAVE_PERIOD_MS;
    float raw = 0.5f + 0.5f * sinf(2.0f * TILES_STANDBY_PI * phase);
    float value = powf(clamp01(raw), RGB_CONTRAST_GAMMA);

    float hue_phase = 0.5f + 0.5f * sinf(2.0f * TILES_STANDBY_PI *
                                          ((float)now_ms / RGB_HUE_CYCLE_MS + diag * 0.04f));
    float hue = RGB_HUE_MIN_DEG + (RGB_HUE_MAX_DEG - RGB_HUE_MIN_DEG) * hue_phase;

    return hsv_to_rgb(hue, value);
}

/* ---- Animation: graphic equalizer -------------------------------------
 * Each column is a VU bar lit bottom-up: rows 3-4 white, row 2 yellow,
 * row 1 red (red only ever appears as row 1's own color). Tempo-locked to
 * 90 BPM (EQ_BEAT_MS), each column pair hitting at its own rate, low to
 * high left to right. Each hit swells up (short attack), holds briefly at
 * its peak, then decays; some hits are skipped for breathing room. About
 * 6% of hits redline (row 1 lights). Buttons are off; the underglow is a
 * steady white accent.
 *
 * Tuned down over several rounds from "too flashy/fast"; a falling red
 * peak-hold marker was removed because it read as dropping red lights. */

#define EQ_NUM_COLS 6u
#define EQ_LIT_LEVEL 0.85f
#define EQ_UNDERGLOW_LEVEL 0.7f
/* One quarter note at 90 BPM, the pulse every column subdivides. */
#define EQ_BEAT_MS 667.0f
/* Fraction of hits dropped to 0: some empty space, never a long silence. */
#define EQ_MISS_FRACTION 0.12f
/* Share of a hit spent rising to full: a meter swings up fast, not
 * instantly. */
#define EQ_ATTACK_FRACTION 0.18f
/* Share of a hit held at the peak after the attack, so a ~40 ms frame
 * reliably samples the peak (see eq_bar_level()). */
#define EQ_PEAK_HOLD_FRACTION 0.10f
/* Share of hits (by velocity_key) that reach full velocity 1.0, so row 1
 * lights now and then. Deliberately rare. */
#define EQ_REDLINE_FRACTION 0.06f

/* Hits per beat per column; pairs share a rate so the bars read low to
 * high like an EQ's frequency axis. */
static const float s_eq_col_hits_per_beat[EQ_NUM_COLS] = {1.0f, 1.0f, 1.5f, 1.5f, 2.0f, 2.0f};
/* Decay shape per column: slower (bass) columns ring longer, faster
 * (treble) ones snap back. */
static const float s_eq_col_decay_exp[EQ_NUM_COLS] = {1.5f, 1.5f, 1.0f, 1.0f, 0.6f, 0.6f};

/* Deterministic in (col, time): attack, peak hold, decay, locked to the
 * column's subdivision, with occasional deterministic misses. No state. */
static float eq_bar_level(uint8_t col, uint32_t now_ms) {
    uint8_t i = (uint8_t)(col - TILES_GRID_MIN_COL);
    float hit_period = EQ_BEAT_MS / s_eq_col_hits_per_beat[i];
    float hit_index = floorf((float)now_ms / hit_period);
    float phase = ((float)now_ms - hit_index * hit_period) / hit_period; /* 0 at the hit, -> 1 before the next */

    /* Golden-ratio stepping keyed on the hit index: cheap deterministic
     * "randomness" that spreads misses evenly. */
    float miss_key = fmodf(hit_index * 0.6180339887f + (float)i * 0.37f, 1.0f);
    if (miss_key < EQ_MISS_FRACTION) {
        return 0.0f;
    }

    /* Without a plateau the envelope touches 1.0 for only an instant, which a
     * ~40 ms frame almost never samples, so redlines never showed. The brief
     * hold (EQ_PEAK_HOLD_FRACTION) is wide enough to be caught every hit. */
    float envelope;
    if (phase < EQ_ATTACK_FRACTION) {
        envelope = phase / EQ_ATTACK_FRACTION; /* linear rise to the peak */
    } else if (phase < EQ_ATTACK_FRACTION + EQ_PEAK_HOLD_FRACTION) {
        envelope = 1.0f; /* brief plateau at the peak (see above) */
    } else {
        float decay_phase =
            (phase - EQ_ATTACK_FRACTION - EQ_PEAK_HOLD_FRACTION) / (1.0f - EQ_ATTACK_FRACTION - EQ_PEAK_HOLD_FRACTION);
        envelope = powf(1.0f - decay_phase, s_eq_col_decay_exp[i]);
    }

    /* Per-hit velocity (same trick, different offset) so hits vary in height.
     * The top EQ_REDLINE_FRACTION jump to exactly 1.0 so row 1 lights. */
    float velocity_key = fmodf(hit_index * 0.6180339887f + (float)i * 0.37f + 0.5f, 1.0f);
    float velocity;
    if (velocity_key >= (1.0f - EQ_REDLINE_FRACTION)) {
        velocity = 1.0f;
    } else {
        velocity = 0.55f + 0.40f * (velocity_key / (1.0f - EQ_REDLINE_FRACTION));
    }
    return clamp01(envelope * velocity);
}

static tiles_standby_color_t eq_row_color(uint8_t row) {
    if (row == 1u) {
        tiles_standby_color_t red = {1.0f, 0.0f, 0.0f};
        return red;
    }
    if (row == 2u) {
        tiles_standby_color_t yellow = {1.0f, 1.0f, 0.0f};
        return yellow;
    }
    /* Rows 3-4: white. */
    tiles_standby_color_t white_bar = {1.0f, 1.0f, 1.0f};
    return white_bar;
}

static tiles_standby_color_t anim_equalizer(uint8_t row, uint8_t col, uint32_t now_ms) {
    if (row == 0u) {
        return white(0.0f); /* buttons off for this animation */
    }

    float level = eq_bar_level(col, now_ms);
    float row_threshold = (float)(5u - row) / 4.0f; /* row 4 = 0.25 ... row 1 = 1.0 */
    if (level >= row_threshold) {
        tiles_standby_color_t c = eq_row_color(row);
        tiles_standby_color_t scaled = {c.r * EQ_LIT_LEVEL, c.g * EQ_LIT_LEVEL, c.b * EQ_LIT_LEVEL};
        return scaled;
    }
    return white(0.0f);
}

static tiles_standby_color_t eq_underglow(uint8_t pixel_index, uint32_t now_ms) {
    (void)pixel_index;
    (void)now_ms;
    /* Steady white accent. */
    return white(EQ_UNDERGLOW_LEVEL);
}

/* ---- Animation: circular underglow wave -------------------------------
 * Only the underglow moves: a wave travels around the 4 pixels in their
 * physical circular order (g_tiles_underglow_circular_position). Pads and
 * buttons sit at a flat, dim level. */

#define CIRCLE_PERIOD_MS 2400.0f
#define CIRCLE_MIN_LEVEL 0.05f
#define CIRCLE_MAX_LEVEL 1.0f
#define CIRCLE_CONTRAST_GAMMA 1.5f
#define CIRCLE_AMBIENT_LEVEL 0.12f

static tiles_standby_color_t anim_underglow_circle(uint8_t row, uint8_t col, uint32_t now_ms) {
    (void)row;
    (void)col;
    (void)now_ms;
    return white(CIRCLE_AMBIENT_LEVEL);
}

static tiles_standby_color_t circle_underglow(uint8_t pixel_index, uint32_t now_ms) {
    float position = (float)g_tiles_underglow_circular_position[pixel_index];
    float phase = (float)now_ms / CIRCLE_PERIOD_MS - position / (float)TILES_NUM_UNDERGLOW_ANCHORS;
    float raw = 0.5f + 0.5f * sinf(2.0f * TILES_STANDBY_PI * phase);
    float v = CIRCLE_MIN_LEVEL + (CIRCLE_MAX_LEVEL - CIRCLE_MIN_LEVEL) * powf(clamp01(raw), CIRCLE_CONTRAST_GAMMA);
    return white(v);
}

/* ---- Animation: tile breaker ------------------------------------------
 * The button row is the block wall; a 3-pad paddle on the bottom row
 * follows the ball (at most one column per step). When every block is
 * broken or the ball gets past, the underglow flashes red/purple for a few
 * seconds and a new round starts. Ball and paddle differ in color so they
 * stay distinct when they overlap. See tb_step() for the reachability fix. */

#define TB_NUM_COLS 6u
#define TB_PADDLE_ROW 4u
#define TB_STEP_MS 350u
#define TB_FLASH_DURATION_MS 2200u
#define TB_FLASH_TOGGLE_MS 260u
#define TB_BLOCK_LEVEL 0.85f
#define TB_PADDLE_LEVEL 0.9f
#define TB_BALL_LEVEL 1.0f

typedef enum {
    TB_PHASE_PLAYING = 0,
    TB_PHASE_ROUND_END,
} tb_phase_t;

static bool s_tb_block_alive[TB_NUM_COLS];
static int8_t s_tb_ball_row;
static int8_t s_tb_ball_col;
static int8_t s_tb_ball_drow;
static int8_t s_tb_ball_dcol;
static int8_t s_tb_paddle_center; /* 2-5; the paddle covers center-1..center+1 */
static tb_phase_t s_tb_phase;
static uint32_t s_tb_last_step_ms;
static uint32_t s_tb_round_end_ms;
static bool s_tb_inited;

static void tb_new_round(uint32_t now_ms) {
    for (uint8_t i = 0; i < TB_NUM_COLS; i++) {
        s_tb_block_alive[i] = true;
    }
    s_tb_paddle_center = 3;
    s_tb_ball_row = (int8_t)(TB_PADDLE_ROW - 1u);
    s_tb_ball_col = s_tb_paddle_center;
    s_tb_ball_drow = -1; /* heads toward the blocks first */
    s_tb_ball_dcol = ((rand() % 2) == 0) ? -1 : 1;
    s_tb_phase = TB_PHASE_PLAYING;
    s_tb_last_step_ms = now_ms;
}

/* Moving row and column by exactly 1 each step keeps (row + col) mod 2
 * fixed for the whole flight, so from the fixed start (3, 3) half the
 * blocks were unreachable. (Throttling the column every other step only
 * changed which half.) Fix: each bounce off the top wall or paddle gets a
 * coin-flip chance to reverse the column direction, so the column does a
 * random walk over all 6. game_mode.c uses the same fix. */
static void tb_step(uint32_t now_ms) {
    int8_t new_col = (int8_t)(s_tb_ball_col + s_tb_ball_dcol);
    if (new_col < (int8_t)TILES_GRID_MIN_COL || new_col > (int8_t)TILES_GRID_MAX_COL) {
        s_tb_ball_dcol = (int8_t)(-s_tb_ball_dcol);
        new_col = (int8_t)(s_tb_ball_col + s_tb_ball_dcol);
    }
    int8_t new_row = (int8_t)(s_tb_ball_row + s_tb_ball_drow);

    if (new_row < 1) {
        /* Hit the block wall (row 0): always bounce, block or not. */
        uint8_t col_index = (uint8_t)(new_col - TILES_GRID_MIN_COL);
        s_tb_block_alive[col_index] = false;
        s_tb_ball_drow = 1;
        new_row = 1;
        if ((rand() % 2) == 0) {
            s_tb_ball_dcol = (int8_t)(-s_tb_ball_dcol);
        }

        bool all_dead = true;
        for (uint8_t i = 0; i < TB_NUM_COLS; i++) {
            if (s_tb_block_alive[i]) {
                all_dead = false;
                break;
            }
        }
        if (all_dead) {
            s_tb_phase = TB_PHASE_ROUND_END;
            s_tb_round_end_ms = now_ms;
        }
    } else if (new_row > (int8_t)TB_PADDLE_ROW) {
        int8_t paddle_min = (int8_t)(s_tb_paddle_center - 1);
        int8_t paddle_max = (int8_t)(s_tb_paddle_center + 1);
        if (new_col >= paddle_min && new_col <= paddle_max) {
            s_tb_ball_drow = -1;
            new_row = (int8_t)TB_PADDLE_ROW;
            if ((rand() % 2) == 0) {
                s_tb_ball_dcol = (int8_t)(-s_tb_ball_dcol);
            }
        } else {
            /* Missed: lost. The ball sits just below the paddle row, outside the
             * drawn rows, so it simply disappears. */
            s_tb_phase = TB_PHASE_ROUND_END;
            s_tb_round_end_ms = now_ms;
        }
    }

    s_tb_ball_col = new_col;
    s_tb_ball_row = new_row;

    if (s_tb_phase == TB_PHASE_PLAYING) {
        if (s_tb_paddle_center < s_tb_ball_col) {
            s_tb_paddle_center++;
        } else if (s_tb_paddle_center > s_tb_ball_col) {
            s_tb_paddle_center--;
        }
        if (s_tb_paddle_center < 2) {
            s_tb_paddle_center = 2;
        }
        if (s_tb_paddle_center > 5) {
            s_tb_paddle_center = 5;
        }
    }
}

static void tb_update(uint32_t now_ms) {
    if (!s_tb_inited) {
        tb_new_round(now_ms);
        s_tb_inited = true;
        return;
    }
    if (s_tb_phase == TB_PHASE_PLAYING) {
        if (now_ms - s_tb_last_step_ms >= TB_STEP_MS) {
            tb_step(now_ms);
            s_tb_last_step_ms = now_ms;
        }
    } else if (now_ms - s_tb_round_end_ms >= TB_FLASH_DURATION_MS) {
        tb_new_round(now_ms);
    }
}

static tiles_standby_color_t anim_tile_breaker(uint8_t row, uint8_t col, uint32_t now_ms) {
    tb_update(now_ms);

    if (row == 0u) {
        uint8_t idx = (uint8_t)(col - TILES_GRID_MIN_COL);
        if (s_tb_block_alive[idx]) {
            /* Orange blocks. */
            tiles_standby_color_t c = {1.0f * TB_BLOCK_LEVEL, 0.4f * TB_BLOCK_LEVEL, 0.0f};
            return c;
        }
        return white(0.0f);
    }

    if (s_tb_ball_row >= 1 && s_tb_ball_row <= (int8_t)TB_PADDLE_ROW && (int8_t)row == s_tb_ball_row &&
        (int8_t)col == s_tb_ball_col) {
        /* Warm white ball, drawn before the paddle so it's on top when they
         * share a cell. */
        tiles_standby_color_t c = {1.0f * TB_BALL_LEVEL, 1.0f * TB_BALL_LEVEL, 0.4f * TB_BALL_LEVEL};
        return c;
    }

    if (row == TB_PADDLE_ROW) {
        int8_t paddle_min = (int8_t)(s_tb_paddle_center - 1);
        int8_t paddle_max = (int8_t)(s_tb_paddle_center + 1);
        if ((int8_t)col >= paddle_min && (int8_t)col <= paddle_max) {
            /* Cyan paddle. */
            tiles_standby_color_t c = {0.0f, 0.6f * TB_PADDLE_LEVEL, 1.0f * TB_PADDLE_LEVEL};
            return c;
        }
    }

    return white(0.0f);
}

static tiles_standby_color_t tb_underglow(uint8_t pixel_index, uint32_t now_ms) {
    (void)pixel_index;
    if (s_tb_phase != TB_PHASE_ROUND_END) {
        return white(0.0f);
    }
    uint32_t toggle = (now_ms - s_tb_round_end_ms) / TB_FLASH_TOGGLE_MS;
    if ((toggle % 2u) == 0u) {
        tiles_standby_color_t red = {1.0f, 0.0f, 0.0f};
        return red;
    }
    tiles_standby_color_t purple = {0.6f, 0.0f, 1.0f};
    return purple;
}

/* ---- Animation: marquee -----------------------------------------------
 * "TILES - " scrolls across the pads in the shared 4-row font
 * (services/pixel_font.h). Underglow and buttons off. Glyphs are separated
 * by MARQUEE_GLYPH_GAP (2) blank columns and the text wraps seamlessly.
 * Slow scroll and wide gaps are what make it readable at 4 rows. */

#define MARQUEE_NUM_GLYPHS ((uint8_t)(sizeof(s_marquee_message) / sizeof(s_marquee_message[0])))
#define MARQUEE_GLYPH_GAP 2u
#define MARQUEE_MS_PER_COLUMN 600u
#define MARQUEE_LIT_LEVEL 0.9f

/* Pointers: an extern object's value isn't a compile-time constant here,
 * so a static initializer can't copy the struct. */
static const tiles_glyph_t *const s_marquee_message[] = {
    &TILES_GLYPH_T, &TILES_GLYPH_I, &TILES_GLYPH_L, &TILES_GLYPH_E, &TILES_GLYPH_S,
    &TILES_GLYPH_SPACE, &TILES_GLYPH_DASH, &TILES_GLYPH_SPACE,
};

static uint16_t marquee_total_width(void) {
    static uint16_t s_total_width;
    static bool s_computed;
    if (!s_computed) {
        uint16_t total = 0;
        for (uint8_t i = 0; i < MARQUEE_NUM_GLYPHS; i++) {
            total = (uint16_t)(total + s_marquee_message[i]->width + MARQUEE_GLYPH_GAP);
        }
        s_total_width = total;
        s_computed = true;
    }
    return s_total_width;
}

static uint8_t marquee_column_bits(uint16_t virtual_col) {
    uint16_t remaining = virtual_col;
    for (uint8_t i = 0; i < MARQUEE_NUM_GLYPHS; i++) {
        uint16_t glyph_span = (uint16_t)(s_marquee_message[i]->width + MARQUEE_GLYPH_GAP);
        if (remaining < s_marquee_message[i]->width) {
            return s_marquee_message[i]->cols[remaining];
        }
        if (remaining < glyph_span) {
            return 0u; /* the spacing columns */
        }
        remaining = (uint16_t)(remaining - glyph_span);
    }
    return 0u;
}

static tiles_standby_color_t anim_marquee(uint8_t row, uint8_t col, uint32_t now_ms) {
    if (row == 0u) {
        return white(0.0f);
    }

    uint16_t total_width = marquee_total_width();
    if (total_width == 0u) {
        return white(0.0f);
    }

    uint32_t scroll_col = (now_ms / MARQUEE_MS_PER_COLUMN) % total_width;
    uint16_t virtual_col = (uint16_t)((scroll_col + (col - TILES_GRID_MIN_COL)) % total_width);
    uint8_t bits = marquee_column_bits(virtual_col);

    uint8_t row_bit = (uint8_t)(1u << (row - 1u));
    if ((bits & row_bit) != 0u) {
        return white(MARQUEE_LIT_LEVEL);
    }
    return white(0.0f);
}

static tiles_standby_color_t marquee_underglow(uint8_t pixel_index, uint32_t now_ms) {
    (void)pixel_index;
    (void)now_ms;
    return white(0.0f);
}

/* ---- Animation: bouncing glow ------------------------------------------
 * One soft white point bouncing around the pad grid. Position is a
 * closed-form triangle wave per axis (no state); the two axes have periods
 * in a non-integer ratio, so the path traces a slow Lissajous figure.
 * Buttons off; underglow samples the field, so it glows as the point
 * passes. */

#define BOUNCE_ROW_PERIOD_MS 5200.0f /* one full min->max->min row traversal */
#define BOUNCE_COL_PERIOD_MS 6700.0f /* not a small-integer ratio of the row period */
#define BOUNCE_RADIUS_CELLS 1.5f
#define BOUNCE_PEAK_LEVEL 0.9f

/* Triangle wave 0->1->0 over one period, scaled into [min_val, max_val]: a
 * wall bounce with no state. */
static float bounce_axis_position(uint32_t now_ms, float period_ms, float min_val, float max_val) {
    float phase = fmodf((float)now_ms, period_ms) / period_ms;
    float tri = 1.0f - fabsf(2.0f * phase - 1.0f);
    return min_val + (max_val - min_val) * tri;
}

static tiles_standby_color_t anim_bounce(uint8_t row, uint8_t col, uint32_t now_ms) {
    if (row == 0u) {
        return white(0.0f);
    }

    float bounce_row = bounce_axis_position(now_ms, BOUNCE_ROW_PERIOD_MS, 1.0f, (float)TILES_GRID_MAX_ROW);
    float bounce_col =
        bounce_axis_position(now_ms, BOUNCE_COL_PERIOD_MS, (float)TILES_GRID_MIN_COL, (float)TILES_GRID_MAX_COL);

    float dr = (float)row - bounce_row;
    float dc = (float)col - bounce_col;
    float dist = sqrtf(dr * dr + dc * dc);

    float t = clamp01(1.0f - dist / BOUNCE_RADIUS_CELLS);
    return white(BOUNCE_PEAK_LEVEL * t * t);
}

/* ---- Animation: Tetris --------------------------------------------------
 * Self-playing counterpart of game_mode.c's Tetris: the same small piece
 * set (dot, domino, straight and corner tromino, 2x2 square), separate
 * code and state. At spawn a greedy AI tries every rotation and column,
 * simulates the drop, and picks the one landing deepest (a cheap "keep
 * the stack low" proxy); the piece then falls one row at a time. Topping
 * out flashes red, then the well clears. Buttons off. */

#define TETRIS_MIN_ROW 1u /* row 0 is buttons, not part of the well */
#define TETRIS_MAX_ROW TILES_GRID_MAX_ROW
#define TETRIS_MIN_COL TILES_GRID_MIN_COL
#define TETRIS_MAX_COL TILES_GRID_MAX_COL
#define TETRIS_ROWS 4u
#define TETRIS_COLS 6u
#define TETRIS_STEP_MS 260u /* faster than the game version: nobody is playing */
#define TETRIS_LOCKED_LEVEL 0.8f
#define TETRIS_FLASH_DURATION_MS 2200u
#define TETRIS_FLASH_TOGGLE_MS 260u
/* White underglow strobe on a line clear: fast and short. */
#define TETRIS_LINE_CLEAR_FLASH_MS 450u
#define TETRIS_LINE_CLEAR_TOGGLE_MS 90u
#define TETRIS_NUM_PIECE_TYPES 5u
#define TETRIS_MAX_CELLS 4u

typedef struct {
    int8_t dr;
    int8_t dc;
} tetris_offset_t;

/* Two rotation states per piece; only the first num_cells entries are
 * used. */
typedef struct {
    uint8_t num_cells;
    tetris_offset_t state0[TETRIS_MAX_CELLS];
    tetris_offset_t state1[TETRIS_MAX_CELLS];
    float r, g, b;
} tetris_piece_def_t;

/* Same pieces as game_mode.c's GT_PIECES, duplicated on purpose. */
static const tetris_piece_def_t TETRIS_PIECES[TETRIS_NUM_PIECE_TYPES] = {
    /* Dot: 1 cell, rotation is a no-op. */
    {1u, {{0, 0}}, {{0, 0}}, 1.0f, 1.0f, 1.0f},
    /* Domino: horizontal/vertical. */
    {2u, {{0, 0}, {0, 1}}, {{0, 0}, {1, 0}}, 0.0f, 1.0f, 1.0f},
    /* Straight tromino: horizontal/vertical. */
    {3u, {{0, 0}, {0, 1}, {0, 2}}, {{0, 0}, {1, 0}, {2, 0}}, 0.0f, 1.0f, 0.0f},
    /* Corner tromino: two different bends for variety (not a strict rotation). */
    {3u, {{0, 0}, {1, 0}, {1, 1}}, {{0, 0}, {0, 1}, {1, 0}}, 1.0f, 0.5f, 0.0f},
    /* Square: 2x2, rotation is a no-op. */
    {4u, {{0, 0}, {0, 1}, {1, 0}, {1, 1}}, {{0, 0}, {0, 1}, {1, 0}, {1, 1}}, 1.0f, 1.0f, 0.0f},
};

typedef enum {
    TETRIS_PHASE_PLAYING = 0,
    TETRIS_PHASE_ROUND_END,
} tetris_phase_t;

/* 0 = empty, else piece type + 1; [row - TETRIS_MIN_ROW][col - TETRIS_MIN_COL]. */
static uint8_t s_tetris_board[TETRIS_ROWS][TETRIS_COLS];
static uint8_t s_tetris_piece_type;
static uint8_t s_tetris_rotation;
static int8_t s_tetris_origin_row;
static int8_t s_tetris_origin_col;
static int8_t s_tetris_target_row; /* the AI's landing row for the current piece */
static tetris_phase_t s_tetris_phase;
static uint32_t s_tetris_last_step_ms;
static uint32_t s_tetris_round_end_ms;
static uint32_t s_tetris_line_clear_flash_ms;
static bool s_tetris_inited;

static const tetris_offset_t *tetris_offsets(uint8_t piece_type, uint8_t rotation) {
    return (rotation == 0u) ? TETRIS_PIECES[piece_type].state0 : TETRIS_PIECES[piece_type].state1;
}

static bool tetris_fits(uint8_t piece_type, uint8_t rotation, int8_t origin_row, int8_t origin_col) {
    const tetris_offset_t *offsets = tetris_offsets(piece_type, rotation);
    uint8_t num_cells = TETRIS_PIECES[piece_type].num_cells;
    for (uint8_t i = 0; i < num_cells; i++) {
        int8_t r = (int8_t)(origin_row + offsets[i].dr);
        int8_t c = (int8_t)(origin_col + offsets[i].dc);
        if (r < (int8_t)TETRIS_MIN_ROW || r > (int8_t)TETRIS_MAX_ROW) {
            return false;
        }
        if (c < (int8_t)TETRIS_MIN_COL || c > (int8_t)TETRIS_MAX_COL) {
            return false;
        }
        if (s_tetris_board[r - (int8_t)TETRIS_MIN_ROW][c - (int8_t)TETRIS_MIN_COL] != 0u) {
            return false;
        }
    }
    return true;
}

/* Drops (piece, rotation, column) from the top and returns the landing
 * row, or false if it doesn't fit even at spawn height. */
static bool tetris_simulate_drop(uint8_t piece_type, uint8_t rotation, int8_t origin_col, int8_t *out_row) {
    if (!tetris_fits(piece_type, rotation, (int8_t)TETRIS_MIN_ROW, origin_col)) {
        return false;
    }
    int8_t row = (int8_t)TETRIS_MIN_ROW;
    while (tetris_fits(piece_type, rotation, (int8_t)(row + 1), origin_col)) {
        row++;
    }
    *out_row = row;
    return true;
}

/* Greedy placement (see the animation comment). */
static void tetris_ai_place(uint8_t piece_type, uint8_t *out_rotation, int8_t *out_col, int8_t *out_row) {
    bool found = false;
    int8_t best_score = -1;
    uint8_t best_rotation = 0u;
    int8_t best_col = (int8_t)TETRIS_MIN_COL;
    int8_t best_row = (int8_t)TETRIS_MIN_ROW;

    for (uint8_t rotation = 0u; rotation < 2u; rotation++) {
        for (int8_t col = (int8_t)TETRIS_MIN_COL; col <= (int8_t)TETRIS_MAX_COL; col++) {
            int8_t landing_row;
            if (!tetris_simulate_drop(piece_type, rotation, col, &landing_row)) {
                continue;
            }
            const tetris_offset_t *offsets = tetris_offsets(piece_type, rotation);
            uint8_t num_cells = TETRIS_PIECES[piece_type].num_cells;
            int8_t min_row = (int8_t)(landing_row + offsets[0].dr);
            for (uint8_t i = 1; i < num_cells; i++) {
                int8_t r = (int8_t)(landing_row + offsets[i].dr);
                if (r < min_row) {
                    min_row = r;
                }
            }
            if (!found || min_row > best_score) {
                found = true;
                best_score = min_row;
                best_rotation = rotation;
                best_col = col;
                best_row = landing_row;
            }
        }
    }

    *out_rotation = best_rotation;
    *out_col = best_col;
    *out_row = found ? best_row : (int8_t)TETRIS_MIN_ROW;
}

static void tetris_spawn(void) {
    s_tetris_piece_type = (uint8_t)(rand() % TETRIS_NUM_PIECE_TYPES);
    s_tetris_origin_row = (int8_t)TETRIS_MIN_ROW;
    tetris_ai_place(s_tetris_piece_type, &s_tetris_rotation, &s_tetris_origin_col, &s_tetris_target_row);
}

/* Bottom-up, rechecking the same row after a shift (like game_mode.c
 * gt_clear_lines()). Returns rows cleared. */
static uint8_t tetris_clear_lines(void) {
    uint8_t cleared = 0u;
    int8_t row = (int8_t)(TETRIS_ROWS - 1u);
    while (row >= 0) {
        bool full = true;
        for (uint8_t c = 0; c < TETRIS_COLS; c++) {
            if (s_tetris_board[row][c] == 0u) {
                full = false;
                break;
            }
        }
        if (!full) {
            row--;
            continue;
        }
        cleared++;
        for (int8_t r = row; r > 0; r--) {
            for (uint8_t c = 0; c < TETRIS_COLS; c++) {
                s_tetris_board[r][c] = s_tetris_board[r - 1][c];
            }
        }
        for (uint8_t c = 0; c < TETRIS_COLS; c++) {
            s_tetris_board[0][c] = 0u;
        }
    }
    return cleared;
}

static void tetris_new_round(uint32_t now_ms) {
    for (uint8_t r = 0; r < TETRIS_ROWS; r++) {
        for (uint8_t c = 0; c < TETRIS_COLS; c++) {
            s_tetris_board[r][c] = 0u;
        }
    }
    s_tetris_phase = TETRIS_PHASE_PLAYING;
    tetris_spawn();
    s_tetris_last_step_ms = now_ms;
    /* "Long past", not 0, so a round starting right after boot doesn't flash. */
    s_tetris_line_clear_flash_ms = now_ms - TETRIS_LINE_CLEAR_FLASH_MS - 1u;
}

static void tetris_lock(uint32_t now_ms) {
    const tetris_offset_t *offsets = tetris_offsets(s_tetris_piece_type, s_tetris_rotation);
    uint8_t num_cells = TETRIS_PIECES[s_tetris_piece_type].num_cells;
    for (uint8_t i = 0; i < num_cells; i++) {
        int8_t r = (int8_t)(s_tetris_origin_row + offsets[i].dr);
        int8_t c = (int8_t)(s_tetris_origin_col + offsets[i].dc);
        s_tetris_board[r - (int8_t)TETRIS_MIN_ROW][c - (int8_t)TETRIS_MIN_COL] = (uint8_t)(s_tetris_piece_type + 1u);
    }
    if (tetris_clear_lines() > 0u) {
        s_tetris_line_clear_flash_ms = now_ms;
    }
    tetris_spawn();
    if (!tetris_fits(s_tetris_piece_type, s_tetris_rotation, s_tetris_origin_row, s_tetris_origin_col)) {
        /* Topped out. */
        s_tetris_phase = TETRIS_PHASE_ROUND_END;
        s_tetris_round_end_ms = now_ms;
    }
}

static void tetris_update(uint32_t now_ms) {
    if (!s_tetris_inited) {
        tetris_new_round(now_ms);
        s_tetris_inited = true;
        return;
    }
    if (s_tetris_phase == TETRIS_PHASE_ROUND_END) {
        if (now_ms - s_tetris_round_end_ms >= TETRIS_FLASH_DURATION_MS) {
            tetris_new_round(now_ms);
        }
        return;
    }
    if (now_ms - s_tetris_last_step_ms < TETRIS_STEP_MS) {
        return;
    }
    s_tetris_last_step_ms = now_ms;
    if (s_tetris_origin_row < s_tetris_target_row) {
        s_tetris_origin_row++;
    } else {
        tetris_lock(now_ms);
    }
}

static tiles_standby_color_t anim_tetris(uint8_t row, uint8_t col, uint32_t now_ms) {
    tetris_update(now_ms);

    if (row == 0u) {
        return white(0.0f);
    }

    if (s_tetris_phase == TETRIS_PHASE_PLAYING) {
        const tetris_offset_t *offsets = tetris_offsets(s_tetris_piece_type, s_tetris_rotation);
        uint8_t num_cells = TETRIS_PIECES[s_tetris_piece_type].num_cells;
        for (uint8_t i = 0; i < num_cells; i++) {
            int8_t r = (int8_t)(s_tetris_origin_row + offsets[i].dr);
            int8_t c = (int8_t)(s_tetris_origin_col + offsets[i].dc);
            if (r == (int8_t)row && c == (int8_t)col) {
                /* The falling piece draws first, at full brightness. */
                const tetris_piece_def_t *active = &TETRIS_PIECES[s_tetris_piece_type];
                tiles_standby_color_t c_active = {active->r, active->g, active->b};
                return c_active;
            }
        }
    }

    uint8_t v = s_tetris_board[row - (uint8_t)TETRIS_MIN_ROW][col - (uint8_t)TETRIS_MIN_COL];
    if (v != 0u) {
        const tetris_piece_def_t *def = &TETRIS_PIECES[v - 1u];
        tiles_standby_color_t c_locked = {def->r * TETRIS_LOCKED_LEVEL, def->g * TETRIS_LOCKED_LEVEL,
                                           def->b * TETRIS_LOCKED_LEVEL};
        return c_locked;
    }
    return white(0.0f);
}

/* Topping out blinks plain red; a line clear is a short white strobe. */
static tiles_standby_color_t tetris_underglow(uint8_t pixel_index, uint32_t now_ms) {
    (void)pixel_index;
    if (s_tetris_phase == TETRIS_PHASE_ROUND_END) {
        uint32_t toggle = (now_ms - s_tetris_round_end_ms) / TETRIS_FLASH_TOGGLE_MS;
        if ((toggle % 2u) == 0u) {
            tiles_standby_color_t red = {1.0f, 0.0f, 0.0f};
            return red;
        }
        return white(0.0f);
    }
    if (now_ms - s_tetris_line_clear_flash_ms < TETRIS_LINE_CLEAR_FLASH_MS) {
        uint32_t toggle = (now_ms - s_tetris_line_clear_flash_ms) / TETRIS_LINE_CLEAR_TOGGLE_MS;
        if ((toggle % 2u) == 0u) {
            return white(1.0f);
        }
    }
    return white(0.0f);
}

/* ---- Animation: Paddle ----------------------------------------------------
 * Self-playing counterpart of game_mode.c's Paddle: same layout and colors,
 * separate code. Only the paddle the ball is heading toward tracks it
 * (pd_ai_track()); the other drifts back to rest (pd_ai_recenter()),
 * since mirrored paddles looked fake. A rare miss flashes the underglow
 * white and re-serves. Buttons off. */

#define PD_MIN_ROW 1u /* row 0 is buttons, not part of the court */
#define PD_MAX_ROW TILES_GRID_MAX_ROW
#define PD_PADDLE_COL_LEFT TILES_GRID_MIN_COL
#define PD_PADDLE_COL_RIGHT TILES_GRID_MAX_COL
#define PD_PADDLE_TOP_MIN PD_MIN_ROW /* paddle covers [top, top+1] */
#define PD_PADDLE_TOP_MAX (TILES_GRID_MAX_ROW - 1u)
#define PD_STEP_MS 220u /* faster than the game version: nobody is playing */
#define PD_POINT_FLASH_MS 500u
#define PD_POINT_FLASH_TOGGLE_MS 110u
#define PD_PADDLE_LEVEL 1.0f
#define PD_BALL_LEVEL 1.0f

static int8_t s_pd_left_paddle_top;
static int8_t s_pd_right_paddle_top;
static int8_t s_pd_ball_row;
static int8_t s_pd_ball_col;
static int8_t s_pd_ball_drow;
static int8_t s_pd_ball_dcol;
static uint32_t s_pd_last_step_ms;
static uint32_t s_pd_point_flash_ms;
static bool s_pd_inited;

static void pd_serve(uint32_t now_ms) {
    s_pd_ball_row = (int8_t)(PD_MIN_ROW + (rand() % (PD_MAX_ROW - PD_MIN_ROW + 1u)));
    s_pd_ball_col = ((rand() % 2) == 0) ? 3 : 4; /* one of the two middle columns */
    s_pd_ball_drow = ((rand() % 2) == 0) ? -1 : 1;
    s_pd_ball_dcol = ((rand() % 2) == 0) ? -1 : 1;
    s_pd_last_step_ms = now_ms;
}

static void pd_new_round(uint32_t now_ms) {
    s_pd_left_paddle_top = 2;
    s_pd_right_paddle_top = 2;
    pd_serve(now_ms);
    /* "Long past", as for Tetris's flash. */
    s_pd_point_flash_ms = now_ms - PD_POINT_FLASH_MS - 1u;
}

/* Moves the paddle at most one row toward the ball (the tile breaker
 * paddle AI). */
static void pd_ai_track(int8_t *paddle_top) {
    if (s_pd_ball_row < *paddle_top) {
        (*paddle_top)--;
    } else if (s_pd_ball_row > (int8_t)(*paddle_top + 1)) {
        (*paddle_top)++;
    }
    if (*paddle_top < (int8_t)PD_PADDLE_TOP_MIN) {
        *paddle_top = (int8_t)PD_PADDLE_TOP_MIN;
    }
    if (*paddle_top > (int8_t)PD_PADDLE_TOP_MAX) {
        *paddle_top = (int8_t)PD_PADDLE_TOP_MAX;
    }
}

/* Drifts the paddle one row back toward its rest (spawn) position. */
#define PD_REST_TOP 2

static void pd_ai_recenter(int8_t *paddle_top) {
    if (*paddle_top < (int8_t)PD_REST_TOP) {
        (*paddle_top)++;
    } else if (*paddle_top > (int8_t)PD_REST_TOP) {
        (*paddle_top)--;
    }
}

static void pd_step(uint32_t now_ms) {
    int8_t new_row = (int8_t)(s_pd_ball_row + s_pd_ball_drow);
    if (new_row < (int8_t)PD_MIN_ROW || new_row > (int8_t)PD_MAX_ROW) {
        s_pd_ball_drow = (int8_t)(-s_pd_ball_drow);
        new_row = (int8_t)(s_pd_ball_row + s_pd_ball_drow);
    }

    int8_t new_col = (int8_t)(s_pd_ball_col + s_pd_ball_dcol);
    if (new_col < (int8_t)PD_PADDLE_COL_LEFT) {
        if (new_row >= s_pd_left_paddle_top && new_row <= (int8_t)(s_pd_left_paddle_top + 1)) {
            s_pd_ball_dcol = 1;
            new_col = (int8_t)PD_PADDLE_COL_LEFT;
        } else {
            s_pd_point_flash_ms = now_ms;
            pd_serve(now_ms);
            return;
        }
    } else if (new_col > (int8_t)PD_PADDLE_COL_RIGHT) {
        if (new_row >= s_pd_right_paddle_top && new_row <= (int8_t)(s_pd_right_paddle_top + 1)) {
            s_pd_ball_dcol = -1;
            new_col = (int8_t)PD_PADDLE_COL_RIGHT;
        } else {
            s_pd_point_flash_ms = now_ms;
            pd_serve(now_ms);
            return;
        }
    }

    s_pd_ball_row = new_row;
    s_pd_ball_col = new_col;

    /* Only the side the ball heads toward defends; the other recenters. */
    if (s_pd_ball_dcol < 0) {
        pd_ai_track(&s_pd_left_paddle_top);
        pd_ai_recenter(&s_pd_right_paddle_top);
    } else {
        pd_ai_track(&s_pd_right_paddle_top);
        pd_ai_recenter(&s_pd_left_paddle_top);
    }
}

static void pd_update(uint32_t now_ms) {
    if (!s_pd_inited) {
        pd_new_round(now_ms);
        s_pd_inited = true;
        return;
    }
    if (now_ms - s_pd_last_step_ms < PD_STEP_MS) {
        return;
    }
    s_pd_last_step_ms = now_ms;
    pd_step(now_ms);
}

static tiles_standby_color_t anim_paddle(uint8_t row, uint8_t col, uint32_t now_ms) {
    pd_update(now_ms);

    if (row == 0u) {
        return white(0.0f);
    }

    if ((int8_t)row == s_pd_ball_row && (int8_t)col == s_pd_ball_col) {
        /* Ball before paddles, so it's on top when they share a cell. */
        tiles_standby_color_t ball = {0.0f, 0.0f, PD_BALL_LEVEL};
        return ball;
    }
    if (col == PD_PADDLE_COL_LEFT && (int8_t)row >= s_pd_left_paddle_top &&
        (int8_t)row <= (int8_t)(s_pd_left_paddle_top + 1)) {
        return white(PD_PADDLE_LEVEL);
    }
    if (col == PD_PADDLE_COL_RIGHT && (int8_t)row >= s_pd_right_paddle_top &&
        (int8_t)row <= (int8_t)(s_pd_right_paddle_top + 1)) {
        return white(PD_PADDLE_LEVEL);
    }
    return white(0.0f);
}

static tiles_standby_color_t pd_underglow(uint8_t pixel_index, uint32_t now_ms) {
    (void)pixel_index;
    if (now_ms - s_pd_point_flash_ms >= PD_POINT_FLASH_MS) {
        return white(0.0f);
    }
    bool on = (((now_ms - s_pd_point_flash_ms) / PD_POINT_FLASH_TOGGLE_MS) % 2u) == 0u;
    return white(on ? 1.0f : 0.0f);
}

/* ---- Animation: falling dots -------------------------------------------
 * White dots fall one row at a time and stay where they land, slowly
 * filling the grid; when it's full it holds a moment, clears, and starts
 * over. Every falling dot shares one step clock, so each frame can
 * cross-fade a dot between its row and the next (smoothstep), instead of
 * jumping. A dot about to land holds full brightness. Buttons off;
 * underglow samples the field. */

#define FALLINGDOTS_MIN_ROW 1u
#define FALLINGDOTS_MAX_ROW TILES_GRID_MAX_ROW
#define FALLINGDOTS_MIN_COL TILES_GRID_MIN_COL
#define FALLINGDOTS_MAX_COL TILES_GRID_MAX_COL
#define FALLINGDOTS_ROWS 4u
#define FALLINGDOTS_COLS 6u
#define FALLINGDOTS_STEP_MS 320u           /* time for a falling dot to cross one row */
#define FALLINGDOTS_SPAWN_INTERVAL_MS 900u /* spawn interval while there's room */
#define FALLINGDOTS_MAX_CONCURRENT 4u
#define FALLINGDOTS_FULL_PAUSE_MS 1800u /* how long the full grid holds before clearing */
#define FALLINGDOTS_ACTIVE_LEVEL 1.0f
#define FALLINGDOTS_LANDED_LEVEL 0.65f

typedef struct {
    bool active;
    uint8_t col;
    int8_t row;
} fallingdots_dot_t;

/* Indexed [row - FALLINGDOTS_MIN_ROW][col - FALLINGDOTS_MIN_COL]. */
static bool s_fallingdots_filled[FALLINGDOTS_ROWS][FALLINGDOTS_COLS];
static fallingdots_dot_t s_fallingdots_active[FALLINGDOTS_MAX_CONCURRENT];
static uint32_t s_fallingdots_last_step_ms;
static uint32_t s_fallingdots_last_spawn_ms;
static uint32_t s_fallingdots_full_since_ms;
static bool s_fallingdots_is_full;
static bool s_fallingdots_inited;

static void fallingdots_reset(void) {
    for (uint8_t r = 0; r < FALLINGDOTS_ROWS; r++) {
        for (uint8_t c = 0; c < FALLINGDOTS_COLS; c++) {
            s_fallingdots_filled[r][c] = false;
        }
    }
    for (uint8_t i = 0; i < FALLINGDOTS_MAX_CONCURRENT; i++) {
        s_fallingdots_active[i].active = false;
    }
    s_fallingdots_is_full = false;
}

/* Columns fill bottom-up with no gaps, so an empty top cell means the
 * column has room (and checking every column answers "is it full"). */
static bool fallingdots_col_has_room(uint8_t col0) {
    return !s_fallingdots_filled[0][col0];
}

static bool fallingdots_all_full(void) {
    for (uint8_t c = 0; c < FALLINGDOTS_COLS; c++) {
        if (!s_fallingdots_filled[0][c]) {
            return false;
        }
    }
    return true;
}

static bool fallingdots_cell_filled(uint8_t row, uint8_t col) {
    return s_fallingdots_filled[row - FALLINGDOTS_MIN_ROW][col - FALLINGDOTS_MIN_COL];
}

static void fallingdots_spawn(void) {
    int8_t slot = -1;
    for (uint8_t i = 0; i < FALLINGDOTS_MAX_CONCURRENT; i++) {
        if (!s_fallingdots_active[i].active) {
            slot = (int8_t)i;
            break;
        }
    }
    if (slot < 0) {
        return; /* at the concurrent cap */
    }

    /* A few random tries for a column with room (bounded, like
     * snake_place_food()). */
    for (uint8_t attempt = 0; attempt < 12u; attempt++) {
        uint8_t col0 = (uint8_t)(rand() % FALLINGDOTS_COLS);
        if (fallingdots_col_has_room(col0)) {
            s_fallingdots_active[slot].active = true;
            s_fallingdots_active[slot].col = (uint8_t)(FALLINGDOTS_MIN_COL + col0);
            s_fallingdots_active[slot].row = (int8_t)FALLINGDOTS_MIN_ROW;
            return;
        }
    }
    /* None found: the grid is nearly full (caught shortly) or we were unlucky;
     * skip this spawn. */
}

/* True if the dot's next row is off the grid or occupied. Shared by the
 * step (advance or land) and the renderer (whether to fade into the next
 * row). */
static bool fallingdots_next_blocked(const fallingdots_dot_t *dot) {
    int8_t next_row = (int8_t)(dot->row + 1);
    if (next_row > (int8_t)FALLINGDOTS_MAX_ROW) {
        return true;
    }
    return fallingdots_cell_filled((uint8_t)next_row, dot->col);
}

static void fallingdots_step(uint32_t now_ms) {
    for (uint8_t i = 0; i < FALLINGDOTS_MAX_CONCURRENT; i++) {
        fallingdots_dot_t *dot = &s_fallingdots_active[i];
        if (!dot->active) {
            continue;
        }
        if (fallingdots_next_blocked(dot)) {
            s_fallingdots_filled[dot->row - (int8_t)FALLINGDOTS_MIN_ROW][dot->col - FALLINGDOTS_MIN_COL] = true;
            dot->active = false;
        } else {
            dot->row++;
        }
    }

    if (fallingdots_all_full()) {
        if (!s_fallingdots_is_full) {
            s_fallingdots_is_full = true;
            s_fallingdots_full_since_ms = now_ms;
        } else if (now_ms - s_fallingdots_full_since_ms >= FALLINGDOTS_FULL_PAUSE_MS) {
            fallingdots_reset();
        }
        return;
    }

    if (now_ms - s_fallingdots_last_spawn_ms >= FALLINGDOTS_SPAWN_INTERVAL_MS) {
        s_fallingdots_last_spawn_ms = now_ms;
        fallingdots_spawn();
    }
}

static void fallingdots_update(uint32_t now_ms) {
    if (!s_fallingdots_inited) {
        fallingdots_reset();
        s_fallingdots_last_step_ms = now_ms;
        s_fallingdots_last_spawn_ms = now_ms;
        s_fallingdots_inited = true;
        return;
    }
    if (now_ms - s_fallingdots_last_step_ms < FALLINGDOTS_STEP_MS) {
        return;
    }
    s_fallingdots_last_step_ms = now_ms;
    fallingdots_step(now_ms);
}

/* Smoothstep, for the row cross-fade. */
static float smoothstep01(float t) {
    t = clamp01(t);
    return t * t * (3.0f - 2.0f * t);
}

static tiles_standby_color_t anim_fallingdots(uint8_t row, uint8_t col, uint32_t now_ms) {
    fallingdots_update(now_ms);

    if (row == 0u) {
        return white(0.0f);
    }

    float progress = smoothstep01((float)(now_ms - s_fallingdots_last_step_ms) / (float)FALLINGDOTS_STEP_MS);

    for (uint8_t i = 0; i < FALLINGDOTS_MAX_CONCURRENT; i++) {
        const fallingdots_dot_t *dot = &s_fallingdots_active[i];
        if (!dot->active || dot->col != col) {
            continue;
        }
        if (fallingdots_next_blocked(dot)) {
            /* About to land: hold full brightness. */
            if (dot->row == (int8_t)row) {
                return white(FALLINGDOTS_ACTIVE_LEVEL);
            }
            continue;
        }
        if (dot->row == (int8_t)row) {
            return white(FALLINGDOTS_ACTIVE_LEVEL * (1.0f - progress));
        }
        if ((int8_t)(dot->row + 1) == (int8_t)row) {
            return white(FALLINGDOTS_ACTIVE_LEVEL * progress);
        }
    }

    if (fallingdots_cell_filled(row, col)) {
        return white(FALLINGDOTS_LANDED_LEVEL);
    }
    return white(0.0f);
}

/* ---- Animation registry + shared render -------------------------------- */

typedef tiles_standby_color_t (*field_fn_t)(uint8_t row, uint8_t col, uint32_t now_ms);
typedef tiles_standby_color_t (*underglow_fn_t)(uint8_t pixel_index, uint32_t now_ms);

static const field_fn_t s_animations[] = {
    anim_wave,           anim_glow,     anim_shooting_stars, anim_snake,
    anim_rgb_showcase,   anim_equalizer, anim_underglow_circle,
    anim_tile_breaker,  anim_marquee,  anim_bounce, anim_tetris, anim_paddle,
    anim_fallingdots,
};
/* Parallel to s_animations[]: NULL = underglow samples the pad field at
 * its anchors; non-NULL = the animation draws its own underglow (EQ accent,
 * circular wave, tile breaker/Tetris/Paddle flashes, marquee off). */
static const underglow_fn_t s_animation_underglow_override[] = {
    NULL, NULL, NULL, NULL, NULL, eq_underglow, circle_underglow, tb_underglow, marquee_underglow, NULL,
    tetris_underglow, pd_underglow, NULL,
};
#define NUM_ANIMATIONS ((uint8_t)(sizeof(s_animations) / sizeof(s_animations[0])))

/* Selection weights, parallel to s_animations[]: ambient 2, game demos 1,
 * so games come up half as often. */
#define ANIM_WEIGHT_REGULAR 2u
#define ANIM_WEIGHT_GAME 1u
static const uint8_t s_animation_weight[] = {
    ANIM_WEIGHT_REGULAR, /* wave */
    ANIM_WEIGHT_REGULAR, /* glow */
    ANIM_WEIGHT_REGULAR, /* shooting stars */
    ANIM_WEIGHT_GAME,    /* snake */
    ANIM_WEIGHT_REGULAR, /* RGB showcase */
    ANIM_WEIGHT_REGULAR, /* equalizer */
    ANIM_WEIGHT_REGULAR, /* underglow circle */
    ANIM_WEIGHT_GAME,    /* tile breaker */
    ANIM_WEIGHT_REGULAR, /* marquee */
    ANIM_WEIGHT_REGULAR, /* bounce */
    ANIM_WEIGHT_GAME,    /* Tetris */
    ANIM_WEIGHT_GAME,    /* Paddle */
    ANIM_WEIGHT_REGULAR, /* falling dots */
};

/* Button LEDs look brighter than pads at the same duty (different LED and
 * drive path), so the button row is scaled down uniformly. Starting guess. */
#define BUTTON_STANDBY_BRIGHTNESS_SCALE 0.35f

static void render_frame(uint8_t animation_index, uint32_t now_ms) {
    field_fn_t field = s_animations[animation_index];
    underglow_fn_t underglow_override = s_animation_underglow_override[animation_index];

    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        tiles_standby_color_t c = field(0u, col, now_ms);
        /* Buttons are monochrome: use the brightest channel, then the button
         * scale. */
        float luminance = c.r;
        if (c.g > luminance) {
            luminance = c.g;
        }
        if (c.b > luminance) {
            luminance = c.b;
        }
        tiles_buttons_set_standby_led(board_button_for_col(col), luminance * BUTTON_STANDBY_BRIGHTNESS_SCALE);
    }
    for (uint8_t row = 1u; row <= TILES_GRID_MAX_ROW; row++) {
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            tiles_standby_color_t c = field(row, col, now_ms);
            tiles_lighting_set_standby_pad_rgb(board_pad_for_row_col(row, col), c.r, c.g, c.b);
        }
    }
    for (uint8_t i = 0; i < 4u; i++) {
        tiles_standby_color_t c = (underglow_override != NULL)
                                       ? underglow_override(i, now_ms)
                                       : field(g_tiles_underglow_anchor[i].row, g_tiles_underglow_anchor[i].col, now_ms);
        tiles_lighting_set_standby_underglow_rgb(i, c.r, c.g, c.b);
    }
}

/* ---- State machine -------------------------------------------------- */

typedef enum {
    TILES_STANDBY_STATE_AWAKE = 0,
    TILES_STANDBY_STATE_STANDBY,
    TILES_STANDBY_STATE_DEEP_SLEEP,
} standby_state_t;

static standby_state_t s_state;
static uint32_t s_last_activity_ms;
static uint32_t s_last_frame_ms;
static uint32_t s_animation_switch_ms;
static uint8_t s_animation_index;
static uint8_t s_prev_animation_index; /* the animation shown before s_animation_index */

/* True only in a STANDBY entered by the 4 s circle hold. Then "-"/"+"
 * scroll animations instead of waking (see real_input_active()) and deep
 * sleep comes after 30 min instead of 20. Cleared on every exit from
 * STANDBY, so a later automatic standby never inherits it. */
static bool s_manual_screensaver;

/* Circle holds: 4 s = screensaver, 8 s = deep sleep. Otherwise circle is
 * the shift button. */
#define TILES_CIRCLE_SCREENSAVER_HOLD_MS 4000u
#define TILES_CIRCLE_DEEP_SLEEP_HOLD_MS 8000u
static bool s_circle_was_held;
static uint32_t s_circle_hold_start_ms;
static bool s_circle_screensaver_fired;
static bool s_circle_deep_sleep_fired;

/* Incoming MIDI (see tiles_standby_scan()): s_seen_midi_activity is the
 * last tiles_midi_in_activity_count() seen (only change matters).
 * s_deep_sleep_manual marks a deep sleep the PLAYER asked for (8 s hold).
 * Together with s_manual_screensaver, they let MIDI wake or hold off an
 * AUTOMATIC screensaver but never undo one the player started. */
static uint32_t s_seen_midi_activity;
static bool s_deep_sleep_manual;

/* Own "-"/"+" edge tracking for scrolling (octave_control.c stands down
 * while this mode owns the buttons). */
static bool s_scroll_prev_minus;
static bool s_scroll_prev_plus;

/* Deep-sleep timeout for a manually started screensaver. */
#define TILES_STANDBY_MANUAL_DEEP_SLEEP_TIMEOUT_MS 1800000u /* 30 * 60 * 1000 */

/* Logs which input woke standby (to check whether touch ever does). Runs
 * only on the wake transition. */
static void print_wake_source(uint32_t now_ms) {
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        if (tiles_touch_is_touched(pad)) {
            printf("[standby] waking: touch pad %u (t=%u ms)\n", pad, now_ms);
            return;
        }
    }
    for (uint8_t button = 1u; button <= TILES_NUM_FUNCTION_BUTTONS; button++) {
        if (tiles_button_is_pressed(button)) {
            printf("[standby] waking: button %u (t=%u ms)\n", button, now_ms);
            return;
        }
    }
    printf("[standby] waking: pedal (t=%u ms)\n", now_ms);
}

/* Touch, buttons, pedal (not Hall; see hall_depth_wake_triggered()). Used
 * to enter standby and to wake from it. Excluded: circle always (it has its
 * own hold handling, and a building hold must not wake on its first
 * tick), and "-"/"+" while a manual screensaver uses them to scroll. */
static bool real_input_active(void) {
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        if (tiles_touch_is_touched(pad)) {
            return true;
        }
    }

    bool scroll_mode = (s_state == TILES_STANDBY_STATE_STANDBY) && s_manual_screensaver;
    for (uint8_t button = 1u; button <= TILES_NUM_FUNCTION_BUTTONS; button++) {
        if (button == TILES_CIRCLE_BUTTON_ID) {
            continue;
        }
        if (scroll_mode && (button == 1u || button == 2u)) {
            continue;
        }
        if (tiles_button_is_pressed(button)) {
            return true;
        }
    }
    return tiles_pedal_is_sustained();
}

/* Wake-only (see TILES_STANDBY_HALL_WAKE_DEPTH). Using raw depth to decide
 * whether anything is active kept standby from ever starting (some pad
 * always looked active); as a wake check, a false positive only wakes it
 * early. */
static bool hall_depth_wake_triggered(void) {
#if TILES_STANDBY_HALL_WAKE_ENABLED
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        if (tiles_hall_get_depth(pad) >= TILES_STANDBY_HALL_WAKE_DEPTH) {
            return true;
        }
    }
#endif
    return false;
}

/* Weighted random pick (s_animation_weight[]), excluding `exclude_a` and
 * `exclude_b` (pass >= NUM_ANIMATIONS, e.g. 0xFF, to exclude nothing), so a
 * switch never repeats the current animation or bounces back to the one
 * before. One pass over the cumulative weights, no retry loop. */
static uint8_t pick_random_animation(uint8_t exclude_a, uint8_t exclude_b) {
    if (NUM_ANIMATIONS <= 2u) {
        return 0u;
    }

    uint16_t total_weight = 0u;
    for (uint8_t i = 0; i < NUM_ANIMATIONS; i++) {
        if (i == exclude_a || i == exclude_b) {
            continue;
        }
        total_weight += s_animation_weight[i];
    }

    uint16_t roll = (uint16_t)(rand() % total_weight);
    for (uint8_t i = 0; i < NUM_ANIMATIONS; i++) {
        if (i == exclude_a || i == exclude_b) {
            continue;
        }
        if (roll < s_animation_weight[i]) {
            return i;
        }
        roll = (uint16_t)(roll - s_animation_weight[i]);
    }
    return 0u; /* unreachable: total_weight > 0 when NUM_ANIMATIONS > 2 */
}

static void enter_standby(uint32_t now_ms) {
    s_state = TILES_STANDBY_STATE_STANDBY;
    /* Exclude what was showing when standby last ended, so re-entering soon
     * after doesn't repeat it. */
    uint8_t next = pick_random_animation(s_animation_index, s_prev_animation_index);
    s_prev_animation_index = s_animation_index;
    s_animation_index = next;
    s_animation_switch_ms = now_ms;
    s_last_frame_ms = 0u; /* forces an immediate first frame */
    tiles_lighting_set_standby_active(true);
    tiles_buttons_set_standby_active(true);
}

/* Back to AWAKE from STANDBY or DEEP_SLEEP (same teardown). Also clears
 * the manual flag and the circle hold tracker. */
static void exit_standby(void) {
    s_state = TILES_STANDBY_STATE_AWAKE;
    s_manual_screensaver = false;
    s_deep_sleep_manual = false;
    s_circle_was_held = false;
    tiles_lighting_set_standby_active(false);
    tiles_buttons_set_standby_active(false);
    /* Always safe to clear (only deep sleep sets it). */
    tiles_haptics_set_sleep_silenced(false);
}

/* Deep sleep, the only sleep state: reached by the inactivity timeout or
 * the 8 s circle hold. Everything dark except circle pulsing slowly (the
 * sign it's asleep, not off). Claims the standby rendering itself, since
 * the 8 s hold can arrive here without passing through STANDBY. */
static void enter_deep_sleep(void) {
    s_state = TILES_STANDBY_STATE_DEEP_SLEEP;
    s_manual_screensaver = false;
    s_deep_sleep_manual = false; /* the circle-hold caller sets it true right after */
    tiles_lighting_set_standby_active(true);
    tiles_buttons_set_standby_active(true);
    s_last_frame_ms = 0u; /* forces an immediate first frame */
    /* Haptics off in deep sleep only (regular standby leaves them running). */
    tiles_haptics_set_sleep_silenced(true);
}

/* Circle's long-press gesture, run at the top of every scan in any state:
 * a continuous hold passes 4 s (manual screensaver) then 8 s (deep sleep),
 * each firing once per hold. */
static void handle_circle_hold(uint32_t now_ms) {
    bool held = tiles_button_is_pressed(TILES_CIRCLE_BUTTON_ID);

    if (held && !s_circle_was_held) {
        s_circle_hold_start_ms = now_ms;
        s_circle_screensaver_fired = false;
        s_circle_deep_sleep_fired = false;
    }

    if (held) {
        uint32_t held_ms = now_ms - s_circle_hold_start_ms;
        if (held_ms >= TILES_CIRCLE_DEEP_SLEEP_HOLD_MS && !s_circle_deep_sleep_fired) {
            s_circle_deep_sleep_fired = true;
            s_last_activity_ms = now_ms;
            /* MIDI panic on the deliberate 8 s hold only, not the automatic timeout,
             * and before sleeping (it's a "stop now" action). */
            tiles_midi_send_panic();
            enter_deep_sleep();
            s_deep_sleep_manual = true; /* the player asked for this one */
        } else if (held_ms >= TILES_CIRCLE_SCREENSAVER_HOLD_MS && !s_circle_screensaver_fired) {
            s_circle_screensaver_fired = true;
            s_last_activity_ms = now_ms;
            enter_standby(now_ms);
            s_manual_screensaver = true;
        }
    } else if (s_circle_was_held && !s_circle_screensaver_fired) {
        /* A short tap (released before 4 s) wakes the board like any button.
         * Circle is excluded from the generic wake check (a building hold mustn't
         * wake on its first tick), so it's handled here on release. */
        if (s_state != TILES_STANDBY_STATE_AWAKE) {
            s_last_activity_ms = now_ms;
            printf("[standby] waking: button %u (t=%u ms)\n", (unsigned)TILES_CIRCLE_BUTTON_ID, now_ms);
            exit_standby();
        }
    }

    s_circle_was_held = held;
}

/* "-"/"+" step through animations in order (not random) while a manual
 * screensaver shows. Each press resets the inactivity clock without
 * counting as a wake. */
static void handle_manual_scroll_input(uint32_t now_ms) {
    bool minus = tiles_button_is_pressed(1u); /* SW1 "-" */
    bool plus = tiles_button_is_pressed(2u);  /* SW2 "+" */

    if (minus && !s_scroll_prev_minus) {
        s_prev_animation_index = s_animation_index;
        s_animation_index = (uint8_t)((s_animation_index + NUM_ANIMATIONS - 1u) % NUM_ANIMATIONS);
        s_animation_switch_ms = now_ms;
        s_last_activity_ms = now_ms;
        s_last_frame_ms = 0u; /* redraw the new animation now */
    }
    if (plus && !s_scroll_prev_plus) {
        s_prev_animation_index = s_animation_index;
        s_animation_index = (uint8_t)((s_animation_index + 1u) % NUM_ANIMATIONS);
        s_animation_switch_ms = now_ms;
        s_last_activity_ms = now_ms;
        s_last_frame_ms = 0u;
    }

    s_scroll_prev_minus = minus;
    s_scroll_prev_plus = plus;
}

static uint32_t current_deep_sleep_timeout_ms(void) {
    if (s_manual_screensaver) {
        /* The player's choice wins over the sequencer default (both 30 min, by
         * coincidence). */
        return TILES_STANDBY_MANUAL_DEEP_SLEEP_TIMEOUT_MS;
    }
    if (tiles_op_mode_is_sequencer_active()) {
        return TILES_STANDBY_SEQUENCER_DEEP_SLEEP_TIMEOUT_MS;
    }
    return TILES_STANDBY_DEEP_SLEEP_TIMEOUT_MS;
}

#define DEEP_SLEEP_PULSE_PERIOD_MS 3000.0f
#define DEEP_SLEEP_PULSE_MIN 0.03f
#define DEEP_SLEEP_PULSE_MAX 0.35f

static void render_deep_sleep_frame(uint32_t now_ms) {
    float phase = (float)now_ms / DEEP_SLEEP_PULSE_PERIOD_MS;
    float raw = 0.5f + 0.5f * sinf(2.0f * TILES_STANDBY_PI * phase);
    float pulse = DEEP_SLEEP_PULSE_MIN + (DEEP_SLEEP_PULSE_MAX - DEEP_SLEEP_PULSE_MIN) * raw;

    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        float level = (col == TILES_CIRCLE_BUTTON_COL) ? pulse : 0.0f;
        tiles_buttons_set_standby_led(board_button_for_col(col), level);
    }
    for (uint8_t row = 1u; row <= TILES_GRID_MAX_ROW; row++) {
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            tiles_lighting_set_standby_pad_rgb(board_pad_for_row_col(row, col), 0.0f, 0.0f, 0.0f);
        }
    }
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        tiles_lighting_set_standby_underglow_rgb(i, 0.0f, 0.0f, 0.0f);
    }
}

void tiles_standby_init(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    s_state = TILES_STANDBY_STATE_AWAKE;
    s_last_activity_ms = now_ms;
    s_last_frame_ms = 0u;
    s_animation_switch_ms = now_ms;
    /* No history yet, so the first pick is unconstrained. Never used as an
     * index: enter_standby() picks a valid one before rendering. */
    s_animation_index = 0xFFu;
    s_prev_animation_index = 0xFFu;
    s_stars_inited = false;
    s_manual_screensaver = false;
    s_circle_was_held = false;
    s_circle_screensaver_fired = false;
    s_circle_deep_sleep_fired = false;
    s_seen_midi_activity = tiles_midi_in_activity_count();
    s_deep_sleep_manual = false;
    s_scroll_prev_minus = false;
    s_scroll_prev_plus = false;
    /* Seeds the ONE shared rand() stream (games, animations, Simon Says) from
     * hardware entropy (get_rand_32(), pico_rand). Seeding from boot time,
     * which is nearly the same every boot, made "random" patterns repeat. */
    srand((unsigned int)get_rand_32());
}

void tiles_standby_scan(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    /* Runs in every state; may change s_state for the rest of this call. */
    handle_circle_hold(now_ms);

    if (s_state == TILES_STANDBY_STATE_STANDBY && s_manual_screensaver) {
        handle_manual_scroll_input(now_ms);
    }

    if (real_input_active()) {
        s_last_activity_ms = now_ms;
        if (s_state != TILES_STANDBY_STATE_AWAKE) {
            print_wake_source(now_ms);
            exit_standby();
        }
        return;
    }

    /* Incoming MIDI counts as activity: tiles_midi_in_activity_count() moves
     * only for notes and clock/transport, i.e. a DAW playing or an echo, not
     * just Live being open. Sampled every scan so a stale value never looks
     * like a fresh burst.
     *   - AWAKE: refreshes the idle timer like a touch, so a playing DAW keeps
     *     the screensaver away (sequencer mode included).
     *   - AUTOMATIC standby/deep sleep: wakes, so the pads show what MIDI is
     *     driving.
     *   - MANUAL screensaver/deep sleep: left alone, and MIDI doesn't push
     *     their timeouts back.
     * No printf here: this runs at clock rate. */
    uint32_t midi_activity = tiles_midi_in_activity_count();
    bool midi_active = (midi_activity != s_seen_midi_activity);
    s_seen_midi_activity = midi_activity;
    if (midi_active) {
        if (s_state == TILES_STANDBY_STATE_AWAKE) {
            s_last_activity_ms = now_ms;
        } else {
            bool manual = (s_state == TILES_STANDBY_STATE_STANDBY && s_manual_screensaver) ||
                          (s_state == TILES_STANDBY_STATE_DEEP_SLEEP && s_deep_sleep_manual);
            if (!manual) {
                s_last_activity_ms = now_ms;
                exit_standby();
                return;
            }
        }
    }

    if (s_state == TILES_STANDBY_STATE_AWAKE) {
        /* An open op_mode menu (mode picker etc.) holds the timer off: reading a
         * menu takes no touch, and the screensaver used to replace it mid-browse. */
        if (tiles_op_mode_has_menu_open()) {
            s_last_activity_ms = now_ms;
            return;
        }
        uint32_t idle_timeout =
            tiles_op_mode_is_sequencer_active() ? TILES_STANDBY_SEQUENCER_IDLE_TIMEOUT_MS : TILES_STANDBY_IDLE_TIMEOUT_MS;
        if (now_ms - s_last_activity_ms >= idle_timeout) {
            enter_standby(now_ms);
        }
        return;
    }

    /* STANDBY or DEEP_SLEEP with no real input: the only other wake path is
     * Hall depth (wake-only; see hall_depth_wake_triggered()). */
    if (hall_depth_wake_triggered()) {
        s_last_activity_ms = now_ms;
        exit_standby();
        return;
    }

    if (s_state == TILES_STANDBY_STATE_DEEP_SLEEP) {
        if (now_ms - s_last_frame_ms >= TILES_STANDBY_FRAME_INTERVAL_MS) {
            render_deep_sleep_frame(now_ms);
            s_last_frame_ms = now_ms;
        }
        return;
    }

    /* STATE_STANDBY */
    if (now_ms - s_last_activity_ms >= current_deep_sleep_timeout_ms()) {
        enter_deep_sleep();
        return;
    }

    /* No auto-cycling while manually browsing, so a chosen animation stays. */
    if (!s_manual_screensaver && now_ms - s_animation_switch_ms >= TILES_STANDBY_ANIMATION_CYCLE_MS) {
        /* Random, excluding the current and previous animation. */
        uint8_t next = pick_random_animation(s_animation_index, s_prev_animation_index);
        s_prev_animation_index = s_animation_index;
        s_animation_index = next;
        s_animation_switch_ms = now_ms;
    }

    if (now_ms - s_last_frame_ms >= TILES_STANDBY_FRAME_INTERVAL_MS) {
        render_frame(s_animation_index, now_ms);
        s_last_frame_ms = now_ms;
    }
}

bool tiles_standby_is_active(void) {
    return s_state == TILES_STANDBY_STATE_STANDBY;
}

bool tiles_standby_is_deep_sleep(void) {
    return s_state == TILES_STANDBY_STATE_DEEP_SLEEP;
}

bool tiles_standby_owns_octave_buttons(void) {
    return s_state == TILES_STANDBY_STATE_STANDBY && s_manual_screensaver;
}
