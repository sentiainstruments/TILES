#include "standby.h"

#include "board_layout.h"
#include "board_pins.h"
#include "buttons.h"
#include "hall.h"
#include "lighting.h"
#include "pedal.h"
#include "touch.h"

#include "pico/time.h"

#include <math.h>
#include <stdlib.h>

/* Demo defaults per the user's own direction: 1 minute is fine for
 * demo mode even though the real idle timeout will likely change once
 * this isn't just a demo; alternating every "few minutes" is
 * intentionally vague -- 2 minutes is a starting guess, not measured
 * against how it actually feels to watch. */
#define TILES_STANDBY_IDLE_TIMEOUT_MS 60000u
#define TILES_STANDBY_ANIMATION_CYCLE_MS 120000u

/* After 15 minutes of TOTAL inactivity (same s_last_activity_ms clock
 * that gates entering standby in the first place -- not 15 minutes of
 * animation specifically, 15 minutes since the last real touch/button/
 * pedal event), standby's animations stop and the board drops to a
 * deeper power-saving indicator: everything dark except the circle
 * button pulsing gently. See enter_power_saving() below. Explicit
 * demo-mode default, like the other timings here. */
#define TILES_STANDBY_POWER_SAVING_TIMEOUT_MS 900000u /* 15 * 60 * 1000 */

/* ~25fps. Unmeasured against real I2C bus load (every pad write is a
 * mux-select-enable-disable dance, see lighting.c) -- if 24 pads + 6
 * buttons + 4 underglow pixels every frame turns out to compete with
 * anything else on the bus, raise this first. */
#define TILES_STANDBY_FRAME_INTERVAL_MS 40u

/* Hall-depth wake fallback: touching a pad during the standby animation
 * was observed to NOT reliably wake it (buttons/pedal woke it fine),
 * pointing at the pad-LED animation itself interfering with capacitive
 * touch sensing -- root cause unconfirmed, see git history for the
 * fuller investigation. Hall (magnetic, not capacitive) isn't subject
 * to whatever that is, so it's meant as an independent second wake
 * path. Checked ONLY while already in standby (hall_depth_wake_triggered()
 * below), never as part of deciding whether to enter standby -- folding
 * it into that check first broke standby from ever entering at all,
 * since hall.c's depth has no drift compensation and can look
 * permanently "active" on its own.
 *
 * DISABLED for now (TILES_STANDBY_HALL_WAKE_ENABLED 0): the magnets
 * aren't in their final position yet (mid-plate assembly still being
 * printed as of this change), so hall.c's rest baseline and every depth
 * reading right now are against a physically incomplete, unrepresentative
 * setup -- any threshold picked against that data would be meaningless,
 * not just untuned, and was the reason standby kept bouncing right back
 * out even after being moved to wake-only. Re-enable once the magnets
 * are seated and hall.c's baseline/depth can be trusted -- pick
 * TILES_STANDBY_HALL_WAKE_DEPTH from real rest-vs-pressed numbers at
 * that point, not another guess. Until then, standby only wakes via
 * touch/button/pedal (see real_input_active()) -- touch not reliably
 * waking it is still open, tracked separately from this Hall path. */
#define TILES_STANDBY_HALL_WAKE_ENABLED 0
#define TILES_STANDBY_HALL_WAKE_DEPTH 1000u

#define TILES_STANDBY_PI 3.14159265358979323846f

/* Grid bounds, pad/button/underglow mapping (board_pad_for_row_col(),
 * board_button_for_col(), g_tiles_underglow_anchor[]) now live in
 * board/board_layout.h -- shared with services/boot_sequence.c, which
 * needs the same "board as one 5x6 grid" model for the power-on
 * animation. */

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

/* Every animation returns full RGB now (0.0-1.0 per channel), not a
 * single brightness scalar -- needed for anim_rgb_showcase (animation
 * 5) below. The white animations (1-4) just return r=g=b=v via white(). */
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

