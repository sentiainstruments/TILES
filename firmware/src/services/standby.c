#include "standby.h"

#include "board_layout.h"
#include "board_pins.h"
#include "buttons.h"
#include "hall.h"
#include "lighting.h"
#include "pedal.h"
#include "pixel_font.h"
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
/* Roughly half the original speed (denominators doubled) -- real
 * feedback that the fall read as too fast. */
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
 * An actual game of snake, not just a segment crawling a fixed path:
 * a pulsing red food dot appears somewhere on the grid, the snake moves
 * toward it a cell at a time, eats it (grows by one segment, a new dot
 * appears), and keeps going. Movement is greedy-toward-the-food with a
 * randomized perturbation each step (snake_step() below) so the path
 * varies run to run instead of always taking the same route -- and the
 * snake resets to a short length at a randomized start position/
 * direction whenever it grows too long or traps itself with nowhere
 * left to go, so it doesn't settle into one repeating pattern long-term
 * either. Reworked from an earlier version that was just a fixed-length
 * segment ping-ponging a deterministic path -- real feedback was that
 * it didn't feel like an actual game of snake. */

#define SNAKE_STEP_MS 300u
#define SNAKE_MAX_LENGTH 14u /* reset threshold -- well under the 30-cell grid */
/* How strongly a step's direction choice gets perturbed away from the
 * purely greedy (shortest-distance-to-food) choice, in the same units
 * as cell distance -- higher means more wandering/varied paths, lower
 * means more direct pathing to the food. Unmeasured, a starting guess. */
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
    /* Small board (30 cells), snake usually much shorter -- a bounded
     * rejection-sampling loop finds an empty cell almost immediately in
     * practice. The fallback after MAX_ATTEMPTS (place it somewhere
     * regardless of overlap) only matters if the snake is nearly
     * filling the board, in which case a reset is imminent anyway. */
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

/* Randomized start position/direction, short length -- "game over" (too
 * long, or nowhere left to move) and the very first call both land
 * here, so every run starts looking different. */
static void snake_reset(uint32_t now_ms) {
    static const int8_t dir_row[4] = {-1, 1, 0, 0};
    static const int8_t dir_col[4] = {0, 0, -1, 1};

    int8_t start_row = (int8_t)(1 + rand() % (int)TILES_GRID_MAX_ROW); /* rows 1-4 */
    int8_t start_col = (int8_t)(TILES_GRID_MIN_COL + rand() % (TILES_GRID_MAX_COL - TILES_GRID_MIN_COL + 1u));
    uint8_t dir_index = (uint8_t)(rand() % 4u);
    int8_t dr = dir_row[dir_index];
    int8_t dc = dir_col[dir_index];

    /* 2, not 3 -- real feedback (from the interactive version in
     * game_mode.c, same concern applies here) that 3 felt cramped given
     * how little space this board actually has, and with a randomized
     * start position here a longer starting length also meant more
     * segments could land clamped/bunched at a boundary. */
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
        /* Boxed in with nowhere to go -- "game over", start fresh. */
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
 * Each column is a fake VU/EQ bar lit bottom-up -- rows 3-4 (the bottom
 * two) blue, row 2 yellow, row 1 (top) red, like a classic hardware
 * graphic equalizer. A per-column "redline" peak marker sticks at the
 * highest segment reached and only falls slowly (EQ_PEAK_DECAY_PER_MS),
 * independent of the bar's own motion -- the peak-hold behavior real
 * EQ/VU hardware has.
 *
 * Reworked twice from real feedback. First pass slowed everything down
 * and added a long, low-biased "phrase" envelope so columns spent real
 * stretches fully dark -- but that overshot: it read as too slow, and
 * with bars rarely reaching full height there were "almost no peaks."
 * Second pass replaced the free-running sine motion with a percussive,
 * tempo-locked hit envelope: every column snaps to full height at its
 * own hit (an instant/near-instant attack, like a kick meter) and decays
 * afterward, so peaks happen reliably instead of by chance. Every
 * column's hit rate is a whole-number subdivision of one shared 127bpm
 * beat (EQ_BEAT_MS) -- bass-register columns hit once a beat and ring
 * out slowly, treble-register columns subdivide down to 16th notes and
 * snap back fast -- so the whole grid still reads as reacting to one
 * underlying pulse ("should feel like a song at 127bpm pumping") while
 * different bars move at different rates, the way a real spectrum
 * analyzer's bands do. A deterministic per-hit "miss" (golden-angle
 * stepping, not real randomness) occasionally drops a hit to 0 so there
 * is still some empty space between hits, without the old envelope's
 * multi-second silences that killed density. Function buttons are fully
 * off (were a faint minimal glow); underglow is a simple constant blue
 * accent (see eq_underglow below) rather than tracking the bars --
 * doesn't correspond to any one column, so there's nothing meaningful
 * for it to track. */

