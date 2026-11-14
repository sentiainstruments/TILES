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

/* A kick that ends by itself (no sustain, no stop needed): a menu pick or
 * a game's signal, which no note-off will ever end. */
void tiles_haptics_trigger_click(uint8_t logical_pad, uint8_t velocity_0_127);

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

/* Per-pad counts since boot (or the last reset), for HAPTICS over the
 * settings link: what was asked for, and what reached the motor. */
typedef struct {
    uint16_t asked;    /* trigger_kick() calls */
    uint16_t started;  /* kicks that drove the motor */
    uint16_t pulses;   /* touch pulses started */
    uint16_t stops;    /* stops of an active pad */
    uint16_t cancelled; /* stopped while still queued: never felt */
    uint16_t stolen;   /* motor taken by a newer kick past the ceiling */
    uint16_t dropped;  /* asked for but refused (silenced, no voices) */
    uint8_t phase;     /* now: 0 idle, 1 queued, 2 pulse, 3 kick, 4 gap, 5 sustain */
} tiles_haptics_pad_stats_t;
tiles_haptics_pad_stats_t tiles_haptics_pad_stats(uint8_t logical_pad);
void tiles_haptics_reset_stats(void);
bool tiles_haptics_is_silenced(void);
/* One pad's motor alone (TEST MOTOR), to tell a dead motor from firmware. */
void tiles_haptics_test_motor(uint8_t logical_pad, float duty_0_to_1);
void tiles_haptics_test_stop(void);