/* ---- Animation 1: wave -------------------------------------------------
 * A traveling diagonal band: phase is a function of (row + col), so
 * constant-phase lines run at 45 degrees across the grid, not
 * horizontally or vertically. WAVE_CONTRAST_GAMMA (>1) biases the curve
 * toward dark, so most of the cycle reads as genuinely off with a
 * brighter band passing through, rather than a smooth 50/50 sine. */

#define WAVE_LENGTH_DIAG 3.0f
#define WAVE_PERIOD_MS 3000.0f
#define WAVE_CONTRAST_GAMMA 2.2f

static tiles_standby_color_t anim_wave(uint8_t row, uint8_t col, uint32_t now_ms) {
    float diag = (float)row + (float)col;
    float phase = diag / WAVE_LENGTH_DIAG - (float)now_ms / WAVE_PERIOD_MS;
    float raw = 0.5f + 0.5f * sinf(2.0f * TILES_STANDBY_PI * phase);
    return white(powf(clamp01(raw), WAVE_CONTRAST_GAMMA));
}

/* ---- Animation 2: glow center-out --------------------------------------
 * A ring pulsing outward from the grid's visual center, repeating.
 * GLOW_RING_WIDTH narrowed and the result squared for a sharper, more
 * dramatic ring against a fully dark background instead of a soft,
 * diffuse glow. */

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

/* ---- Animation 3: shooting stars ----------------------------------------
 * Comet-tailed points falling top (above row 0) to bottom (past row 4),
 * each respawning at a random column once it exits. More "complex" than
 * the original version: more concurrent stars, per-star randomized
 * speed and tail length (not identical falls), exponential (not linear)
 * tail decay for a sharper head/softer trail, and a subtle twinkle
 * (brightness jitter) per star. The only stateful animation here -- the
 * others are pure functions of (row, col, time); this one owns a small
 * fixed-size particle array. */

#define NUM_STARS 5u
#define STAR_TAIL_ROWS_MIN 2.0f
#define STAR_TAIL_ROWS_MAX 4.5f
#define STAR_SPEED_ROWS_PER_MS_MIN (1.0f / 300.0f)
#define STAR_SPEED_ROWS_PER_MS_MAX (1.0f / 140.0f)
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
            /* Stagger initial spawn times so they don't all fall in
             * lockstep the first time this animation is shown. */
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

/* ---- Animation 4: snake --------------------------------------------------
 * A fixed-length lit segment crawling along a deterministic boustrophedon
 * (serpentine) path that visits every button and pad cell, then reverses
 * and crawls back -- a ping-pong bounce, so the snake's direction of
 * travel alternates each full traversal instead of teleport-resetting
 * back to the start. */

#define SNAKE_STEP_MS 150u
#define SNAKE_TAIL_LENGTH 6u
#define SNAKE_PATH_LENGTH 30u /* 5 rows (0-4) x 6 cols */

static uint8_t snake_path_index(uint8_t row, uint8_t col) {
    uint8_t col0 = (uint8_t)(col - TILES_GRID_MIN_COL); /* 0-5 */
    /* Even rows walk left->right, odd rows right->left. */
    if ((row % 2u) == 0u) {
        return (uint8_t)(row * 6u + col0);
    }
    return (uint8_t)(row * 6u + (5u - col0));
}

static tiles_standby_color_t anim_snake(uint8_t row, uint8_t col, uint32_t now_ms) {
    uint32_t half = SNAKE_PATH_LENGTH - 1u;
    uint32_t cycle_len = 2u * half;
    uint32_t t_mod = (now_ms / SNAKE_STEP_MS) % cycle_len;
    bool forward = (t_mod <= half);
    uint32_t head = forward ? t_mod : (cycle_len - t_mod);

    uint8_t p = snake_path_index(row, col);
    /* "diff" is how far this cell is behind the head in the CURRENT
     * direction of travel -- sign flips with direction so the tail
     * trails correctly on both the forward and backward pass. */
    int32_t diff = forward ? ((int32_t)head - (int32_t)p) : ((int32_t)p - (int32_t)head);
    if (diff < 0 || (uint32_t)diff >= SNAKE_TAIL_LENGTH) {
        return white(0.0f);
    }
    return white(1.0f - (float)diff / (float)SNAKE_TAIL_LENGTH);
}

