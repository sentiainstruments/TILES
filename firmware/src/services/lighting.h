#pragma once

/*
 * Pad LED + underglow control. Both start solid white. Pad LEDs sit at
 * idle baseline until touch/Hall drivers exist to drive per-pad
 * brightness -- this service exposes that hook now
 * (tiles_lighting_set_pad_press) so those services can call it once
 * they exist, without lighting.c changing.
 *
 * Pad brightness is clamped to services/power.h's live
 * led_brightness_ceiling_percent (35-40% on USB-only, 70-80% once
 * external power is confirmed present, per the truth table there) --
 * see lighting.c's ceiling_level(). A future profiles/ module can
 * still override this; don't bypass tiles_power_get_state() to raise
 * pad brightness some other way.
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

/* Same brightness curve as tiles_lighting_set_pad_press (0.0 = idle
 * baseline, 1.0 = ceiling), but always writes -- meant only for
 * standby.c's animation frames. */
void tiles_lighting_set_standby_pad(uint8_t logical_pad, float level_0_to_1);

/* Sets one underglow pixel (0-3, chain order) as a fraction of its
 * fixed default level (TILES_LIGHTING_UNDERGLOW_LEVEL in lighting.c) --
 * meant only for standby.c's animation frames. */
void tiles_lighting_set_standby_underglow(uint8_t pixel_index, float level_0_to_1);
