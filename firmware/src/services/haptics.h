#pragma once

/* Per-pad haptics: a velocity-scaled KICK on a strike, then a SUSTAIN from
 * velocity and live pressure (fast attack, slow release). Driven by
 * expression.c at the same points as MIDI.
 * Hardware: one low-side NMOS per motor, no H-bridge, so no braking: a kick
 * opens with a full-duty overdrive spike and ends in a hard cutoff.
 * Close kicks are staggered (motor inrush); past power.h's voice ceiling a
 * kick steals the oldest pad's motor (never its MIDI). A light TOUCH_PULSE
 * ticks on touch alone. Every curve and timing is an unmeasured start. */

#include <stdbool.h>
#include <stdint.h>

/* Zeroes per-pad state. Writes no registers (buttons init already left
 * every motor channel off), so any time after tiles_buttons_init(). */
void tiles_haptics_init(void);

/* Advances each pad's envelope. Call every main-loop pass, after
 * services/expression.c's scan. */
void tiles_haptics_scan(void);

/* On note-on, with the MIDI velocity. At the voice ceiling, steals the
 * oldest active pad's voice (see header). MIDI is unaffected. */
void tiles_haptics_trigger_kick(uint8_t logical_pad, uint8_t velocity_0_127);

/* When capacitive touch first starts (IDLE -> AWAITING_STRIKE). A soft
 * tick that never blocks a kick. */
void tiles_haptics_trigger_touch_pulse(uint8_t logical_pad);

/* While a note is held, with the pressure value sent over MIDI. Sets the
 * target only; scan slews the motor toward it. Harmless during KICK/GAP
 * (cached for SUSTAIN). */
void tiles_haptics_set_sustain_level(uint8_t logical_pad, uint8_t aftertouch_0_127);

/* At note-off: motor to 0 at once (hard cut) and the voice is freed. */
void tiles_haptics_stop(uint8_t logical_pad);

/* Global intensity, from the Feel menu's haptics row or square + "-"/"+"
 * (same mapping). 0 = off. */
void tiles_haptics_set_intensity(float level_0_to_1);

/* Current global intensity. For diagnostics/UI. */
float tiles_haptics_get_intensity(void);

/* Deep sleep silences haptics (services/standby.h), separately from
 * intensity so waking restores the player's setting. Regular standby
 * leaves haptics running. */
void tiles_haptics_set_sleep_silenced(bool silenced);

/* A power switch can reset the PCA9685s without the RP2350, leaving a held
 * sustain silent: rewrite every active pad. Called right after
 * tiles_buttons_resync_pca9685(). */
void tiles_haptics_resync_hardware(void);

/* Bench test (TEST MOTORS): pads 1..count at raw duty for
 * TILES_HAPTICS_TEST_MAX_MS, count capped at the power mode's voices.
 * Returns the count started. */
#define TILES_HAPTICS_TEST_MAX_MS 8000u
uint8_t tiles_haptics_test_motors(uint8_t count, float duty_0_to_1);
void tiles_haptics_test_stop(void);