#define EQ_NUM_COLS 6u
#define EQ_LIT_LEVEL 0.85f
#define EQ_PEAK_LEVEL 1.0f
#define EQ_UNDERGLOW_LEVEL 0.7f
/* One quarter note at 127bpm -- the shared pulse every column's hit rate
 * subdivides. */
#define EQ_BEAT_MS 472.0f
/* Fraction of hits that get deterministically dropped to 0 -- "some
 * empty space" without a multi-second silent stretch. */
#define EQ_MISS_FRACTION 0.12f
/* Peaks now happen every beat/subdivision, so the hold needs to actually
 * fall between hits to still read as movement rather than a
 * permanently-lit top segment -- full fall takes a bit over 2 beats. */
#define EQ_PEAK_DECAY_PER_MS (1.0f / 1200.0f)

/* Per-column hit rate, in hits per beat -- pairs of columns share a rate
 * (low/low/mid/mid/high/high) so the six bars still read left-to-right
 * as low to high register, like a real EQ's frequency axis. */
static const float s_eq_col_hits_per_beat[EQ_NUM_COLS] = {1.0f, 1.0f, 2.0f, 2.0f, 4.0f, 4.0f};
/* Per-column decay shape for the percussive envelope -- lower (slower
 * subdivisions / bass) rings out over more of its hit window; higher
 * (faster subdivisions / treble) snaps back almost immediately. */
static const float s_eq_col_decay_exp[EQ_NUM_COLS] = {1.5f, 1.5f, 1.0f, 1.0f, 0.6f, 0.6f};

static float s_eq_peak[EQ_NUM_COLS];
static uint32_t s_eq_peak_update_ms[EQ_NUM_COLS];
static bool s_eq_inited;

/* Deterministic function of (col, time): a percussive envelope --
 * instant peak at the start of each hit window, decaying across it --
 * tempo-locked to that column's own subdivision of EQ_BEAT_MS, plus a
 * deterministic occasional full-miss for some breathing room between
 * hits. No randomness/state needed for the bar itself, only for the
 * peak-hold below. */
