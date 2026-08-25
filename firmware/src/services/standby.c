#include "standby.h"

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

#define GRID_MIN_ROW 0u /* function buttons */
#define GRID_MAX_ROW 4u /* pad row 4, the bottom row */
#define GRID_MIN_COL 1u
#define GRID_MAX_COL 6u

/* logical_pad = (row-1)*6 + col for row 1-4, col 1-6 -- the row-major
 * layout pad_config.c documents and its table's literal data confirms,
 * not re-derived via a table lookup here since it's a fixed invariant
 * of this board revision. */
static uint8_t pad_for_row_col(uint8_t row, uint8_t col) {
    return (uint8_t)((row - 1u) * 6u + col);
}

/* Column -> function button id. SW1-SW6 sit left-to-right directly
 * above pad columns 1-6 (docs/hardware/SENTIA_TILES_FIRMWARE_HANDOFF.md's
 * button x_mm order, already verified against services/buttons.c's own
 * comment) -- a clean 1:1 mapping, so button id == col here. */
static uint8_t button_for_col(uint8_t col) {
    return col;
}

/* The 4 underglow pixels' logical grid anchors, in SK6805 chain order
 * (LED1->LED2->LED3->LED4 per the board map) -- "under pad 3, pad 5,
 * pad 15, pad 17" per the user's description of the physical board,
 * not something documented in docs/hardware/ (confirmed absent there).
 * Each anchor just reuses that pad's (row, col) as a sample point in
 * the same brightness field the pad grid uses, so a wave/ripple/etc.
 * that reaches pad 3 or pad 5 reaches the matching underglow pixel at
 * the same time -- see render_frame(). If the physical LED1-4 order
 * turns out reversed or different once seen lit on real hardware, only
 * this array needs to change. */
typedef struct {
    uint8_t row;
    uint8_t col;
} grid_point_t;

static const grid_point_t s_underglow_anchor[4] = {
    {1u, 3u}, /* under pad 3 */
    {1u, 5u}, /* under pad 5 */
    {3u, 3u}, /* under pad 15 */
    {3u, 5u}, /* under pad 17 */
};

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
    star->col = (uint8_t)(GRID_MIN_COL + (uint8_t)(rand() % (GRID_MAX_COL - GRID_MIN_COL + 1u)));
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

        if (head_row > (float)GRID_MAX_ROW + star->tail_rows) {
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
    uint8_t col0 = (uint8_t)(col - GRID_MIN_COL); /* 0-5 */
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

/* ---- Animation registry + shared render -------------------------------- */

typedef tiles_standby_color_t (*field_fn_t)(uint8_t row, uint8_t col, uint32_t now_ms);

static const field_fn_t s_animations[] = {
    anim_wave, anim_glow, anim_shooting_stars, anim_snake, anim_rgb_showcase,
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

    for (uint8_t col = GRID_MIN_COL; col <= GRID_MAX_COL; col++) {
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
        tiles_buttons_set_standby_led(button_for_col(col), luminance * BUTTON_STANDBY_BRIGHTNESS_SCALE);
    }
    for (uint8_t row = 1u; row <= GRID_MAX_ROW; row++) {
        for (uint8_t col = GRID_MIN_COL; col <= GRID_MAX_COL; col++) {
            tiles_standby_color_t c = field(row, col, now_ms);
            tiles_lighting_set_standby_pad_rgb(pad_for_row_col(row, col), c.r, c.g, c.b);
        }
    }
    for (uint8_t i = 0; i < 4u; i++) {
        tiles_standby_color_t c = field(s_underglow_anchor[i].row, s_underglow_anchor[i].col, now_ms);
        tiles_lighting_set_standby_underglow_rgb(i, c.r, c.g, c.b);
    }
}

/* ---- State machine -------------------------------------------------- */

typedef enum {
    TILES_STANDBY_STATE_AWAKE = 0,
    TILES_STANDBY_STATE_STANDBY,
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

static void exit_standby(void) {
    s_state = TILES_STANDBY_STATE_AWAKE;
    tiles_lighting_set_standby_active(false);
    tiles_buttons_set_standby_active(false);
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
        if (s_state == TILES_STANDBY_STATE_STANDBY) {
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

    /* STATE_STANDBY: real_input_active() above was false, so the only
     * remaining wake path is Hall depth -- see
     * hall_depth_wake_triggered()'s comment for why it's checked only
     * here, not folded into real_input_active(). */
    if (hall_depth_wake_triggered()) {
        s_last_activity_ms = now_ms;
        exit_standby();
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
