#include "lighting.h"

#include "board_pins.h"
#include "crash_indicator.h"
#include "debug_mode.h"
#include "note_map.h"
#include "op_mode.h"
#include "pad_config.h"
#include "power.h"

#include "pico/time.h"

#include "sk6805.h"
#include "tca9554.h"

#include <math.h>
#include <stdio.h>

/* Real feedback, across several rounds: "make all led brighter its hard
 * to see" (10 -> 25), then "lets standardise led brightnbess, resting led
 * should be brigher always. en its too dim" -- raised again, 25 -> 50, to
 * match OP_SCALE_AVAILABLE_LEVEL (services/op_mode.c) -- this codebase's
 * own already-validated "readable secondary brightness" convention
 * (itself raised 0.35 -> 0.5 for the identical complaint), rather than
 * inventing a third, different "resting" percentage. Standardizing on
 * one shared value for "visible but not the active/selected thing" across
 * melodic idle pads AND menu-available items, instead of two similar-but-
 * different numbers that drifted apart over separate rounds of tuning.
 * Still a fraction of the active brightness ceiling (a power-derived
 * safety cap from tiles_power_get_state(), untouched by this change --
 * see this file's "Pad brightness ceiling" section further below for how
 * that ceiling is picked), so this only spends more of whatever headroom
 * that ceiling already allows on the resting/idle state, not a change to
 * the underlying power budget. */
#define TILES_LIGHTING_IDLE_BASELINE_PERCENT 50u

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
 * TILES_LIGHTING_IDLE_BASELINE_PERCENT above), then raised again, real
 * feedback: "root note as well slightly brighter," then once more
 * alongside the natural-key baseline's own 25 -> 50 standardization
 * (20 -> 40, keeping roughly the same ~80% ratio to the natural-key
 * baseline rather than picking a fresh number). Kept below that
 * constant's own value so root stays visibly dimmer than a natural key
 * at rest, per the same real feedback that made it dimmer in the first
 * place -- just a less extreme gap now that both are brighter in
 * absolute terms. */
#define TILES_LIGHTING_ROOT_BASELINE_PERCENT 40u

/* Real feedback: "i need more references on melodic mode, highlight the
 * 3rd scale degree with the color teal." A second landmark alongside
 * root (see tiles_note_map_is_third_pad()'s own comment) -- teal
 * (0, level, level: green+blue equal, no red) is visually distinct from
 * both root's magenta and a natural key's white, so all three read
 * apart from each other at a glance. Same baseline percent as root
 * rather than inventing a separate number -- both are "landmark, dimmer
 * than a natural key" pads by the same reasoning TILES_LIGHTING_ROOT_
 * BASELINE_PERCENT's own comment already established; unmeasured
 * against real hardware, like every first-pass brightness constant in
 * this file. */
#define TILES_LIGHTING_THIRD_BASELINE_PERCENT 40u

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
 * every pad's baseline would. Its own current draw is still accounted
 * for in the fuller budget breakdown in this file's "Pad brightness
 * ceiling" section below, just not by any code here -- it stays a fixed
 * output regardless. */
#define TILES_LIGHTING_UNDERGLOW_LEVEL 230u

#define TILES_LIGHTING_NUM_UNDERGLOW_PIXELS 4u

