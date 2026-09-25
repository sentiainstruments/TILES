#pragma once

/*
 * Pad LED + underglow control. Both start solid white. Pad LEDs sit at
 * idle baseline until touch/Hall drivers exist to drive per-pad
 * brightness -- this service exposes that hook now
 * (tiles_lighting_set_pad_press) so those services can call it once
 * they exist, without lighting.c changing.
 *
 * Pad brightness is a flat fraction of services/power.h's live
 * led_brightness_ceiling_percent (37% on USB-only, 90% once external
 * power is confirmed present -- see power.c's own comment for the fuller
 * current-budget accounting behind those two numbers). Deliberately a
 * single flat multiplier, not load-aware: an earlier attempt
 * (pad_dynamic_scale(), since removed) recomputed the ceiling every
 * frame from real projected current draw so a few bright pads could run
 * brighter when the rest of the grid was idle -- real feedback after
 * seeing it on hardware: "this led solution might look glitchy like we
 * have unstable power. lets find a solution that doesnt include shifting
 * brightness." A ceiling that reacts to how many other pads are lit
 * makes the WHOLE board's brightness visibly shift as notes are struck
 * or an animation's lit-pixel count changes -- reads exactly like a
 * brownout even when the underlying math is current-safe. A given pad's
 * brightness at a given state is now always the same fixed value,
 * changing only when the power MODE itself changes (a rare, deliberate
 * transition), never with unrelated activity. A future profiles/ module
 * can still override the underlying percentage; don't bypass
 * tiles_power_get_state() to raise pad brightness some other way.
 *
 * Underglow is always on at its own fixed, high brightness (see
 * lighting.c's TILES_LIGHTING_UNDERGLOW_LEVEL) and deliberately does
 * NOT scale with the power ceiling above -- only 4 LEDs are on that
 * chain, so even at full brightness it's a negligible fraction of the
 * board's current budget, unlike the 24-pad grid.
 */

#include <stdbool.h>
#include <stdint.h>

/* Claims the underglow (GP8) and pad-LED (GP3) SK6805 PIO chains and
 * the TCA9554 LED mux controller, sets underglow to its idle-baseline
 * solid white, and sweeps all 24 pads to idle-baseline white once (SK6805
 * pixels latch, so this sweep is a one-time write, not a refresh loop).
 * Must run after board_i2c_init(). Returns false if any resource (PIO
 * program/state machine, I2C device) is unavailable. */
bool tiles_lighting_init(void);

/* Sets one pad (1-24)'s brightness as a fraction of the active ceiling:
 * 0.0 is the idle baseline (not off -- pads never go fully dark in V1),
 * 1.0 is the ceiling. Takes effect the next time
 * tiles_lighting_service() reaches that pad in its round-robin.
 *
 * A no-op while standby is active (see tiles_lighting_set_standby_active
 * below) -- touch.c/expression.c can keep calling this unconditionally
 * every scan without needing to know standby exists; it silently has no
 * effect until standby ends, at which point the very next call restores
 * the pad's real state. */
void tiles_lighting_set_pad_press(uint8_t logical_pad, float press_0_to_1);

/* The tunable "look" of the pad LEDs -- the resting brightness tiers and the
 * echo colors, formerly compile-time constants in lighting.c. Each is a
 * runtime setting (profiles/settings_table.c: look.*), saved to flash. All
 * values are whole percent of the active brightness ceiling (tints: percent of
 * full), except the echo flash duration in milliseconds. NONE of them can raise
 * the ceiling itself -- that stays fixed by the power state (see lighting.c's
 * "Pad brightness ceiling" section). Out-of-range values are clamped
 * (percent <= 100, flash <= 2000 ms). Defaults are the constants at the top of
 * lighting.c. */
typedef enum {
    TILES_LOOK_IDLE_BASELINE_PERCENT = 0, /* shared "visible but not active": pressed floor, guitar frets, chord strip */
    TILES_LOOK_NATURAL_PERCENT,           /* melodic mode: regular (non-root, non-fifth, non-playing) white pad */
    TILES_LOOK_ROOT_PERCENT,              /* melodic mode: root pad (Sentia magenta) */
    TILES_LOOK_FIFTH_PERCENT,             /* melodic mode: perfect-fifth pad (blue + red tint) */
    TILES_LOOK_FIFTH_RED_TINT_PERCENT,    /* how much red is mixed into the fifth's blue */
    TILES_LOOK_ECHO_SUSTAIN_TINT_PERCENT, /* echo (primary, green): how much white stays mixed into the sustain */
    TILES_LOOK_ECHO_SECONDARY_G_PERCENT,  /* second TILES DISPLAY (soft red): settled green amount */
    TILES_LOOK_ECHO_SECONDARY_B_PERCENT,  /* second TILES DISPLAY (soft red): settled blue amount */
    TILES_LOOK_ECHO_FLASH_MS,             /* echo onset flash duration; 0 = no flash */
    TILES_LOOK_COUNT
} tiles_look_param_t;

uint16_t tiles_lighting_get_look(tiles_look_param_t param);
void tiles_lighting_set_look(tiles_look_param_t param, uint16_t value);

/* Re-writes one pad LED per call, round-robining through all 24, via
 * the required "disable all muxes -> set select -> enable one bank ->
 * send one pixel -> hold reset interval -> disable" sequence. Cheap to
 * call every main-loop iteration: with nothing dynamic driving press
 * yet, each call just re-sends the same idle-baseline color. */
void tiles_lighting_service(void);

/* ---- Standby animation hooks (services/standby.c) ----------------------
 *
 * These exist so standby.c can drive pads/underglow directly without
 * fighting touch.c's own continuous tiles_lighting_set_pad_press() calls
 * (see that function's comment above) or needing its own copy of the
 * ceiling/idle-baseline math.
 */

/* true: touch/expression's tiles_lighting_set_pad_press() calls are
 * ignored and underglow stops being driven by anything else, so only
 * tiles_lighting_set_standby_pad()/_underglow() (below) reach the LEDs.
 * false: restores underglow to its default fixed level immediately;
 * pads are NOT explicitly restored here -- the next
 * tiles_lighting_set_pad_press() call (touch.c runs every main-loop
 * iteration regardless of standby) does that naturally. */
void tiles_lighting_set_standby_active(bool active);

/* Sets one pad's standby-animation color: each of r/g/b (0.0-1.0) is
 * scaled independently by the pad ceiling -- unlike
 * tiles_lighting_set_pad_press, there is NO idle-baseline floor here,
 * so {0,0,0} is true black. Meant only for standby.c's animation
 * frames; a no-op unless standby is active. White animations just pass
 * r=g=b=brightness. */
void tiles_lighting_set_standby_pad_rgb(uint8_t logical_pad, float r, float g, float b);

/* Sets one underglow pixel's (0-3, chain order) standby-animation
 * color: each of r/g/b (0.0-1.0) is scaled independently by
 * TILES_LIGHTING_UNDERGLOW_LEVEL. Meant only for standby.c's animation
 * frames; a no-op unless standby is active. */
void tiles_lighting_set_standby_underglow_rgb(uint8_t pixel_index, float r, float g, float b);
