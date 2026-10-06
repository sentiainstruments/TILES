#pragma once

/* Per-pad haptic feedback. A strike gives a short, strong KICK scaled by
 * velocity; a held note then gets a SUSTAIN that mixes the strike velocity
 * with live pressure. Pressure dominates; velocity gives a harder strike a
 * fuller baseline (haptics.c SUSTAIN_VELOCITY_WEIGHT). Sustain duty is
 * slewed asymmetrically every scan: fast attack (~30 ms), slow release
 * (~200 ms), so easing off pressure eases off the motor.
 *
 * HARDWARE CONSTRAINT, read before changing pulse shapes: each motor is a
 * single low-side NMOS (AO3400A) to a fixed rail (handoff "Haptics"). No
 * H-bridge or driver IC, so the motor can only be driven forward (PWM) or
 * left off; true braking is impossible. "Brake" here means a hard cutoff
 * right after the kick. What the hardware does allow is overdrive: every
 * kick opens with a short full-duty spike (KICK_OVERDRIVE_MS) to start
 * the motor fast, then settles to the velocity-mapped level.
 *
 * Envelope per pad, driven by services/expression.c at the same points it
 * sends MIDI (one state machine decides note timing):
 *
 *   KICK (overdrive spike -> velocity-mapped duty)
 *     -> GAP (hard zero, the "brake")
 *       -> SUSTAIN (velocity + pressure, slewed each scan)
 *          -> hard cutoff on tiles_haptics_stop() (note-off)
 *   (TILES_HAPTICS_SUSTAIN_ENABLED 0 falls back to KICK -> GAP -> idle.)
 *
 * A kick can wait briefly in PENDING if another started very recently, to
 * stagger motor start currents (KICK_STAGGER_MIN_GAP_MS; no added latency
 * for single notes).
 *
 * Voice ceiling: services/power.h's max_haptic_voices. A kick past the
 * ceiling steals the OLDEST active pad's haptic voice, so new notes always
 * get feedback. Stealing only silences that pad's motor; its MIDI note is
 * untouched (haptics never block MIDI).
 *
 * TOUCH_PULSE: a separate, light tick the moment capacitive touch starts,
 * whether or not a press follows ("I felt you"). A real kick takes over if
 * the press arrives; the pulse never delays it. It bypasses the voice
 * ceiling (too short and weak to matter for current).
 *
 * Uses the two PCA9685s owned by services/buttons.c
 * (tiles_buttons_pca9685_for_addr()).
 *
 * Every duty curve and timing constant is an unmeasured starting point;
 * motor current hasn't been measured yet. */

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

/* Sets the global intensity. Both the expression menu's haptics row and
 * square + "-"/"+" go through this with the same column mapping, so the
 * menu always shows what was last set. Clamped to
 * [HAPTIC_INTENSITY_MIN, HAPTIC_INTENSITY_MAX]; 0.0 is a real "haptics
 * off" setting (column 1). */
void tiles_haptics_set_intensity(float level_0_to_1);

/* Current global intensity. For diagnostics/UI. */
float tiles_haptics_get_intensity(void);

/* Deep sleep silences haptics (services/standby.h), separately from
 * intensity so waking restores the player's setting. Regular standby
 * leaves haptics running. */
void tiles_haptics_set_sleep_silenced(bool silenced);

/* A power-source switch can glitch the PCA9685s' rail and reset their
 * state without resetting the RP2350. Kicks self-heal (short), but a held
 * SUSTAIN only rewrites its motor when the duty changes, so it could stay
 * silent. This rewrites every non-idle pad's current level. Called from
 * main.c's power-change callback, right AFTER
 * tiles_buttons_resync_pca9685(). */
void tiles_haptics_resync_hardware(void);

/* Bench test (settings shell `TEST MOTORS`): runs pads 1..count's motors at
 * duty_0_to_1 (raw, not scaled by the haptic strength) for
 * TILES_HAPTICS_TEST_MAX_MS, then stops them. count is capped at the power
 * mode's max_haptic_voices, so the test never asks for more than the
 * product would. Don't touch the pads meanwhile (a pad event takes its
 * motor back). Returns the count actually started. */
#define TILES_HAPTICS_TEST_MAX_MS 8000u
uint8_t tiles_haptics_test_motors(uint8_t count, float duty_0_to_1);
void tiles_haptics_test_stop(void);