/* ---- Animation 5: blue/purple RGB showcase ------------------------------
 * Shows off both the underglow AND the pad grid's actual RGB capability
 * (every other animation is deliberately white) -- a diagonal value
 * wave, same shape as anim_wave, but hue cycles within the blue-to-
 * violet range instead of brightness alone staying white. Underglow
 * samples this same field at its usual anchor points (see
 * s_animation_underglow_off below, false for this animation), so it
 * shows the same moving color as the pads instead of sitting the
 * animation out. The function-button row is held to a low, constant,
 * non-animated dim glow rather than fully off or full brightness --
 * button LEDs are plain monochrome PWM, not addressable RGB, so they
 * can't show the color itself, but going fully dark read as an
 * unrelated glitch; a low, deliberately non-pulsing glow reads as
 * "quietly present" without competing with the pad color. */

#define RGB_WAVE_LENGTH_DIAG 3.0f
#define RGB_WAVE_PERIOD_MS 3500.0f
#define RGB_CONTRAST_GAMMA 1.8f
#define RGB_HUE_CYCLE_MS 6000.0f
#define RGB_HUE_MIN_DEG 220.0f /* blue */
#define RGB_HUE_MAX_DEG 285.0f /* violet/purple */

/* Raw luminance returned for the button row -- render_frame() further
 * multiplies every animation's button row by BUTTON_STANDBY_BRIGHTNESS_SCALE
 * (0.35), so the actual final brightness is roughly this times that,
 * not this value directly. Picked to land low/subtle after that scale;
 * unmeasured, adjust directly if it still reads as too bright or too
 * dark once seen lit. */
#define RGB_SHOWCASE_BUTTON_ROW_LEVEL 0.35f

/* Standard HSV->RGB, s always 1.0 here (fully saturated blue/purple hues). */
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

/* ---- Animation 6: graphic equalizer --------------------------------------
 * Each column is a fake VU/EQ bar: a slow, purely synthetic level
 * (layered sines, different period per column so bars don't move in
 * lockstep) lights that column's bottom-up segments -- rows 3-4 (the
 * bottom two) blue, row 2 yellow, row 1 (top) red, like a classic
 * hardware graphic equalizer. A per-column "redline" peak marker sticks
 * at the highest segment reached and only falls slowly (EQ_PEAK_DECAY_PER_MS),
 * independent of the bar's own motion -- the peak-hold behavior real
 * EQ/VU hardware has.
 *
 * Reworked from real feedback: was too fast and too continuously lit,
 * reading as busy/mechanical rather than stylized. Two changes fix
 * that: every period is much longer now, and each column also has its
 * own slow, independent "phrase" envelope (eq_bar_envelope() below)
 * that most of the time sits low -- so columns spend real stretches
 * fully dark (not just dim), sometimes several at once, rather than
 * always showing at least a segment or two. Function buttons are fully
 * off (were a faint minimal glow); underglow is a simple constant blue
 * accent (see eq_underglow below) rather than tracking the bars --
 * doesn't correspond to any one column, so there's nothing meaningful
 * for it to track. */

#define EQ_NUM_COLS 6u
#define EQ_LIT_LEVEL 0.85f
#define EQ_PEAK_LEVEL 1.0f
#define EQ_UNDERGLOW_LEVEL 0.7f
/* Both roughly 3-4x the original periods -- the main fix for "too
 * fast". */
#define EQ_BAR_PERIOD1_BASE_MS 3200.0f
#define EQ_BAR_PERIOD2_BASE_MS 1300.0f
/* The per-column "phrase" envelope: how active vs. silent a column is
 * right now, cycling slowly and independently per column. Biased toward
 * low via EQ_ENVELOPE_SHAPE (>1 spends more time near 0 than near 1) --
 * "more empty space, sometimes columns fully off" is exactly a low
 * envelope value forcing the bar level to ~0 regardless of the
 * (still-moving) raw motion underneath it. */