/* ---- Pad brightness ceiling: back to static, deliberately -------------
 * Real feedback: "could we push the led celing a bit more safely?" led to
 * a first attempt (pad_dynamic_scale(), recomputing the ceiling every
 * frame from real projected current draw) -- then, after real hardware
 * feedback: "this led solution might look glitchy like we have unstable
 * power. lets find a solution that doesnt include shifting brightness."
 * Correct call: a ceiling that continuously reacts to how many OTHER
 * pads happen to be lit means the WHOLE board's brightness visibly
 * shifts as notes are struck/released or an animation frame's lit-pixel
 * count changes -- exactly what a real brownout looks like, even though
 * the underlying math was current-safe. Removed entirely; back to a
 * single flat ceiling per pad, chosen once (power mode changes, not
 * every frame) so a given pad's brightness at a given state is always
 * the same fixed value, never drifting with unrelated activity.
 *
 * Real feedback then asked to "calculate the safe range again to make
 * sure, acountign for ics lights and haptics and sensors" -- a fuller
 * accounting than the original ~448mA-LEDs-only estimate in
 * docs/architecture/defaults-and-safeguards.md:
 *   - LEDs: solid. 16mA/pixel at full white including ~1mA controller
 *     overhead (board map's own current_model), 28 pixels (24 pad + 4
 *     underglow) -> 448mA worst case, ~28mA idle floor even at zero
 *     brightness. This is the number the existing ceiling was built on.
 *   - MCU + sensors + I2C ICs + function-button LEDs: not measured for
 *     this board, but reasonably estimable from typical datasheet
 *     figures -- RP2350 active (~60mA) + 24x TMAG5273 Hall sensors
 *     (~3mA each, ~72mA) + 2x MPR121 (~4mA) + TCA9554/TCA9548A (~2mA) +
 *     2x PCA9685 IC overhead, not the loads they switch (~2mA) + 6
 *     function-button LEDs at worst case all lit, 150-ohm-from-5V per
 *     the board map (~80mA) -- roughly 220mA of overhead this file's
 *     own ceiling math never subtracted before.
 *   - Haptic motors: genuinely UNMEASURED -- both hardware docs flag
 *     this explicitly ("measure one motor's running and stall/start
 *     current" before trusting the higher-voice profiles). Small ERM
 *     motors typically run ~60-100mA each while spinning; USB-only
 *     allows up to 5 simultaneous voices, so a real worst case could be
 *     300-400mA from haptics ALONE -- potentially the single largest
 *     term in the whole budget, not LEDs.
 * On USB-only (500mA total), 220mA overhead + a genuinely uncertain
 * 300-400mA haptics worst case leaves little to no headroom confirmed
 * safe for LEDs beyond the existing ceiling -- raising it further isn't
 * something this fuller accounting actually supports, so power.c's
 * USB_ONLY/FAULT led_brightness_ceiling_percent stays at 37%, not
 * increased. External power (2500mA) keeps a large margin
 * (~1.8A) even under the same pessimistic haptics assumption, so that
 * ceiling (power.c's led_brightness_ceiling_percent for
 * EXTERNAL_ONLY/USB_AND_EXTERNAL) was raised 75 -> 90 there instead --
 * see power.c's own comment. Haptic motor current is the actual
 * highest-priority unknown to measure here, not anything in this file. */
static uint8_t static_ceiling_level(void) {
    uint8_t ceiling_percent = tiles_power_get_state().led_brightness_ceiling_percent;
    return (uint8_t)((255u * ceiling_percent) / 100u);
}

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
/* True while write_crash_underglow()/write_debug_underglow() owned the
 * underglow as of the last tiles_lighting_service() call -- see that
 * function's own comment on the restore-on-dismiss fix this drives. */
static bool s_underglow_override_was_active;

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

