#include "lighting.h"

#include "board_pins.h"
#include "note_map.h"
#include "pad_config.h"
#include "power.h"

#include "sk6805.h"
#include "tca9554.h"

/* Real feedback: "make all led brighter its hard to see" -- raised from
 * 10. Still a fraction of the active brightness ceiling (a power-derived
 * safety cap from tiles_power_get_state(), untouched by this change --
 * see this file's "Dynamic, load-aware pad brightness ceiling" section
 * further below for how that ceiling is now computed), so this only
 * spends more of whatever headroom that ceiling already allows on the
 * resting/idle state, not a change to the underlying power budget. */
#define TILES_LIGHTING_IDLE_BASELINE_PERCENT 25u

/* Idle (untouched) chromatic-play pad coloring by note role -- real
 * feedback: "root should be blue and black keys shouldnt have led this
 * in rest non pressed moment... push should be regular white illumination"
 * (unchanged, see pad_desired_rgb() below, still used whenever a pad
 * IS touched, root or not), then, after a first hardware pass: "make the
 * blue sentia purple for root notes but dim it a bit more than standard
 * non pressed pads." Root now uses Sentia Instruments' own brand
 * magenta/purple (#FF00FF -- the same color services/expression_control.c
 * uses for its sub-menu's selected-pad indicator and
 * services/boot_sequence.c uses for its final pulse phase) instead of
 * plain blue, at TILES_LIGHTING_ROOT_BASELINE_PERCENT -- deliberately
 * LOWER than TILES_LIGHTING_IDLE_BASELINE_PERCENT above (the natural-key
 * baseline), a reversal of this feature's first pass, which had root
 * brighter than naturals to stand out as a landmark; real feedback
 * called for the opposite, a subtler root indicator that reads as dimmer
 * than the surrounding white keys rather than a bright highlight.
 * Unmeasured -- a first attempt at "visibly dimmer than a natural key,
 * not so dim it disappears," not calibrated against real LED
 * brightness/diffusion. Sharp/black keys get no baseline floor at all
 * when idle (true black, see write_pad() below) -- unlike every other
 * idle pad in this file, which is deliberately never allowed to go
 * fully dark (see tiles_lighting_set_pad_press()'s header); this is a
 * narrow, deliberate exception specifically for the natural/sharp
 * readability distinction real feedback asked for.
 * Raised from 6, real feedback: "make root note led also brighter" (part
 * of a broader "make all led brighter its hard to see" -- see
 * TILES_LIGHTING_IDLE_BASELINE_PERCENT above). Kept below that constant's
 * new value so root stays visibly dimmer than a natural key at rest, per
 * the same real feedback that made it dimmer in the first place -- just
 * a less extreme gap now that both are brighter in absolute terms. */
#define TILES_LIGHTING_ROOT_BASELINE_PERCENT 15u

/* Underglow's own fixed brightness, out of 255 -- deliberately NOT
 * scaled by the active brightness ceiling/the power state. It used to be
 * a percentage of the active ceiling (65%), which meant it rode down
 * with the USB-only ceiling (37%) to ~24% of full and read as
 * "basically not glowing" on real hardware. Only 4 LEDs are on this
 * chain vs 24 on the pad grid -- even at full raw brightness the
 * current draw is a small fraction of the ~448mA full-grid estimate in
 * docs/architecture/defaults-and-safeguards.md, so there's no power
 * budget reason to hold it down the way the 24-pad grid needs to be.
 * It's a fixed ambient halo, not a per-pad state indicator, so running
 * it bright doesn't compete with touch/press feedback the way raising
 * every pad's baseline would. Its contribution IS still counted against
 * the dynamic pad budget below, though -- see underglow_fixed_ma(). */
#define TILES_LIGHTING_UNDERGLOW_LEVEL 230u

#define TILES_LIGHTING_NUM_UNDERGLOW_PIXELS 4u