#define EQ_ENVELOPE_PERIOD_MS_BASE 7000.0f
#define EQ_ENVELOPE_SHAPE 2.2f
/* Full fall from a maxed-out peak to 0 takes about 6s -- "drops slowly",
 * nudged up slightly to match the generally slower, more stylized pace. */
#define EQ_PEAK_DECAY_PER_MS (1.0f / 6000.0f)

static float s_eq_peak[EQ_NUM_COLS];
static uint32_t s_eq_peak_update_ms[EQ_NUM_COLS];
static bool s_eq_inited;

static float eq_bar_envelope(uint8_t col, uint32_t now_ms) {
    float period = EQ_ENVELOPE_PERIOD_MS_BASE + (float)col * 900.0f;
    float phase = (float)col * 0.9f;
    float raw = 0.5f + 0.5f * sinf(2.0f * TILES_STANDBY_PI * (float)now_ms / period + phase);
    return powf(clamp01(raw), EQ_ENVELOPE_SHAPE);
}

/* Deterministic function of (col, time) -- two sine waves at different,
 * per-column periods/phases blended together for texture, then scaled
 * by that column's own slow envelope above so the bar genuinely goes
 * quiet (not just dim) during that envelope's low stretches. No
 * randomness/state needed for the bar itself, only for the peak below. */
static float eq_bar_level(uint8_t col, uint32_t now_ms) {
    float col_f = (float)col;
    float period1 = EQ_BAR_PERIOD1_BASE_MS + col_f * 400.0f;
    float period2 = EQ_BAR_PERIOD2_BASE_MS + col_f * 180.0f;
    float phase = col_f * 1.7f;
    float s1 = sinf(2.0f * TILES_STANDBY_PI * (float)now_ms / period1 + phase);
    float s2 = sinf(2.0f * TILES_STANDBY_PI * (float)now_ms / period2 + phase * 1.3f);
    float raw = clamp01(0.5f + 0.3f * s1 + 0.2f * s2);
    return raw * eq_bar_envelope(col, now_ms);
}

/* Advances every column's peak by however long it's been since that
 * column was last updated, then only if this frame's bar level is
 * higher. Guarded by s_eq_peak_update_ms[col] == now_ms so calling this
 * once per (row, col) cell within the same render_frame() -- up to 4
 * times per column, once per pad row -- only actually advances each
 * column's peak once per frame, not once per cell (now_ms is identical
 * across every cell queried within one frame, same self-limiting
 * pattern anim_shooting_stars' respawn already relies on). */
static void eq_update_peaks(uint32_t now_ms) {
    if (!s_eq_inited) {
        for (uint8_t i = 0; i < EQ_NUM_COLS; i++) {
            s_eq_peak[i] = 0.0f;
            s_eq_peak_update_ms[i] = now_ms;
        }
        s_eq_inited = true;
    }
    for (uint8_t i = 0; i < EQ_NUM_COLS; i++) {
        if (s_eq_peak_update_ms[i] == now_ms) {
            continue;
        }
        uint32_t dt = now_ms - s_eq_peak_update_ms[i];
        s_eq_peak_update_ms[i] = now_ms;

        uint8_t col = (uint8_t)(TILES_GRID_MIN_COL + i);
        float level = eq_bar_level(col, now_ms);
        float decayed = s_eq_peak[i] - EQ_PEAK_DECAY_PER_MS * (float)dt;
        if (decayed < 0.0f) {
            decayed = 0.0f;
        }
        s_eq_peak[i] = (level > decayed) ? level : decayed;
    }
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
    tiles_standby_color_t blue = {0.0f, 0.0f, 1.0f}; /* rows 3, 4 */
    return blue;
}

