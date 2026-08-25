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

/* Hall-depth wake fallback: on real hardware, touching a pad during the
 * standby animation was observed to NOT reliably wake it, even though
 * MPR121 touch wakes it fine outside of standby and buttons/pedal wake
 * it fine during standby too -- pointing at the animation itself (most
 * likely the pad-LED SK6805 chain, continuously rewritten at ~25fps
 * while idle, vs. only writing reactively on a touch change during
 * normal play) interfering specifically with capacitive touch sensing.
 * Root cause isn't confirmed -- Hall (magnetic, not capacitive) isn't
 * subject to whatever that is, so it's used here as an independent
 * second path to detect a real press regardless of why touch alone
 * isn't enough. Matches this codebase's existing stance that touch
 * alone is foolable and Hall should be fused with it (see hall.h /
 * expression.c). PLACEHOLDER, unmeasured: expression.c's own
 * DEPTH_TO_AFTERTOUCH_FULL_SCALE (2000, also unmeasured) is "full
 * press" depth; this is a coarse fraction of that meant to catch even a
 * light press, not tuned against real noise floor.
 *
 * IMPORTANT: only ever check this while ALREADY in standby (see
 * hall_depth_wake_triggered() below) -- the first version of this fix
 * folded it into the same "is anything active" check used to decide
 * whether to enter standby at all, which broke standby entirely
 * (real symptom: it never triggered). hall.c's depth has no baseline
 * drift compensation yet, so treating it as part of the idle condition
 * meant any pad's ambient noise/drift could permanently look "active"
 * and the idle timer would never elapse. */
#define TILES_STANDBY_HALL_WAKE_DEPTH 150u

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

/* ---- Animation 1: wave -------------------------------------------------
 * A traveling sine band, mostly top-to-bottom with a mild diagonal skew
 * (via the col term) so it doesn't read as perfectly flat horizontal
 * bars. */

#define WAVE_LENGTH_ROWS 2.2f
#define WAVE_PERIOD_MS 3000.0f
#define WAVE_DIAGONAL_SKEW 0.12f

static float anim_wave(uint8_t row, uint8_t col, uint32_t now_ms) {
    float phase = (float)row / WAVE_LENGTH_ROWS + (float)col * WAVE_DIAGONAL_SKEW -
                  (float)now_ms / WAVE_PERIOD_MS;
    return clamp01(0.5f + 0.5f * sinf(2.0f * TILES_STANDBY_PI * phase));
}

/* ---- Animation 2: glow center-out --------------------------------------
 * A ring pulsing outward from the grid's visual center, repeating. */

#define GLOW_CENTER_ROW 2.0f
#define GLOW_CENTER_COL 3.5f
#define GLOW_MAX_RADIUS 4.2f
#define GLOW_PULSE_PERIOD_MS 2500.0f
#define GLOW_RING_WIDTH 1.1f

static float anim_glow(uint8_t row, uint8_t col, uint32_t now_ms) {
    float dr = (float)row - GLOW_CENTER_ROW;
    float dc = (float)col - GLOW_CENTER_COL;
    float dist = sqrtf(dr * dr + dc * dc);
    float ring = fmodf((float)now_ms / GLOW_PULSE_PERIOD_MS * GLOW_MAX_RADIUS, GLOW_MAX_RADIUS);
    float delta = dist - ring;
    return clamp01(expf(-(delta * delta) / (2.0f * GLOW_RING_WIDTH * GLOW_RING_WIDTH)));
}

/* ---- Animation 3: shooting stars ----------------------------------------
 * A handful of comet-tailed points falling top (above row 0) to bottom
 * (past row 4), each respawning at a random column once it exits. The
 * only stateful animation here -- the others are pure functions of
 * (row, col, time); this one owns a small fixed-size particle array. */

#define NUM_STARS 3u
#define STAR_TAIL_ROWS 3.0f
#define STAR_SPEED_ROWS_PER_MS (1.0f / 220.0f) /* one row every 220ms */

typedef struct {
    uint8_t col;
    uint32_t spawn_ms;
} star_t;

static star_t s_stars[NUM_STARS];
static bool s_stars_inited;

static void star_respawn(star_t *star, uint32_t spawn_ms) {
    star->col = (uint8_t)(GRID_MIN_COL + (uint8_t)(rand() % (GRID_MAX_COL - GRID_MIN_COL + 1u)));
    star->spawn_ms = spawn_ms;
}