static float eq_bar_level(uint8_t col, uint32_t now_ms) {
    uint8_t i = (uint8_t)(col - TILES_GRID_MIN_COL);
    float hit_period = EQ_BEAT_MS / s_eq_col_hits_per_beat[i];
    float hit_index = floorf((float)now_ms / hit_period);
    float phase = ((float)now_ms - hit_index * hit_period) / hit_period; /* 0 at the hit, ->1 before the next */

    /* Golden-angle stepping keyed on which hit this is -- a cheap,
     * deterministic stand-in for randomness that still spreads misses
     * evenly across hits instead of clustering them. */
    float miss_key = fmodf(hit_index * 0.6180339887f + (float)i * 0.37f, 1.0f);
    if (miss_key < EQ_MISS_FRACTION) {
        return 0.0f;
    }

    float envelope = powf(1.0f - phase, s_eq_col_decay_exp[i]);
    /* Per-hit velocity variance (same golden-angle trick, different
     * offset) so hits that do land aren't all identically full-height --
     * still real dynamic range without needing every hit to be a peak. */
    float velocity_key = fmodf(hit_index * 0.6180339887f + (float)i * 0.37f + 0.5f, 1.0f);
    float velocity = 0.55f + 0.45f * velocity_key;
    return clamp01(envelope * velocity);
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

/* ---- Animation 8: brick breaker -------------------------------------------
 * The function-button row is the wall of bricks; a 3-pad-wide paddle
 * (bottom pad row) tracks the ball (simple AI: move at most one column
 * per step toward the ball's current column); the ball bounces around
 * knocking bricks out until either every brick is broken (won) or the
 * ball gets past the paddle (lost) -- either way, underglow flashes red
 * and purple for a few seconds, then a fresh round starts (bricks
 * restored, ball and paddle reset). Ball and paddle are different
 * colors so they read as distinct objects even mid-bounce, when they
 * briefly overlap. */

#define BB_NUM_COLS 6u
#define BB_PADDLE_ROW 4u
#define BB_STEP_MS 350u
#define BB_FLASH_DURATION_MS 2200u
#define BB_FLASH_TOGGLE_MS 260u
#define BB_BRICK_LEVEL 0.85f
#define BB_PADDLE_LEVEL 0.9f
#define BB_BALL_LEVEL 1.0f

typedef enum {
    BB_PHASE_PLAYING = 0,
    BB_PHASE_ROUND_END,
} bb_phase_t;

static bool s_bb_brick_alive[BB_NUM_COLS];
static int8_t s_bb_ball_row;
static int8_t s_bb_ball_col;
static int8_t s_bb_ball_drow;
static int8_t s_bb_ball_dcol;
static int8_t s_bb_paddle_center; /* 2-5 -- paddle spans center-1..center+1 */
static bb_phase_t s_bb_phase;
static uint32_t s_bb_last_step_ms;
static uint32_t s_bb_round_end_ms;
static bool s_bb_inited;

static void bb_new_round(uint32_t now_ms) {
    for (uint8_t i = 0; i < BB_NUM_COLS; i++) {
        s_bb_brick_alive[i] = true;
    }
    s_bb_paddle_center = 3;
    s_bb_ball_row = (int8_t)(BB_PADDLE_ROW - 1u);
    s_bb_ball_col = s_bb_paddle_center;
    s_bb_ball_drow = -1; /* heads up toward the bricks first */
    s_bb_ball_dcol = ((rand() % 2) == 0) ? -1 : 1;
    s_bb_phase = BB_PHASE_PLAYING;
    s_bb_last_step_ms = now_ms;
}

static void bb_step(uint32_t now_ms) {
    int8_t new_col = (int8_t)(s_bb_ball_col + s_bb_ball_dcol);
    if (new_col < (int8_t)TILES_GRID_MIN_COL || new_col > (int8_t)TILES_GRID_MAX_COL) {
        s_bb_ball_dcol = (int8_t)(-s_bb_ball_dcol);
        new_col = (int8_t)(s_bb_ball_col + s_bb_ball_dcol);
    }
    int8_t new_row = (int8_t)(s_bb_ball_row + s_bb_ball_drow);

    if (new_row < 1) {
        /* Hit the brick wall (row 0) -- always bounces here regardless
         * of whether this column's brick is still alive. */
        uint8_t col_index = (uint8_t)(new_col - TILES_GRID_MIN_COL);
        s_bb_brick_alive[col_index] = false;
        s_bb_ball_drow = 1;
        new_row = 1;

        bool all_dead = true;
        for (uint8_t i = 0; i < BB_NUM_COLS; i++) {
            if (s_bb_brick_alive[i]) {
                all_dead = false;
                break;
            }
        }
        if (all_dead) {
            s_bb_phase = BB_PHASE_ROUND_END;
            s_bb_round_end_ms = now_ms;
        }
    } else if (new_row > (int8_t)BB_PADDLE_ROW) {
        int8_t paddle_min = (int8_t)(s_bb_paddle_center - 1);
        int8_t paddle_max = (int8_t)(s_bb_paddle_center + 1);
        if (new_col >= paddle_min && new_col <= paddle_max) {
            s_bb_ball_drow = -1;
            new_row = (int8_t)BB_PADDLE_ROW;
        } else {
            /* Missed -- lost. Ball is left just past the paddle row, off
             * the renderable 0-4 range, so it naturally disappears from
             * view rather than needing an explicit "hide it" case. */
            s_bb_phase = BB_PHASE_ROUND_END;
            s_bb_round_end_ms = now_ms;
        }
    }

    s_bb_ball_col = new_col;
    s_bb_ball_row = new_row;

    if (s_bb_phase == BB_PHASE_PLAYING) {
        if (s_bb_paddle_center < s_bb_ball_col) {
            s_bb_paddle_center++;
        } else if (s_bb_paddle_center > s_bb_ball_col) {
            s_bb_paddle_center--;
        }
        if (s_bb_paddle_center < 2) {
            s_bb_paddle_center = 2;
        }
        if (s_bb_paddle_center > 5) {
            s_bb_paddle_center = 5;
        }
    }
}

static void bb_update(uint32_t now_ms) {
    if (!s_bb_inited) {
        bb_new_round(now_ms);
        s_bb_inited = true;
        return;
    }
    if (s_bb_phase == BB_PHASE_PLAYING) {
        if (now_ms - s_bb_last_step_ms >= BB_STEP_MS) {
            bb_step(now_ms);
            s_bb_last_step_ms = now_ms;
        }
    } else if (now_ms - s_bb_round_end_ms >= BB_FLASH_DURATION_MS) {
        bb_new_round(now_ms);
    }
}

static tiles_standby_color_t anim_brick_breaker(uint8_t row, uint8_t col, uint32_t now_ms) {
    bb_update(now_ms);

    if (row == 0u) {
        uint8_t idx = (uint8_t)(col - TILES_GRID_MIN_COL);
        if (s_bb_brick_alive[idx]) {
            /* Orange bricks -- distinct from both the ball and paddle. */
            tiles_standby_color_t c = {1.0f * BB_BRICK_LEVEL, 0.4f * BB_BRICK_LEVEL, 0.0f};
            return c;
        }
        return white(0.0f);
    }

    if (s_bb_ball_row >= 1 && s_bb_ball_row <= (int8_t)BB_PADDLE_ROW && (int8_t)row == s_bb_ball_row &&
        (int8_t)col == s_bb_ball_col) {
        /* Warm white/yellow ball -- checked before the paddle below so
         * it draws on top during a bounce, when both occupy the same
         * cell. */
        tiles_standby_color_t c = {1.0f * BB_BALL_LEVEL, 1.0f * BB_BALL_LEVEL, 0.4f * BB_BALL_LEVEL};
        return c;
    }

    if (row == BB_PADDLE_ROW) {
        int8_t paddle_min = (int8_t)(s_bb_paddle_center - 1);
        int8_t paddle_max = (int8_t)(s_bb_paddle_center + 1);
        if ((int8_t)col >= paddle_min && (int8_t)col <= paddle_max) {
            /* Cyan paddle. */
            tiles_standby_color_t c = {0.0f, 0.6f * BB_PADDLE_LEVEL, 1.0f * BB_PADDLE_LEVEL};
            return c;
        }
    }

    return white(0.0f);
}

static tiles_standby_color_t bb_underglow(uint8_t pixel_index, uint32_t now_ms) {
    (void)pixel_index;
    if (s_bb_phase != BB_PHASE_ROUND_END) {
        return white(0.0f);
    }
    uint32_t toggle = (now_ms - s_bb_round_end_ms) / BB_FLASH_TOGGLE_MS;
    if ((toggle % 2u) == 0u) {
        tiles_standby_color_t red = {1.0f, 0.0f, 0.0f};
        return red;
    }
    tiles_standby_color_t purple = {0.6f, 0.0f, 1.0f};
    return purple;
}

/* ---- Animation 9: scrolling marquee ---------------------------------------
 * "SENTIA - TILES - " scrolls across the pad grid using
 * services/pixel_font.h's shared 4-row font -- underglow and function
 * buttons both stay off, keeping the whole thing purely a pad-grid text
 * effect. The message is a sequence of glyphs with a blank spacing
 * column automatically inserted after each one, and the whole thing
 * scrolls by indexing into that sequence at a virtual column offset
 * that advances with time and wraps around (marquee_total_width()), so
 * the message repeats seamlessly.
 *
 * Reworked from real feedback that the font itself needed fixing (the
 * glyphs used to live here as a one-off, hand-guessed set -- moved into
 * pixel_font.h/.c so this and services/octave_control.c's transpose key
 * display share one already-checked font instead of each guessing its
 * own) and that the scroll was too fast -- slowed accordingly
 * (MARQUEE_MS_PER_COLUMN). */

#define MARQUEE_NUM_GLYPHS ((uint8_t)(sizeof(s_marquee_message) / sizeof(s_marquee_message[0])))
#define MARQUEE_GLYPH_GAP 1u
#define MARQUEE_MS_PER_COLUMN 420u
#define MARQUEE_LIT_LEVEL 0.9f

/* Pointers, not struct copies -- TILES_GLYPH_* are extern objects
 * defined in pixel_font.c, and an extern object's value (as opposed to
 * its address) isn't a compile-time constant to this translation unit,
 * so a static initializer can't copy the struct by value. */
static const tiles_glyph_t *const s_marquee_message[] = {
    &TILES_GLYPH_S,     &TILES_GLYPH_E, &TILES_GLYPH_N, &TILES_GLYPH_T, &TILES_GLYPH_I, &TILES_GLYPH_A,
    &TILES_GLYPH_SPACE, &TILES_GLYPH_DASH, &TILES_GLYPH_SPACE,
    &TILES_GLYPH_T,     &TILES_GLYPH_I, &TILES_GLYPH_L, &TILES_GLYPH_E, &TILES_GLYPH_S,
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
            return 0u; /* the trailing spacing column */
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

/* ---- Animation 10: bouncing glow -----------------------------------------
 * A single soft white point bounces diagonally around the pad grid,
 * reflecting off the edges like a screensaver ball -- deliberately the
 * "simple but elegant" one: no particle array, no game state, just a
 * closed-form position (a triangle wave per axis, which is a bounce-off-
 * the-walls reflection with zero bookkeeping) and a soft Gaussian-ish
 * falloff around it. Row and col bounce at different, non-integer-ratio
 * periods so the path slowly traces out a Lissajous-like figure instead
 * of repeating quickly. Function buttons stay off for a clean, minimal
 * look; underglow mirrors the pad field (NULL override below) so the
 * glow naturally spills into it when the point passes near an anchor,
 * same as animations 1-4. */

#define BOUNCE_ROW_PERIOD_MS 5200.0f /* one full row min->max->min traversal */
#define BOUNCE_COL_PERIOD_MS 6700.0f /* deliberately not a small-integer ratio of the row period */
#define BOUNCE_RADIUS_CELLS 1.5f
#define BOUNCE_PEAK_LEVEL 0.9f

/* Triangle wave 0->1->0 over one period, then scaled/offset into
 * [min_val, max_val] -- a closed-form bounce-off-the-walls reflection,
 * no velocity/state needed. */
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

/* ---- Animation 11: Tetris --------------------------------------------------
 * Standard tetromino set, AI-placed and AI-played -- the autonomous
 * counterpart to game_mode.c's real, player-controlled Tetris (same
 * piece shapes/colors, deliberately separate state and code, per this
 * file's established precedent of not sharing anything between the
 * idle-loop and player-driven versions of the same game -- see
 * animations 4 and 8 above). A lightweight greedy AI picks each new
 * piece's rotation and column immediately at spawn: of every
 * (rotation, column) combination that fits, it simulates the drop and
 * keeps whichever lands the piece's topmost cell deepest (a cheap proxy
 * for "keeps the resulting stack lowest," without real hole-counting).
 * The piece then visibly falls one row at a time toward that chosen
 * landing spot, the same step-throttle pattern as every other stateful
 * animation here. Topping out (nowhere for a freshly spawned piece to
 * fit) triggers the same red/purple round-end flash brick breaker uses,
 * then the well clears and a fresh game starts. Function buttons stay
 * off. */

#define TETRIS_MIN_ROW 1u /* row 0 is buttons, not part of the well */
#define TETRIS_MAX_ROW TILES_GRID_MAX_ROW
#define TETRIS_MIN_COL TILES_GRID_MIN_COL
#define TETRIS_MAX_COL TILES_GRID_MAX_COL
#define TETRIS_ROWS 4u
#define TETRIS_COLS 6u
#define TETRIS_STEP_MS 260u /* faster than the interactive version -- nothing here waits on a player */
#define TETRIS_LOCKED_LEVEL 0.8f
#define TETRIS_FLASH_DURATION_MS 2200u
#define TETRIS_FLASH_TOGGLE_MS 260u
/* Dramatic white underglow strobe on a line clear -- fast toggle, short
 * total duration, so it reads as a flash rather than a glow. */
#define TETRIS_LINE_CLEAR_FLASH_MS 450u
#define TETRIS_LINE_CLEAR_TOGGLE_MS 90u
#define TETRIS_NUM_PIECE_TYPES 7u

typedef struct {
    int8_t dr;
    int8_t dc;
} tetris_offset_t;

/* Two rotation states per piece (not full 4-state SRS) -- with only 4
 * rows of height the extra states would rarely matter. Classic piece
 * colors, classic piece order (I,O,T,S,Z,J,L). */
typedef struct {
    tetris_offset_t state0[4];
    tetris_offset_t state1[4];
    float r, g, b;
} tetris_piece_def_t;

static const tetris_piece_def_t TETRIS_PIECES[TETRIS_NUM_PIECE_TYPES] = {
    {{{0, 0}, {0, 1}, {0, 2}, {0, 3}}, {{0, 0}, {1, 0}, {2, 0}, {3, 0}}, 0.0f, 1.0f, 1.0f}, /* I: cyan */
    {{{0, 0}, {0, 1}, {1, 0}, {1, 1}}, {{0, 0}, {0, 1}, {1, 0}, {1, 1}}, 1.0f, 1.0f, 0.0f}, /* O: yellow */
    {{{0, 0}, {0, 1}, {0, 2}, {1, 1}}, {{0, 0}, {1, 0}, {2, 0}, {1, 1}}, 0.6f, 0.0f, 1.0f}, /* T: purple */
    {{{0, 1}, {0, 2}, {1, 0}, {1, 1}}, {{0, 0}, {1, 0}, {1, 1}, {2, 1}}, 0.0f, 1.0f, 0.0f}, /* S: green */
    {{{0, 0}, {0, 1}, {1, 1}, {1, 2}}, {{0, 1}, {1, 0}, {1, 1}, {2, 0}}, 1.0f, 0.0f, 0.0f}, /* Z: red */
    {{{0, 0}, {1, 0}, {1, 1}, {1, 2}}, {{0, 0}, {0, 1}, {1, 0}, {2, 0}}, 0.0f, 0.0f, 1.0f}, /* J: blue */
    {{{0, 2}, {1, 0}, {1, 1}, {1, 2}}, {{0, 0}, {1, 0}, {2, 0}, {2, 1}}, 1.0f, 0.5f, 0.0f}, /* L: orange */
};

typedef enum {
    TETRIS_PHASE_PLAYING = 0,
    TETRIS_PHASE_ROUND_END,
} tetris_phase_t;

/* 0 = empty, else (piece type index + 1) -- indexed [row - TETRIS_MIN_ROW][col - TETRIS_MIN_COL]. */
static uint8_t s_tetris_board[TETRIS_ROWS][TETRIS_COLS];
static uint8_t s_tetris_piece_type;
static uint8_t s_tetris_rotation;
static int8_t s_tetris_origin_row;
static int8_t s_tetris_origin_col;
static int8_t s_tetris_target_row; /* the AI's chosen landing row for the current piece */
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
    for (uint8_t i = 0; i < 4u; i++) {
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

/* Simulates dropping (piece_type, rotation, origin_col) from the top of
 * the well and returns the row it would land at (the deepest row it
 * still fits), or false if it doesn't even fit at spawn height for that
 * column. */
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

/* Greedy placement AI -- see the animation's file comment above. */
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
            int8_t min_row = (int8_t)(landing_row + offsets[0].dr);
            for (uint8_t i = 1; i < 4u; i++) {
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

/* Same bottom-up, recheck-same-row-after-a-shift sweep as
 * game_mode.c's gt_clear_lines() -- correctly collapses multiple
 * simultaneous line clears in one pass. Returns how many rows were
 * cleared, so tetris_lock() below can trigger the line-clear flash only
 * when something actually cleared. */
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
    /* "Already long past" rather than 0 -- 0 could still read as
     * "within the flash window" if this round starts within
     * TETRIS_LINE_CLEAR_FLASH_MS of boot. */
    s_tetris_line_clear_flash_ms = now_ms - TETRIS_LINE_CLEAR_FLASH_MS - 1u;
}

static void tetris_lock(uint32_t now_ms) {
    const tetris_offset_t *offsets = tetris_offsets(s_tetris_piece_type, s_tetris_rotation);
    for (uint8_t i = 0; i < 4u; i++) {
        int8_t r = (int8_t)(s_tetris_origin_row + offsets[i].dr);
        int8_t c = (int8_t)(s_tetris_origin_col + offsets[i].dc);
        s_tetris_board[r - (int8_t)TETRIS_MIN_ROW][c - (int8_t)TETRIS_MIN_COL] = (uint8_t)(s_tetris_piece_type + 1u);
    }
    if (tetris_clear_lines() > 0u) {
        s_tetris_line_clear_flash_ms = now_ms;
    }
    tetris_spawn();
    if (!tetris_fits(s_tetris_piece_type, s_tetris_rotation, s_tetris_origin_row, s_tetris_origin_col)) {
        /* Nowhere for the next piece to go -- topped out. */
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
        for (uint8_t i = 0; i < 4u; i++) {
            int8_t r = (int8_t)(s_tetris_origin_row + offsets[i].dr);
            int8_t c = (int8_t)(s_tetris_origin_col + offsets[i].dc);
            if (r == (int8_t)row && c == (int8_t)col) {
                /* The falling piece draws at full brightness, checked
                 * before the locked board below so it's never masked by
                 * whatever is already in that cell. */
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

/* Topping out (game lost) blinks plain red -- real feedback: "when game
 * is lost it should flash red," not the red/purple alternation
 * brick breaker's win/lose flash uses. A line clear (still playing) is
 * a separate, much shorter dramatic white strobe instead. */
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

/* ---- Animation 12: Pong ----------------------------------------------------
 * The AI-vs-AI autonomous counterpart to game_mode.c's real, two-player
 * Pong -- same court/paddle/ball layout and colors, deliberately
 * separate state and code (same precedent as every other animation/
 * game pair here). Both paddles use the same simple "move at most one
 * row per step toward the ball" heuristic brick breaker's paddle AI
 * already established, so rallies essentially never end on their own; a
 * miss (on the rare occasion the ball reverses faster than a paddle can
 * react) triggers a brief white underglow flash and an immediate
 * re-serve, the same "stay in this animation, just flash and continue"
 * behavior the interactive version uses instead of a win/lose
 * round-end. Function buttons stay off. */

#define PONG_MIN_ROW 1u /* row 0 is buttons, not part of the court */
#define PONG_MAX_ROW TILES_GRID_MAX_ROW
#define PONG_PADDLE_COL_LEFT TILES_GRID_MIN_COL
#define PONG_PADDLE_COL_RIGHT TILES_GRID_MAX_COL
#define PONG_PADDLE_TOP_MIN PONG_MIN_ROW /* paddle spans [top, top+1] */
#define PONG_PADDLE_TOP_MAX (TILES_GRID_MAX_ROW - 1u)
#define PONG_STEP_MS 220u /* faster than the interactive version -- nothing here waits on a player */
#define PONG_POINT_FLASH_MS 500u
#define PONG_POINT_FLASH_TOGGLE_MS 110u
#define PONG_PADDLE_LEVEL 1.0f
#define PONG_BALL_LEVEL 1.0f

static int8_t s_pong_left_paddle_top;
static int8_t s_pong_right_paddle_top;
static int8_t s_pong_ball_row;
static int8_t s_pong_ball_col;
static int8_t s_pong_ball_drow;
static int8_t s_pong_ball_dcol;
static uint32_t s_pong_last_step_ms;
static uint32_t s_pong_point_flash_ms;
static bool s_pong_inited;

static void pong_serve(uint32_t now_ms) {
    s_pong_ball_row = (int8_t)(PONG_MIN_ROW + (rand() % (PONG_MAX_ROW - PONG_MIN_ROW + 1u)));
    s_pong_ball_col = ((rand() % 2) == 0) ? 3 : 4; /* the two middle columns of 1-6 */
    s_pong_ball_drow = ((rand() % 2) == 0) ? -1 : 1;
    s_pong_ball_dcol = ((rand() % 2) == 0) ? -1 : 1;
    s_pong_last_step_ms = now_ms;
}

static void pong_new_round(uint32_t now_ms) {
    s_pong_left_paddle_top = 2;
    s_pong_right_paddle_top = 2;
    pong_serve(now_ms);
    /* "Already long past" rather than 0 -- same reasoning as Tetris's
     * line-clear flash above. */
    s_pong_point_flash_ms = now_ms - PONG_POINT_FLASH_MS - 1u;
}

/* Moves *paddle_top by at most one row toward covering the ball's
 * current row -- the same "at most one column/row per step" simple AI
 * anim_brick_breaker's paddle already uses. */
static void pong_ai_track(int8_t *paddle_top) {
    if (s_pong_ball_row < *paddle_top) {
        (*paddle_top)--;
    } else if (s_pong_ball_row > (int8_t)(*paddle_top + 1)) {
        (*paddle_top)++;
    }
    if (*paddle_top < (int8_t)PONG_PADDLE_TOP_MIN) {
        *paddle_top = (int8_t)PONG_PADDLE_TOP_MIN;
    }
    if (*paddle_top > (int8_t)PONG_PADDLE_TOP_MAX) {
        *paddle_top = (int8_t)PONG_PADDLE_TOP_MAX;
    }
}

static void pong_step(uint32_t now_ms) {
    int8_t new_row = (int8_t)(s_pong_ball_row + s_pong_ball_drow);
    if (new_row < (int8_t)PONG_MIN_ROW || new_row > (int8_t)PONG_MAX_ROW) {
        s_pong_ball_drow = (int8_t)(-s_pong_ball_drow);
        new_row = (int8_t)(s_pong_ball_row + s_pong_ball_drow);
    }

    int8_t new_col = (int8_t)(s_pong_ball_col + s_pong_ball_dcol);
    if (new_col < (int8_t)PONG_PADDLE_COL_LEFT) {
        if (new_row >= s_pong_left_paddle_top && new_row <= (int8_t)(s_pong_left_paddle_top + 1)) {
            s_pong_ball_dcol = 1;
            new_col = (int8_t)PONG_PADDLE_COL_LEFT;
        } else {
            s_pong_point_flash_ms = now_ms;
            pong_serve(now_ms);
            return;
        }
    } else if (new_col > (int8_t)PONG_PADDLE_COL_RIGHT) {
        if (new_row >= s_pong_right_paddle_top && new_row <= (int8_t)(s_pong_right_paddle_top + 1)) {
            s_pong_ball_dcol = -1;
            new_col = (int8_t)PONG_PADDLE_COL_RIGHT;
        } else {
            s_pong_point_flash_ms = now_ms;
            pong_serve(now_ms);
            return;
        }
    }

    s_pong_ball_row = new_row;
    s_pong_ball_col = new_col;

    pong_ai_track(&s_pong_left_paddle_top);
    pong_ai_track(&s_pong_right_paddle_top);
}

static void pong_update(uint32_t now_ms) {
    if (!s_pong_inited) {
        pong_new_round(now_ms);
        s_pong_inited = true;
        return;
    }
    if (now_ms - s_pong_last_step_ms < PONG_STEP_MS) {
        return;
    }
    s_pong_last_step_ms = now_ms;
    pong_step(now_ms);
}

static tiles_standby_color_t anim_pong(uint8_t row, uint8_t col, uint32_t now_ms) {
    pong_update(now_ms);

    if (row == 0u) {
        return white(0.0f);
    }

    if ((int8_t)row == s_pong_ball_row && (int8_t)col == s_pong_ball_col) {
        /* Checked before either paddle so it draws on top during a
         * bounce -- same precedent as brick breaker's ball. */
        tiles_standby_color_t ball = {0.0f, 0.0f, PONG_BALL_LEVEL};
        return ball;
    }
    if (col == PONG_PADDLE_COL_LEFT && (int8_t)row >= s_pong_left_paddle_top &&
        (int8_t)row <= (int8_t)(s_pong_left_paddle_top + 1)) {
        return white(PONG_PADDLE_LEVEL);
    }
    if (col == PONG_PADDLE_COL_RIGHT && (int8_t)row >= s_pong_right_paddle_top &&
        (int8_t)row <= (int8_t)(s_pong_right_paddle_top + 1)) {
        return white(PONG_PADDLE_LEVEL);
    }
    return white(0.0f);
}

static tiles_standby_color_t pong_underglow(uint8_t pixel_index, uint32_t now_ms) {
    (void)pixel_index;
    if (now_ms - s_pong_point_flash_ms >= PONG_POINT_FLASH_MS) {
        return white(0.0f);
    }
    bool on = (((now_ms - s_pong_point_flash_ms) / PONG_POINT_FLASH_TOGGLE_MS) % 2u) == 0u;
    return white(on ? 1.0f : 0.0f);
}

/* ---- Animation registry + shared render -------------------------------- */

typedef tiles_standby_color_t (*field_fn_t)(uint8_t row, uint8_t col, uint32_t now_ms);
typedef tiles_standby_color_t (*underglow_fn_t)(uint8_t pixel_index, uint32_t now_ms);

static const field_fn_t s_animations[] = {
    anim_wave,           anim_glow,     anim_shooting_stars, anim_snake,
    anim_rgb_showcase,   anim_equalizer, anim_underglow_circle,
    anim_brick_breaker,  anim_marquee,  anim_bounce, anim_tetris, anim_pong,
};
/* Parallel to s_animations[] -- NULL means underglow samples the same
 * field the pads use at its anchor points (animations 1-5 and 10, where
 * underglow mirroring the pad grid is exactly what's wanted). A
 * non-NULL entry means underglow needs genuinely different behavior
 * from whatever the pad field computes at that (row, col) -- the
 * equalizer's underglow is a constant accent unrelated to any one
 * column's bar, the circular-wave animation's whole point is
 * underglow-specific motion indexed by pixel, not by row/col at all,
 * brick breaker's underglow is off except for the won/lost flash, and
 * the marquee's underglow is simply always off. */
static const underglow_fn_t s_animation_underglow_override[] = {
    NULL, NULL, NULL, NULL, NULL, eq_underglow, circle_underglow, bb_underglow, marquee_underglow, NULL,
    tetris_underglow, pong_underglow,
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