static tiles_standby_color_t anim_equalizer(uint8_t row, uint8_t col, uint32_t now_ms) {
    eq_update_peaks(now_ms);

    if (row == 0u) {
        return white(0.0f); /* function buttons fully off for this animation */
    }

    uint8_t col0 = (uint8_t)(col - TILES_GRID_MIN_COL);
    float level = eq_bar_level(col, now_ms);
    float peak = s_eq_peak[col0];

    /* Same threshold quantization for both: row_threshold(row) is this
     * row's height as a 0-1 fraction (row 4 = 0.25 ... row 1 = 1.0), and
     * peak_segment is how many segments (1-4) the peak has reached,
     * using the identical quantization so the two stay consistent. */
    float row_threshold = (float)(5u - row) / 4.0f;
    bool bar_lit = (level >= row_threshold);

    uint8_t peak_segment = (uint8_t)(peak * 4.0f);
    if (peak_segment > 4u) {
        peak_segment = 4u;
    }
    uint8_t peak_row = (peak_segment >= 1u) ? (uint8_t)(5u - peak_segment) : 0u;

    if (peak_row == row && !bar_lit) {
        /* The "redline" -- always red regardless of this row's normal
         * EQ color, matching the classic peak-hold LED. */
        tiles_standby_color_t c = {EQ_PEAK_LEVEL, 0.0f, 0.0f};
        return c;
    }
    if (bar_lit) {
        tiles_standby_color_t c = eq_row_color(row);
        tiles_standby_color_t scaled = {c.r * EQ_LIT_LEVEL, c.g * EQ_LIT_LEVEL, c.b * EQ_LIT_LEVEL};
        return scaled;
    }
    return white(0.0f);
}

static tiles_standby_color_t eq_underglow(uint8_t pixel_index, uint32_t now_ms) {
    (void)pixel_index;
    (void)now_ms;
    tiles_standby_color_t c = {0.0f, 0.0f, EQ_UNDERGLOW_LEVEL};
    return c;
}

/* ---- Animation 7: circular underglow wave -------------------------------
 * Only the underglow does anything -- a wave travels around the 4
 * pixels in their actual physical circular order (see
 * g_tiles_underglow_circular_position in board_layout.h), each pixel
 * rising and dimming significantly as the wave passes through, going
 * around and around continuously. Pads and buttons sit at a flat,
 * minimal, non-animated brightness so the underglow motion is the whole
 * focus without the rest of the board going fully dark. */

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

/* ---- Animation registry + shared render -------------------------------- */

typedef tiles_standby_color_t (*field_fn_t)(uint8_t row, uint8_t col, uint32_t now_ms);
typedef tiles_standby_color_t (*underglow_fn_t)(uint8_t pixel_index, uint32_t now_ms);

static const field_fn_t s_animations[] = {
    anim_wave, anim_glow, anim_shooting_stars, anim_snake, anim_rgb_showcase, anim_equalizer,
    anim_underglow_circle,
};
/* Parallel to s_animations[] -- NULL means underglow samples the same
 * field the pads use at its anchor points (animations 1-5, where
 * underglow mirroring the pad grid is exactly what's wanted). A
 * non-NULL entry means underglow needs genuinely different behavior
 * from whatever the pad field computes at that (row, col) -- the
 * equalizer's underglow is a constant green unrelated to any one
 * column's bar, and the circular-wave animation's whole point is
 * underglow-specific motion indexed by pixel, not by row/col at all. */
static const underglow_fn_t s_animation_underglow_override[] = {
    NULL, NULL, NULL, NULL, NULL, eq_underglow, circle_underglow,
};
#define NUM_ANIMATIONS ((uint8_t)(sizeof(s_animations) / sizeof(s_animations[0])))

/* Function-button LEDs read noticeably brighter than pad LEDs at the
 * same commanded duty (different LED/diffusion/drive path -- PCA9685
 * PWM vs SK6805 addressable) -- observed on real hardware as the top
 * row "overpowering" every animation. Scaled down uniformly here rather
 * than per-animation since it's a hardware-brightness mismatch, not an
 * animation design choice. Unmeasured -- a starting guess, adjust if
 * still too bright/now too dim once seen lit. */