/* ---- Dynamic, load-aware pad brightness ceiling -------------------------
 * Real feedback: "could we push the led celing a bit more safely?" The
 * old ceiling (led_brightness_ceiling_percent from power.c, e.g. 37% on
 * USB-only) was a single flat multiplier applied to every pad regardless
 * of how many others are lit at the same time -- correct for the
 * absolute worst case (all 24 pads + underglow at full white) but far
 * more conservative than necessary for the much more common case of a
 * few notes held while the rest of the grid sits at a low idle baseline.
 * "More safely" means computing the SAME documented current budget from
 * the REAL current load every frame instead of assuming worst-case
 * always -- individual active pads can run much brighter when few pads
 * are lit, while the true worst case still lands at (if anything,
 * slightly more conservative than) today's flat number, since this is
 * the first time underglow's own real contribution is actually
 * subtracted from the shared budget rather than ignored.
 *
 * The physical numbers are exactly the ones already in
 * docs/architecture/defaults-and-safeguards.md, just used dynamically
 * instead of as a single static clamp -- nothing here raises the actual
 * current-draw safety envelope power.c already established:
 *   - ~448mA at 100% duty across all 24 pad LEDs + 4 underglow pixels
 *     (3 channels each = 84 channels total) -- so ~5.33mA per channel
 *     at full duty, a derived rate, not a new measurement.
 *   - The existing led_brightness_ceiling_percent (power.c) is now read
 *     as "what fraction of that 448mA worst-case total we're willing to
 *     spend on LEDs right now" (166mA on USB-only, 336mA external) --
 *     the SAME percentage, just enforced as a real mA budget against
 *     actual projected load instead of a blanket per-pad multiplier.
 *   - Underglow's own (fixed, unscaled) contribution is computed once
 *     and subtracted first, leaving the real remaining budget for the
 *     24-pad grid specifically -- previously not accounted for at all.
 * Still engineering estimates, not hardware-mandated -- see this
 * project's own "revisit once real 5V input current is measured" note,
 * same caveat that already applied to the flat percentage this replaces. */
#define TILES_LIGHTING_TOTAL_MA_AT_FULL 448.0f
#define TILES_LIGHTING_TOTAL_CHANNELS_AT_FULL 84.0f /* (24 pads + 4 underglow) * 3 channels */
#define TILES_LIGHTING_MA_PER_CHANNEL_AT_FULL (TILES_LIGHTING_TOTAL_MA_AT_FULL / TILES_LIGHTING_TOTAL_CHANNELS_AT_FULL)

/* Small headroom margin even when real load is very light -- never
 * actually spend 100% of the computed budget, standard practice against
 * an estimate this file's own comments already call unmeasured. */
#define TILES_LIGHTING_DYNAMIC_SCALE_CEIL 0.95f
/* Sanity floor -- guards the pathological all-pads-lit case (see this
 * section's header) from reading as "basically off" even though the
 * budget math alone would already land well above true black there. */
#define TILES_LIGHTING_DYNAMIC_SCALE_FLOOR 0.15f

typedef struct {
    float r;
    float g;
    float b;
} tiles_rgb01_t;

static tiles_sk6805_chain_t s_underglow_chain;
static tiles_sk6805_chain_t s_pad_chain;
static tiles_tca9554_t s_led_mux;
static float s_pad_press[TILES_NUM_PADS]; /* touch-driven, white, baseline-floored -- normal operation only */
static tiles_rgb01_t s_pad_standby_rgb[TILES_NUM_PADS]; /* standby animation color, no baseline floor */
static tiles_rgb01_t s_underglow_rgb[TILES_LIGHTING_NUM_UNDERGLOW_PIXELS];
static uint8_t s_service_cursor;
static bool s_initialized;
static bool s_standby_active;

static float clamp01(float v) {
    if (v < 0.0f) {
        return 0.0f;
    }
    if (v > 1.0f) {
        return 1.0f;
    }
    return v;
}

/* Fraction of TILES_LIGHTING_UNDERGLOW_LEVEL -- see that constant's
 * header comment for why underglow doesn't share the pad grid's
 * power-derived ceiling. No baseline floor: unlike pad press (below),
 * underglow (and standby pad color, also below) are allowed to go to
 * true 0 -- there's no "never fully dark" requirement for either of
 * those, and standby animations specifically need real black for
 * contrast. */