/* What a pad's r/g/b wants (0.0-1.0 each), independent of the ceiling --
 * the ceiling is applied once, afterward, in write_pad() below. Kept as
 * its own function (rather than inlined into write_pad()) as the single
 * source of truth for "what does this pad look like" in every state. */
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
    uint8_t logical_pad = (uint8_t)(pad_index + 1u);

    /* Real feedback: "im asking for the sequencer to be visible on the
     * pads on the leds" -- the capture underglow pulse below wasn't
     * enough on its own; this shows the actual loop playing back by
     * flashing whichever pad the currently-sounding note would live
     * on, layered on top of every OTHER mode's own idle coloring (see
     * tiles_op_mode_song_capture_is_note_sounding()'s own comment).
     * Checked ahead of guitar/chord-region/melodic idle coloring below
     * -- a note actually sounding right now is a more time-sensitive
     * thing to see than any of those static idle looks -- but AFTER the
     * active-touch check above, so a real live touch always wins over
     * this ambient hint. Chord-region pads are excluded: they don't
     * resolve through tiles_note_map_get_note() at all (see that
     * function's own comment), so checking it there would risk a
     * coincidental, meaningless match.
     * Formerly cross-capture's own indicator (a fixed lane on the
     * regular sequencer); now Song mode's, rewired the same way that
     * whole feature was -- see op_mode.c's own "Song mode: capture"
     * section. The OLD "current step pad" marker this file's own
     * pad_desired_rgb() used to also show here doesn't have an
     * equivalent yet -- Song mode's 128 steps have no equally natural
     * single-pad mapping onto melodic/chord/guitar's 24-pad grid the
     * way the regular sequencer's 24 steps did. Deferred, not
     * forgotten -- see tiles_op_mode_song_capture_is_note_sounding()'s
     * own declaration comment in op_mode.h. */
    if (tiles_op_mode_song_capture_is_active() && !tiles_note_map_is_chord_region_pad(logical_pad) &&
        tiles_op_mode_song_capture_is_note_sounding(tiles_note_map_get_note(logical_pad))) {
        return (tiles_rgb01_t){1.0f, 0.6f, 0.0f};
    }

    /* Guitar/bass fret mode: a completely different idle-coloring scheme,
     * checked before the melodic root/natural logic below (mutually
     * exclusive -- see services/note_map.h's own header). Real feedback:
     * "the lights should light up as frets for whatever marking make the
     * most sence" -- the standard inlay-dot convention every real guitar/
     * bass neck uses (see tiles_note_map_is_guitar_fret_marker_pad()'s own
     * comment for the exact fret numbers). Unmarked frets use the SAME
     * baseline brightness the melodic idle state does (this file's own
     * "standardize resting brightness" pass), just tinted amber instead
     * of white so guitar mode still reads as visually distinct at a
     * glance; marked frets step up from there, octave markers brightest
     * of all, mirroring how a real neck's double-dot markers stand out
     * more than the single dots. */
    if (tiles_note_map_is_guitar_mode_active()) {
        bool is_octave = false;
        if (tiles_note_map_is_guitar_fret_marker_pad(logical_pad, &is_octave)) {
            float level = is_octave ? 1.0f : 0.75f;
            return (tiles_rgb01_t){level, level * 0.5f, 0.0f};
        }
        float level = (float)TILES_LIGHTING_IDLE_BASELINE_PERCENT / 100.0f;
        return (tiles_rgb01_t){level, level * 0.5f, 0.0f};
    }

    /* Chord mode's chord strip (columns 1-2): one solid color for the
     * whole region, no per-pad root/natural/sharp distinction -- real
     * feedback: "leds for chords are color blue all of them together."
     * Checked before the melody sub-grid's own root/natural/sharp logic
     * below, which already handles chord mode's melody region (columns
     * 3-6) correctly on its own -- tiles_note_map_is_root_pad()/
     * is_natural_pad() are both already chord-mode-aware (see
     * note_map.c's own chord_mode_degree()), so no separate branch is
     * needed for that half. */
    if (tiles_note_map_is_chord_region_pad(logical_pad)) {
        float level = (float)TILES_LIGHTING_IDLE_BASELINE_PERCENT / 100.0f;
        return (tiles_rgb01_t){0.0f, 0.0f, level};
    }

    /* Idle (untouched), normal chromatic play: color by note role -- real
     * feedback: "root should be blue [later: purple] and black keys
     * shouldnt have led this in rest non pressed moment." Root checked
     * first since a root pad can itself be a sharp/black key depending on
     * the current key offset (see tiles_note_map_is_root_pad()'s own
     * comment) -- root's color always wins over that. */
    if (tiles_note_map_is_root_pad(logical_pad)) {
        /* Sentia Instruments Magenta (#FF00FF) -- R and B channels only,
         * G stays 0 -- see TILES_LIGHTING_ROOT_BASELINE_PERCENT's own
         * comment for the color and brightness reasoning. */
        float level = (float)TILES_LIGHTING_ROOT_BASELINE_PERCENT / 100.0f;
        return (tiles_rgb01_t){level, 0.0f, level};
    }
    if (tiles_note_map_is_third_pad(logical_pad)) {
        /* Teal -- G and B channels only, R stays 0 -- see
         * TILES_LIGHTING_THIRD_BASELINE_PERCENT's own comment. Checked
         * after root (root always wins if a pad were somehow both,
         * though that never actually happens for any real scale here). */
        float level = (float)TILES_LIGHTING_THIRD_BASELINE_PERCENT / 100.0f;
        return (tiles_rgb01_t){0.0f, level, level};
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

static void write_pad(uint8_t pad_index /* 0-23 */) {
    const tiles_pad_config_t *cfg = board_pad_config((uint8_t)(pad_index + 1u));
    if (cfg == NULL) {
        return;
    }

    tiles_rgb01_t desired = pad_desired_rgb(pad_index);
    uint8_t ceiling = static_ceiling_level();
    uint8_t r = (uint8_t)((float)ceiling * clamp01(desired.r));
    uint8_t g = (uint8_t)((float)ceiling * clamp01(desired.g));
    uint8_t b = (uint8_t)((float)ceiling * clamp01(desired.b));
    uint32_t pixel = tiles_sk6805_pack_rgb(r, g, b);

    /* Real feedback chasing a recurring real-hardware freeze whose
     * crash-report trace ends at 'L' (this whole function) TWICE now:
     * main.c's own per-stage trace only proves the hang is SOMEWHERE
     * in tiles_lighting_service(), not which of its two genuinely
     * different blocking operations -- I2C (the 4 TCA9554 mux calls
     * below, already timeout-bounded via drivers/i2c_bus.h) or the PIO
     * SK6805 write (also already timeout-bounded, see drivers/
     * sk6805.c's own header). Both already have real, working
     * timeouts, confirmed by reading the actual code, yet the hang
     * still recurs there -- these two characters exist so the NEXT
     * occurrence's crash report says which of the two it actually was,
     * instead of leaving that as the still-open question it currently
     * is. */
    tiles_debug_trace('i');
    tiles_tca9554_disable_all_muxes(&s_led_mux);
    tiles_tca9554_set_select(&s_led_mux, cfg->led.mux_channel);
    tiles_tca9554_enable_mux(&s_led_mux, cfg->led.mux_index);
    tiles_debug_trace('w');
    tiles_sk6805_write(&s_pad_chain, &pixel, 1);
    /* Real hardware finally caught the hang itself, not boot noise --
     * first genuine crash report since tiles_debug_mode_capture_crash_
     * snapshot() moved the snapshot copy earlier (see that function's
     * own comment): trace cut off right after 'w' on TWO boards
     * independently, at realistic multi-minute uptimes, with nothing
     * after it for the full watchdog window. But the code right there
     * -- a 5ms-timeout-bounded PIO push per pixel, then a 300us sleep_
     * us() -- has no business taking anywhere near 1000ms even in its
     * worst case (every pixel timing out). This 'x' answers whether
     * tiles_sk6805_write() actually RETURNED at all: if the next crash
     * report shows 'w' then 'x', the hang is somewhere AFTER this call
     * (the mux-disable below, or something entirely outside this
     * function, e.g. an interrupt context) -- if it still cuts off at
     * 'w' with no 'x', the hang is genuinely INSIDE tiles_sk6805_write()
     * despite its own timeout, meaning that timeout isn't actually
     * bounding the wait the way its own code implies it should. */
    tiles_debug_trace('x');
    tiles_tca9554_disable_all_muxes(&s_led_mux);
}

/* Real feedback, originally: debug mode "confirmed by the underglow
 * pulsing red steady." Recolored to Sentia Magenta once the crash
 * indicator below ALSO needed a pulsing underglow: "turn debug mode
 * light to sentia magenta instead of red to avoid confusion" -- red is
 * now reserved exclusively for "a crash just happened, unacknowledged"
 * (write_crash_underglow() below), so the two can never be mistaken for
 * each other. Same sine-pulse shape services/op_mode.c's own menu_
 * selected_pulse_level() uses for its own "this is the active/selected
 * thing" language -- not shared code (op_mode.c's own copy is static to
 * that file), just the same established visual convention, so debug
 * mode reads as consistent with everything else that pulses in this
 * firmware rather than inventing a new animation language. */
#define DEBUG_UNDERGLOW_PULSE_PERIOD_MS 900.0f
#define DEBUG_UNDERGLOW_PULSE_MIN 0.35f
#define DEBUG_UNDERGLOW_PULSE_MAX 1.0f
#define DEBUG_UNDERGLOW_PI 3.14159265358979323846f

static float debug_underglow_pulse_level(uint32_t now_ms) {
    float phase = (float)now_ms / DEBUG_UNDERGLOW_PULSE_PERIOD_MS;
    float raw = 0.5f + 0.5f * sinf(2.0f * DEBUG_UNDERGLOW_PI * phase);
    return DEBUG_UNDERGLOW_PULSE_MIN + (DEBUG_UNDERGLOW_PULSE_MAX - DEBUG_UNDERGLOW_PULSE_MIN) * raw;
}

/* Bypasses s_underglow_rgb[]/s_standby_active entirely -- real feedback
 * specifically wants this indicator visible regardless of whatever mode
 * or sub-view currently owns rendering (melodic, sequencer mid-pattern,
 * a menu, standby's own animation...), and fighting over standby-active
 * ownership to get that is exactly the bug class services/op_mode.c's
 * own README history had to fix twice already this session (search
 * "standby active" there). Writing directly to hardware here, unrelated
 * to whatever s_underglow_rgb[] currently holds, sidesteps that
 * entirely -- the instant debug mode exits, the very next call falls
 * through to this function's normal round-robin/on-change behavior and
 * underglow simply reflects whatever it was already supposed to.
 * R and B both scaled by the same pulse level (G stays 0) so the color
 * stays true Sentia Magenta at every point in the pulse, not just at
 * full brightness. */
static void write_debug_underglow(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    float pulse = debug_underglow_pulse_level(now_ms);
    uint8_t level = underglow_channel_level(pulse);
    uint32_t pixel = tiles_sk6805_pack_rgb(level, 0u, level);
    uint32_t pixels[TILES_LIGHTING_NUM_UNDERGLOW_PIXELS];
    for (uint8_t i = 0; i < TILES_LIGHTING_NUM_UNDERGLOW_PIXELS; i++) {
        pixels[i] = pixel;
    }
    /* Same 'w' used by write_pad()'s own SK6805 write -- see that
     * function's own comment on why. Sharing one character between
     * pad and underglow writes trades a little precision (which of
     * the two) for staying within the trace ring's own small budget;
     * whether debug/crash mode was active that same instant (see this
     * function's own callers) narrows it back down if needed. */
    tiles_debug_trace('w');
    tiles_sk6805_write(&s_underglow_chain, pixels, TILES_LIGHTING_NUM_UNDERGLOW_PIXELS);
    /* See write_pad()'s own 'x' comment -- same bisection, this call
     * site included since debug mode's own magenta pulse is exactly
     * what's active during a real debug-mode test session. */
    tiles_debug_trace('x');
}

/* Real feedback: "we need an indicator for crash now that we skip boot
 * sequence so turn underglow a pulsing red to indicate crash." Same
 * pulse shape as write_debug_underglow() just above, same "bypass
 * standby-active entirely, write straight to hardware" reasoning --
 * see that function's own comment -- kept as a separate copy rather
 * than parameterizing one shared function, matching this file's own
 * established precedent for this exact pulse shape (see the comment
 * above debug_underglow_pulse_level()). Pure red: this is now the one
 * and only thing in this firmware that uses it, on purpose, so it can
 * never be confused with debug mode's magenta. */
#define CRASH_UNDERGLOW_PULSE_PERIOD_MS 900.0f
#define CRASH_UNDERGLOW_PULSE_MIN 0.35f
#define CRASH_UNDERGLOW_PULSE_MAX 1.0f

static float crash_underglow_pulse_level(uint32_t now_ms) {
    float phase = (float)now_ms / CRASH_UNDERGLOW_PULSE_PERIOD_MS;
    float raw = 0.5f + 0.5f * sinf(2.0f * DEBUG_UNDERGLOW_PI * phase);
    return CRASH_UNDERGLOW_PULSE_MIN + (CRASH_UNDERGLOW_PULSE_MAX - CRASH_UNDERGLOW_PULSE_MIN) * raw;
}

static void write_crash_underglow(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    float pulse = crash_underglow_pulse_level(now_ms);
    uint8_t level = underglow_channel_level(pulse);
    uint32_t pixel = tiles_sk6805_pack_rgb(level, 0u, 0u);
    uint32_t pixels[TILES_LIGHTING_NUM_UNDERGLOW_PIXELS];
    for (uint8_t i = 0; i < TILES_LIGHTING_NUM_UNDERGLOW_PIXELS; i++) {
        pixels[i] = pixel;
    }
    /* Same 'w' used by write_pad()'s own SK6805 write -- see that
     * function's own comment on why. Sharing one character between
     * pad and underglow writes trades a little precision (which of
     * the two) for staying within the trace ring's own small budget;
     * whether debug/crash mode was active that same instant (see this
     * function's own callers) narrows it back down if needed. */
    tiles_debug_trace('w');
    tiles_sk6805_write(&s_underglow_chain, pixels, TILES_LIGHTING_NUM_UNDERGLOW_PIXELS);
    /* See write_pad()'s own 'x' comment -- same bisection. */
    tiles_debug_trace('x');
}

/* Real feedback: "captures from melodic mode or chord mode or any mode
 * into lane 3 sequencer on command... this will make the steps start
 * counting like in sequencer flashing under the current layout and the
 * playing gets saved," later rewired onto Song mode's own pattern
 * library entirely ("song mode as the default capture mode instead of
 * regular sequencer" -- see op_mode.c's own "Song mode: capture"
 * section). Same "bypass standby-active entirely, write straight to
 * hardware" reasoning as write_debug_underglow()/write_crash_
 * underglow() above -- this feature's whole point is that the current
 * mode's own pad grid stays exactly as-is underneath (except while
 * capturing from within Song mode itself, where Song's OWN standby-
 * claimed grid is what's showing instead -- see op_mode.c's own
 * render_song_underglow(), a separate, always-on yellow rather than
 * this ambient pulse), so underglow is the only real estate left for
 * an indicator here, and it has to work regardless of whether standby_
 * active happens to be claimed (melodic/chord/guitar mode never claim
 * it at all -- see set_active_mode()'s own comment in services/
 * op_mode.c). Amber (full R+G, no B): distinct from crash's pure red,
 * debug's magenta, and chord mode's own solid blue strip -- nothing
 * else in this firmware currently uses it. Same 900ms pulse period as
 * the other two for visual consistency, not shared code, matching
 * this file's own established "same convention, separate copy"
 * precedent. */
#define SONG_CAPTURE_UNDERGLOW_PULSE_PERIOD_MS 900.0f
#define SONG_CAPTURE_UNDERGLOW_PULSE_MIN 0.35f
#define SONG_CAPTURE_UNDERGLOW_PULSE_MAX 1.0f

static float song_capture_underglow_pulse_level(uint32_t now_ms) {
    float phase = (float)now_ms / SONG_CAPTURE_UNDERGLOW_PULSE_PERIOD_MS;
    float raw = 0.5f + 0.5f * sinf(2.0f * DEBUG_UNDERGLOW_PI * phase);
    return SONG_CAPTURE_UNDERGLOW_PULSE_MIN + (SONG_CAPTURE_UNDERGLOW_PULSE_MAX - SONG_CAPTURE_UNDERGLOW_PULSE_MIN) * raw;
}

static void write_song_capture_underglow(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    float pulse = song_capture_underglow_pulse_level(now_ms);
    uint8_t level = underglow_channel_level(pulse);
    uint32_t pixel = tiles_sk6805_pack_rgb(level, level, 0u);
    uint32_t pixels[TILES_LIGHTING_NUM_UNDERGLOW_PIXELS];
    for (uint8_t i = 0; i < TILES_LIGHTING_NUM_UNDERGLOW_PIXELS; i++) {
        pixels[i] = pixel;
    }
    /* Same 'w' used by write_pad()'s own SK6805 write -- see that
     * function's own comment on why. */
    tiles_debug_trace('w');
    tiles_sk6805_write(&s_underglow_chain, pixels, TILES_LIGHTING_NUM_UNDERGLOW_PIXELS);
    /* See write_pad()'s own 'x' comment -- same bisection. */
    tiles_debug_trace('x');
}

/* Real feedback: "touching the pad with shift is doing the delete and
 * save but the led indication is not working there is no underglow
 * and no pad flash confirmation either." See services/op_mode.h's own
 * tiles_op_mode_pattern_flash_underglow_color() comment for the root
 * cause -- debug mode's own override below was unconditionally
 * swallowing this confirmation the whole time it was armed. Reads the
 * color that function already resolved (same two-blink timing
 * op_mode.c's own render_pattern_bank() shows on the pad, computed
 * once there rather than a second copy here) instead of owning any
 * pulse-shape math of its own, unlike every OTHER function in this
 * priority chain -- this one's timing is inherently tied to a specific
 * pad's own flash cycle, not an independent ambient pulse. */
static void write_pattern_flash_underglow(float r, float g, float b) {
    uint8_t level_r = underglow_channel_level(r);
    uint8_t level_g = underglow_channel_level(g);
    uint8_t level_b = underglow_channel_level(b);
    uint32_t pixel = tiles_sk6805_pack_rgb(level_r, level_g, level_b);
    uint32_t pixels[TILES_LIGHTING_NUM_UNDERGLOW_PIXELS];
    for (uint8_t i = 0; i < TILES_LIGHTING_NUM_UNDERGLOW_PIXELS; i++) {
        pixels[i] = pixel;
    }
    tiles_debug_trace('w');
    tiles_sk6805_write(&s_underglow_chain, pixels, TILES_LIGHTING_NUM_UNDERGLOW_PIXELS);
    tiles_debug_trace('x');
}

static void write_underglow(void) {
    uint32_t pixels[TILES_LIGHTING_NUM_UNDERGLOW_PIXELS];
    for (uint8_t i = 0; i < TILES_LIGHTING_NUM_UNDERGLOW_PIXELS; i++) {
        const tiles_rgb01_t *c = &s_underglow_rgb[i];
        pixels[i] = tiles_sk6805_pack_rgb(underglow_channel_level(c->r), underglow_channel_level(c->g),
                                           underglow_channel_level(c->b));
    }
    /* Same 'w' used by write_pad()'s own SK6805 write -- see that
     * function's own comment on why. Sharing one character between
     * pad and underglow writes trades a little precision (which of
     * the two) for staying within the trace ring's own small budget;
     * whether debug/crash mode was active that same instant (see this
     * function's own callers) narrows it back down if needed. */
    tiles_debug_trace('w');
    tiles_sk6805_write(&s_underglow_chain, pixels, TILES_LIGHTING_NUM_UNDERGLOW_PIXELS);
    /* See write_pad()'s own 'x' comment -- same bisection. */
    tiles_debug_trace('x');
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
    s_underglow_override_was_active = false;

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

    /* Crash takes priority when both happen to be active at once (debug
     * mode can survive a crash-recovery reboot via its own
     * __uninitialized_ram state -- see services/debug_mode.c -- so a
     * fresh crash indicator and an already-on debug mode CAN genuinely
     * coexist). The crash is the more urgent, less-expected thing to
     * see; debug mode being on is something the person already knows,
     * since they're the one who turned it on. */
    bool underglow_override_active = false;
    float pattern_flash_r, pattern_flash_g, pattern_flash_b;
    /* Diagnostic only (real feedback: "still no underglow ... while in
     * pattern selector menu" -- reported AGAIN after the redundant-
     * writer fix, so the remaining gap is somewhere in here, not in
     * render_pattern_bank() anymore). Prints only on a TRANSITION (which
     * override, if any, currently owns underglow), never once per frame
     * -- narrows down whether pattern-flash is even being selected at
     * all, or whether something else is still winning ahead of it. */
    static uint8_t s_debug_last_override_kind = 0xFFu;
    uint8_t debug_override_kind = 0u;
    if (tiles_crash_indicator_is_active()) {
        write_crash_underglow();
        underglow_override_active = true;
        debug_override_kind = 1u;
    } else if (tiles_op_mode_pattern_flash_underglow_color(&pattern_flash_r, &pattern_flash_g, &pattern_flash_b)) {
        /* Above debug mode specifically -- real feedback found debug
         * mode's own override (checked just below) was unconditionally
         * swallowing this confirmation the whole time it was armed,
         * which is most of this session. A brief (600ms), directly-
         * caused-by-what-the-person-just-did confirmation outranks an
         * ambient "recording is on" pulse for that short window. */
        write_pattern_flash_underglow(pattern_flash_r, pattern_flash_g, pattern_flash_b);
        underglow_override_active = true;
        debug_override_kind = 2u;
    } else if (tiles_debug_mode_is_active()) {
        write_debug_underglow();
        underglow_override_active = true;
        debug_override_kind = 3u;
    } else if (tiles_op_mode_song_capture_is_active()) {
        /* Lowest priority of the three -- crash/debug are rarer and
         * more urgent; this one's own trigger (shift+diamond) is
         * something the person just did on purpose, same reasoning as
         * debug mode's own priority below crash. Formerly cross-
         * capture's own indicator; rewired onto Song mode's own
         * capture the same way that whole feature was -- see
         * op_mode.c's own "Song mode: capture" section. */
        write_song_capture_underglow();
        underglow_override_active = true;
        debug_override_kind = 4u;
    }
    if (debug_override_kind != s_debug_last_override_kind) {
        printf("[lighting] underglow override -> %u (0=none 1=crash 2=pattern_flash 3=debug 4=song_capture)\n",
               (unsigned)debug_override_kind);
        s_debug_last_override_kind = debug_override_kind;
    }

    /* Real feedback on the crash indicator's dismiss: "the dismiss
     * didnt work it just made the red color solid." Root cause: unlike
     * pads (write_pad()'s round-robin below re-drives every pad every
     * few frames regardless), underglow has NO other continuous
     * per-frame driver once neither override above is active -- so the
     * instant either one stops owning it, nothing ever wrote a fresh
     * value again, and it just stayed latched at whatever the pulse's
     * last brightness happened to be, forever. Detects that exact
     * transition (was overriding last frame, isn't this frame) and
     * fires write_underglow() once, which pushes s_underglow_rgb[]'s
     * CURRENT value -- always kept correctly up to date underneath the
     * override by whatever normally owns it (the plain default, or
     * standby's own animation via tiles_lighting_set_standby_underglow_
     * rgb()), regardless of this override having been on top of it --
     * exactly mirroring tiles_lighting_set_standby_active(false)'s own
     * explicit restore just below, for the identical reason. */
    if (s_underglow_override_was_active && !underglow_override_active) {
        write_underglow();
    }
    s_underglow_override_was_active = underglow_override_active;

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