#define BUTTON_STANDBY_BRIGHTNESS_SCALE 0.35f

static void render_frame(uint8_t animation_index, uint32_t now_ms) {
    field_fn_t field = s_animations[animation_index];
    underglow_fn_t underglow_override = s_animation_underglow_override[animation_index];

    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        tiles_standby_color_t c = field(0u, col, now_ms);
        /* Buttons are monochrome -- collapse color to a single
         * brightness via the brightest channel, then apply the
         * button-specific dimming above. */
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
    TILES_STANDBY_STATE_POWER_SAVING,
} standby_state_t;

static standby_state_t s_state;
static uint32_t s_last_activity_ms;
static uint32_t s_last_frame_ms;
static uint32_t s_animation_switch_ms;
static uint8_t s_animation_index;
static uint8_t s_prev_animation_index; /* the one that played immediately before s_animation_index */

/* Touch/button/pedal only -- deliberately NOT Hall (see
 * hall_depth_wake_triggered() below for why Hall is kept separate).
 * Used both to decide whether to enter standby and, once in it, as one
 * of the two ways to wake back up. */
static bool real_input_active(void) {
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        if (tiles_touch_is_touched(pad)) {
            return true;
        }
    }
    for (uint8_t button = 1u; button <= TILES_NUM_FUNCTION_BUTTONS; button++) {
        if (tiles_button_is_pressed(button)) {
            return true;
        }
    }
    return tiles_pedal_is_sustained();
}

/* See TILES_STANDBY_HALL_WAKE_DEPTH above for why this exists. Checked
 * ONLY while already in standby (tiles_standby_scan() below), never as
 * part of deciding whether to ENTER standby or as the idle-timer reset
 * condition -- hall.c's depth is a raw, uncalibrated reading against a
 * baseline captured once at boot with no drift compensation yet (see
 * hall.h), so ambient noise or thermal drift alone can plausibly sit
 * above this threshold for some pad at any given moment. Folding that
 * into the same check used to decide "is anything active" broke
 * standby entirely the first time it was tried (real symptom: standby
 * never triggered at all) -- some pad's drifted/noisy reading looked
 * "always active" and the idle timer never got a chance to elapse.
 * Restricting it to wake-only means a false positive here just costs an
 * unnecessary early exit from standby, not a permanently broken idle
 * timer. */
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

/* Picks a random animation index, excluding both `exclude_a` and
 * `exclude_b` (pass a value >= NUM_ANIMATIONS, e.g. 0xFFu, for either to
 * not exclude anything there -- used for the very first pick, which has
 * no real history to avoid repeating). Excluding both the animation
 * about to end AND the one before it means a switch never immediately
 * repeats the current animation, and never bounces straight back to
 * the one two animations ago either (e.g. A, B, A back-to-back) -- with
 * NUM_ANIMATIONS==5 this still leaves 3 valid choices, so the loop
 * always terminates. */
static uint8_t pick_random_animation(uint8_t exclude_a, uint8_t exclude_b) {
    if (NUM_ANIMATIONS <= 2u) {
        return 0u;
    }
    uint8_t next;
    do {
        next = (uint8_t)(rand() % NUM_ANIMATIONS);
    } while (next == exclude_a || next == exclude_b);
    return next;
}

static void enter_standby(uint32_t now_ms) {
    s_state = TILES_STANDBY_STATE_STANDBY;
    /* Excludes whatever was showing (and the one before that) when
     * standby last ended, so re-entering standby shortly after leaving
     * it doesn't immediately repeat the same animation either. */
    uint8_t next = pick_random_animation(s_animation_index, s_prev_animation_index);
    s_prev_animation_index = s_animation_index;
    s_animation_index = next;
    s_animation_switch_ms = now_ms;
    s_last_frame_ms = 0u; /* forces an immediate first frame below */
    tiles_lighting_set_standby_active(true);
    tiles_buttons_set_standby_active(true);
}