static uint8_t underglow_channel_level(float channel_0_to_1) {
    return (uint8_t)((float)TILES_LIGHTING_UNDERGLOW_LEVEL * clamp01(channel_0_to_1));
}

/* Underglow's constant contribution to the shared LED current budget --
 * see this file's "Dynamic, load-aware pad brightness ceiling" section.
 * A fixed number (underglow's brightness never varies), computed once
 * per call rather than cached since it's four multiplies and this isn't
 * a hot path. */
static float underglow_fixed_ma(void) {
    float channel_fraction = (float)TILES_LIGHTING_UNDERGLOW_LEVEL / 255.0f;
    float channels = (float)TILES_LIGHTING_NUM_UNDERGLOW_PIXELS * 3.0f;
    return channels * channel_fraction * TILES_LIGHTING_MA_PER_CHANNEL_AT_FULL;
}

/* What a pad's r/g/b would want (0.0-1.0 each) at a full 100% ceiling --
 * i.e. BEFORE any budget-driven scaling, using the fixed baseline
 * PERCENT constants directly rather than any ceiling-derived value (that
 * would be circular: the dynamic ceiling itself is computed FROM this
 * function's total across all 24 pads, see pad_dynamic_scale() below).
 * Replicates write_pad()'s own standby/press/idle-root/idle-natural/
 * idle-sharp branching -- kept as the single source of truth for "what
 * does this pad look like" so the budget estimate and the actual render
 * can never drift apart from each other. */
static tiles_rgb01_t pad_desired_rgb(uint8_t pad_index) {
    if (s_standby_active) {
        return s_pad_standby_rgb[pad_index];
    }
    if (s_pad_press[pad_index] > 0.0f) {
        /* Baseline-floored: 0.0 maps to the idle baseline fraction, not
         * true black -- this is normal (non-standby) touch-driven
         * operation's "pads never go fully dark in V1" requirement (see
         * tiles_lighting_set_pad_press's header). */
        float baseline = (float)TILES_LIGHTING_IDLE_BASELINE_PERCENT / 100.0f;
        float level = baseline + (1.0f - baseline) * clamp01(s_pad_press[pad_index]);
        return (tiles_rgb01_t){level, level, level};
    }
    /* Idle (untouched), normal chromatic play: color by note role -- real
     * feedback: "root should be blue [later: purple] and black keys
     * shouldnt have led this in rest non pressed moment." Root checked
     * first since a root pad can itself be a sharp/black key depending on
     * the current key offset (see tiles_note_map_is_root_pad()'s own
     * comment) -- root's color always wins over that. */
    uint8_t logical_pad = (uint8_t)(pad_index + 1u);
    if (tiles_note_map_is_root_pad(logical_pad)) {
        /* Sentia Instruments Magenta (#FF00FF) -- R and B channels only,
         * G stays 0 -- see TILES_LIGHTING_ROOT_BASELINE_PERCENT's own
         * comment for the color and brightness reasoning. */
        float level = (float)TILES_LIGHTING_ROOT_BASELINE_PERCENT / 100.0f;
        return (tiles_rgb01_t){level, 0.0f, level};
    }
    if (tiles_note_map_is_natural_pad(logical_pad)) {
        float level = (float)TILES_LIGHTING_IDLE_BASELINE_PERCENT / 100.0f;
        return (tiles_rgb01_t){level, level, level};
    }
    /* Sharp/black key, idle -- true black, deliberately bypassing this
     * file's usual "pads never go fully dark" floor (see
     * TILES_LIGHTING_ROOT_BASELINE_PERCENT's own comment for why this
     * specific exception exists). */
    return (tiles_rgb01_t){0.0f, 0.0f, 0.0f};
}

/* The dynamic replacement for the old flat ceiling_level() -- see this
 * file's "Dynamic, load-aware pad brightness ceiling" section for the
 * full reasoning. Sums every pad's CURRENT desired brightness (at an
 * assumed 100% ceiling, via pad_desired_rgb() above) to get a real
 * projected mA figure, and only scales down if that would exceed the
 * budget power.c's led_brightness_ceiling_percent already establishes
 * for the current power mode -- so a handful of bright pads with the
 * rest at idle can run far brighter than the old flat percentage, while
 * the true worst case (everything lit) still lands at (if anything, more
 * conservative than) that same documented number. Live, not cached --
 * recomputed fresh from current pad state every call, cheap enough (24
 * pads * a few float ops) that this isn't a concern even called once per
 * pad write. */
