#pragma once

/*
 * Pad LED + underglow control per
 * docs/architecture/defaults-and-safeguards.md "LED color and
 * brightness": both start solid white. Underglow is always on at the
 * idle baseline and never changes. Pad LEDs sit at idle baseline until
 * touch/Hall drivers exist to drive per-pad brightness -- this service
 * exposes that hook now (tiles_lighting_set_pad_press) so those
 * services can call it once they exist, without lighting.c changing.
 *
 * Brightness is clamped to services/power.h's live
 * led_brightness_ceiling_percent (35-40% on USB-only, 70-80% once
 * external power is confirmed present, per the truth table there) --
 * see lighting.c's ceiling_level(). A future profiles/ module can
 * still override this; don't bypass tiles_power_get_state() to raise
 * brightness some other way.
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
 * tiles_lighting_service() reaches that pad in its round-robin. */
void tiles_lighting_set_pad_press(uint8_t logical_pad, float press_0_to_1);

/* Re-writes one pad LED per call, round-robining through all 24, via
 * the required "disable all muxes -> set select -> enable one bank ->
 * send one pixel -> hold reset interval -> disable" sequence. Cheap to
 * call every main-loop iteration: with nothing dynamic driving press
 * yet, each call just re-sends the same idle-baseline color. */
void tiles_lighting_service(void);