/* Wakes to AWAKE from either STANDBY or POWER_SAVING -- the same
 * teardown either way (hand rendering back to touch-driven behavior),
 * so one function covers both. */
static void exit_standby(void) {
    s_state = TILES_STANDBY_STATE_AWAKE;
    tiles_lighting_set_standby_active(false);
    tiles_buttons_set_standby_active(false);
}

/* Deeper dormant state after TILES_STANDBY_POWER_SAVING_TIMEOUT_MS of
 * total inactivity: animations stop, everything goes dark except the
 * circle button (SW6, the rightmost -- see TILES_CIRCLE_BUTTON_COL in
 * board_layout.h), which pulses gently to show how to wake it back up.
 * lighting/buttons standby-active is already true from enter_standby()
 * -- this only changes s_state, no need to re-claim the rendering
 * path. */
static void enter_power_saving(void) {
    s_state = TILES_STANDBY_STATE_POWER_SAVING;
    s_last_frame_ms = 0u; /* forces an immediate first frame */
}

#define POWER_SAVING_PULSE_PERIOD_MS 3000.0f
#define POWER_SAVING_PULSE_MIN 0.03f
#define POWER_SAVING_PULSE_MAX 0.35f

static void render_power_saving_frame(uint32_t now_ms) {
    float phase = (float)now_ms / POWER_SAVING_PULSE_PERIOD_MS;
    float raw = 0.5f + 0.5f * sinf(2.0f * TILES_STANDBY_PI * phase);
    float pulse = POWER_SAVING_PULSE_MIN + (POWER_SAVING_PULSE_MAX - POWER_SAVING_PULSE_MIN) * raw;

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
    /* Both out of range -- nothing to avoid repeating yet, so the very
     * first standby entry's pick is fully unconstrained. Never used as a
     * real array index: enter_standby() always overwrites
     * s_animation_index with a valid pick before STANDBY rendering ever
     * runs. */
    s_animation_index = 0xFFu;
    s_prev_animation_index = 0xFFu;
    s_stars_inited = false;
    srand(now_ms);
}

void tiles_standby_scan(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    if (real_input_active()) {
        s_last_activity_ms = now_ms;
        if (s_state != TILES_STANDBY_STATE_AWAKE) {
            exit_standby();
        }
        return;
    }

    if (s_state == TILES_STANDBY_STATE_AWAKE) {
        if (now_ms - s_last_activity_ms >= TILES_STANDBY_IDLE_TIMEOUT_MS) {
            enter_standby(now_ms);
        }
        return;
    }

    /* STANDBY or POWER_SAVING: real_input_active() above was false, so
     * the only remaining wake path is Hall depth -- see
     * hall_depth_wake_triggered()'s comment for why it's checked only
     * here, not folded into real_input_active(). Applies to both states
     * equally -- same underlying "not fully awake" concern. */
    if (hall_depth_wake_triggered()) {
        s_last_activity_ms = now_ms;
        exit_standby();
        return;
    }

    if (s_state == TILES_STANDBY_STATE_POWER_SAVING) {
        if (now_ms - s_last_frame_ms >= TILES_STANDBY_FRAME_INTERVAL_MS) {
            render_power_saving_frame(now_ms);
            s_last_frame_ms = now_ms;
        }
        return;
    }

    /* STATE_STANDBY */
    if (now_ms - s_last_activity_ms >= TILES_STANDBY_POWER_SAVING_TIMEOUT_MS) {
        enter_power_saving();
        return;
    }

    if (now_ms - s_animation_switch_ms >= TILES_STANDBY_ANIMATION_CYCLE_MS) {
        /* Random, not sequential -- excludes both the current animation
         * and the one before it, so a switch never immediately repeats
         * itself and never bounces straight back to the animation two
         * ago either. */
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

bool tiles_standby_is_power_saving(void) {
    return s_state == TILES_STANDBY_STATE_POWER_SAVING;
}