static float pad_dynamic_scale(void) {
    float total_channel_fraction = 0.0f;
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        tiles_rgb01_t desired = pad_desired_rgb(i);
        total_channel_fraction += desired.r + desired.g + desired.b;
    }
    float desired_ma = total_channel_fraction * TILES_LIGHTING_MA_PER_CHANNEL_AT_FULL;

    uint8_t ceiling_percent = tiles_power_get_state().led_brightness_ceiling_percent;
    float led_budget_ma = ((float)ceiling_percent / 100.0f) * TILES_LIGHTING_TOTAL_MA_AT_FULL;
    float pad_budget_ma = led_budget_ma - underglow_fixed_ma();
    if (pad_budget_ma < 0.0f) {
        pad_budget_ma = 0.0f;
    }

    float scale = TILES_LIGHTING_DYNAMIC_SCALE_CEIL;
    if (desired_ma > 0.0f && pad_budget_ma < desired_ma) {
        scale = pad_budget_ma / desired_ma;
        if (scale > TILES_LIGHTING_DYNAMIC_SCALE_CEIL) {
            scale = TILES_LIGHTING_DYNAMIC_SCALE_CEIL;
        }
        if (scale < TILES_LIGHTING_DYNAMIC_SCALE_FLOOR) {
            scale = TILES_LIGHTING_DYNAMIC_SCALE_FLOOR;
        }
    }
    return scale;
}

static void write_pad(uint8_t pad_index /* 0-23 */) {
    const tiles_pad_config_t *cfg = board_pad_config((uint8_t)(pad_index + 1u));
    if (cfg == NULL) {
        return;
    }

    tiles_rgb01_t desired = pad_desired_rgb(pad_index);
    float scale = pad_dynamic_scale();
    uint8_t r = (uint8_t)(255.0f * clamp01(desired.r * scale));
    uint8_t g = (uint8_t)(255.0f * clamp01(desired.g * scale));
    uint8_t b = (uint8_t)(255.0f * clamp01(desired.b * scale));
    uint32_t pixel = tiles_sk6805_pack_rgb(r, g, b);

    tiles_tca9554_disable_all_muxes(&s_led_mux);
    tiles_tca9554_set_select(&s_led_mux, cfg->led.mux_channel);
    tiles_tca9554_enable_mux(&s_led_mux, cfg->led.mux_index);
    tiles_sk6805_write(&s_pad_chain, &pixel, 1);
    tiles_tca9554_disable_all_muxes(&s_led_mux);
}

static void write_underglow(void) {
    uint32_t pixels[TILES_LIGHTING_NUM_UNDERGLOW_PIXELS];
    for (uint8_t i = 0; i < TILES_LIGHTING_NUM_UNDERGLOW_PIXELS; i++) {
        const tiles_rgb01_t *c = &s_underglow_rgb[i];
        pixels[i] = tiles_sk6805_pack_rgb(underglow_channel_level(c->r), underglow_channel_level(c->g),
                                           underglow_channel_level(c->b));
    }
    tiles_sk6805_write(&s_underglow_chain, pixels, TILES_LIGHTING_NUM_UNDERGLOW_PIXELS);
}

static void set_pad_press_internal(uint8_t logical_pad, float press_0_to_1) {
    if (logical_pad < 1u || logical_pad > TILES_NUM_PADS) {
        return;
    }

    uint8_t pad_index = (uint8_t)(logical_pad - 1u);
    if (s_pad_press[pad_index] == press_0_to_1) {
        return;
    }
    s_pad_press[pad_index] = press_0_to_1;

    /* Write immediately rather than waiting for tiles_lighting_service()'s
     * round-robin to reach this pad -- with 24 pads serviced one per
     * main-loop iteration, a touch change could otherwise take up to
     * ~24 loop iterations to actually reach the LED, which reads as
     * sluggish. The round-robin still runs continuously as a background
     * "keep everything current" sweep, this just short-circuits the
     * common case (touch/release) to feel immediate. */
    if (s_initialized) {
        write_pad(pad_index);
    }
}