static float anim_shooting_stars(uint8_t row, uint8_t col, uint32_t now_ms) {
    if (!s_stars_inited) {
        for (uint8_t i = 0; i < NUM_STARS; i++) {
            /* Stagger initial spawn times so all three don't fall in
             * lockstep the first time this animation is shown. */
            star_respawn(&s_stars[i], now_ms - (uint32_t)i * 900u);
        }
        s_stars_inited = true;
    }

    float brightness = 0.0f;
    for (uint8_t i = 0; i < NUM_STARS; i++) {
        star_t *star = &s_stars[i];
        float head_row = (float)(now_ms - star->spawn_ms) * STAR_SPEED_ROWS_PER_MS - STAR_TAIL_ROWS;

        if (head_row > (float)GRID_MAX_ROW + STAR_TAIL_ROWS) {
            star_respawn(star, now_ms);
            continue;
        }
        if (star->col != col) {
            continue;
        }

        float behind = head_row - (float)row;
        if (behind >= 0.0f && behind <= STAR_TAIL_ROWS) {
            float b = 1.0f - behind / STAR_TAIL_ROWS;
            if (b > brightness) {
                brightness = b;
            }
        }
    }
    return clamp01(brightness);
}

/* ---- Animation 4: snake --------------------------------------------------
 * A fixed-length lit segment crawling along a deterministic boustrophedon
 * (serpentine) path that visits every button and pad cell once, then
 * wraps -- classic snake movement without needing actual game state
 * (growth, food, collisions aren't meaningful for a lighting pattern). */

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

static float anim_snake(uint8_t row, uint8_t col, uint32_t now_ms) {
    uint32_t head = (now_ms / SNAKE_STEP_MS) % SNAKE_PATH_LENGTH;
    uint8_t p = snake_path_index(row, col);
    uint32_t behind = (head + SNAKE_PATH_LENGTH - p) % SNAKE_PATH_LENGTH;
    if (behind >= SNAKE_TAIL_LENGTH) {
        return 0.0f;
    }
    return clamp01(1.0f - (float)behind / (float)SNAKE_TAIL_LENGTH);
}

/* ---- Animation registry + shared render -------------------------------- */

typedef float (*field_fn_t)(uint8_t row, uint8_t col, uint32_t now_ms);

static const field_fn_t s_animations[] = {
    anim_wave,
    anim_glow,
    anim_shooting_stars,
    anim_snake,
};
#define NUM_ANIMATIONS ((uint8_t)(sizeof(s_animations) / sizeof(s_animations[0])))

static void render_frame(field_fn_t field, uint32_t now_ms) {
    for (uint8_t col = GRID_MIN_COL; col <= GRID_MAX_COL; col++) {
        float b = field(0u, col, now_ms);
        tiles_buttons_set_standby_led(button_for_col(col), b);
    }
    for (uint8_t row = 1u; row <= GRID_MAX_ROW; row++) {
        for (uint8_t col = GRID_MIN_COL; col <= GRID_MAX_COL; col++) {
            float b = field(row, col, now_ms);
            tiles_lighting_set_standby_pad(pad_for_row_col(row, col), b);
        }
    }
    for (uint8_t i = 0; i < 4u; i++) {
        float b = field(s_underglow_anchor[i].row, s_underglow_anchor[i].col, now_ms);
        tiles_lighting_set_standby_underglow(i, b);
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
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        if (tiles_hall_get_depth(pad) >= TILES_STANDBY_HALL_WAKE_DEPTH) {
            return true;
        }
    }
    return false;
}

static void enter_standby(uint32_t now_ms) {
    s_state = TILES_STANDBY_STATE_STANDBY;
    s_animation_index = 0u;
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
    s_animation_index = 0u;
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
        s_animation_index = (uint8_t)((s_animation_index + 1u) % NUM_ANIMATIONS);
        s_animation_switch_ms = now_ms;
    }

    if (now_ms - s_last_frame_ms >= TILES_STANDBY_FRAME_INTERVAL_MS) {
        render_frame(s_animations[s_animation_index], now_ms);
        s_last_frame_ms = now_ms;
    }
}

bool tiles_standby_is_active(void) {
    return s_state == TILES_STANDBY_STATE_STANDBY;
}