bool tiles_lighting_init(void) {
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_pad_press[i] = 0.0f;
        s_pad_standby_rgb[i] = (tiles_rgb01_t){0.0f, 0.0f, 0.0f};
    }
    for (uint8_t i = 0; i < TILES_LIGHTING_NUM_UNDERGLOW_PIXELS; i++) {
        s_underglow_rgb[i] = (tiles_rgb01_t){1.0f, 1.0f, 1.0f};
    }
    s_service_cursor = 0;
    s_initialized = false;
    s_standby_active = false;

    if (!tiles_sk6805_init(&s_underglow_chain, pio0, TILES_GPIO_UNDERGLOW_DATA)) {
        return false;
    }
    if (!tiles_sk6805_init(&s_pad_chain, pio0, TILES_GPIO_PAD_LED_DATA)) {
        tiles_sk6805_deinit(&s_underglow_chain);
        return false;
    }
    if (!tiles_tca9554_init(&s_led_mux, i2c1, TILES_I2C1_ADDR_LED_MUX_TCA9554)) {
        tiles_sk6805_deinit(&s_underglow_chain);
        tiles_sk6805_deinit(&s_pad_chain);
        return false;
    }

    write_underglow();

    s_initialized = true;

    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        write_pad(i);
    }

    return true;
}

void tiles_lighting_set_pad_press(uint8_t logical_pad, float press_0_to_1) {
    if (s_standby_active) {
        return;
    }
    set_pad_press_internal(logical_pad, press_0_to_1);
}

void tiles_lighting_service(void) {
    if (!s_initialized) {
        return;
    }

    write_pad(s_service_cursor);
    s_service_cursor = (uint8_t)((s_service_cursor + 1u) % TILES_NUM_PADS);
}

void tiles_lighting_set_standby_active(bool active) {
    s_standby_active = active;

    if (!active) {
        /* Pads: no explicit restore needed -- touch.c calls
         * tiles_lighting_set_pad_press() every main-loop iteration
         * regardless of standby, so the very next scan (now unguarded)
         * writes each pad's real state. Underglow has no other
         * continuous driver, so restore it here explicitly. */
        for (uint8_t i = 0; i < TILES_LIGHTING_NUM_UNDERGLOW_PIXELS; i++) {
            s_underglow_rgb[i] = (tiles_rgb01_t){1.0f, 1.0f, 1.0f};
        }
        if (s_initialized) {
            write_underglow();
        }
    }
}

void tiles_lighting_set_standby_pad_rgb(uint8_t logical_pad, float r, float g, float b) {
    if (!s_standby_active || logical_pad < 1u || logical_pad > TILES_NUM_PADS) {
        return;
    }
    uint8_t pad_index = (uint8_t)(logical_pad - 1u);
    tiles_rgb01_t c = {clamp01(r), clamp01(g), clamp01(b)};
    tiles_rgb01_t *stored = &s_pad_standby_rgb[pad_index];
    if (stored->r == c.r && stored->g == c.g && stored->b == c.b) {
        return;
    }
    *stored = c;

    /* Same immediate-write reasoning as set_pad_press_internal() above --
     * an animation frame should reach the LED right away, not wait for
     * the round-robin. */
    if (s_initialized) {
        write_pad(pad_index);
    }
}

void tiles_lighting_set_standby_underglow_rgb(uint8_t pixel_index, float r, float g, float b) {
    if (!s_standby_active || pixel_index >= TILES_LIGHTING_NUM_UNDERGLOW_PIXELS) {
        return;
    }
    tiles_rgb01_t c = {clamp01(r), clamp01(g), clamp01(b)};
    tiles_rgb01_t *stored = &s_underglow_rgb[pixel_index];
    if (stored->r == c.r && stored->g == c.g && stored->b == c.b) {
        return;
    }
    *stored = c;
    if (s_initialized) {
        write_underglow();
    }
}
