#pragma once

/*
 * Per-pad haptic feedback: a strike gives a brief, strong "kick" pulse
 * mapped to velocity; while held, the motor's intensity continuously
 * tracks aftertouch (press depth), ramping up and down with pressure.
 *
 * HARDWARE CONSTRAINT, read before changing pulse shapes: each motor is
 * switched by a single low-side NMOS (AO3400A) to a fixed supply rail --
 * see docs/hardware/SENTIA_TILES_FIRMWARE_HANDOFF.md's "Haptics"
 * section and sentia_tiles_board_map_v1.json's "driver" field. There is
 * no H-bridge and no dedicated haptic driver IC (e.g. a DRV2605L), so
 * the motor can only be driven forward (PWM 0-100%) or left off --
 * genuine reverse-drive / active braking is not physically possible on
 * this board and can't be added in software. "Braking" here means the
 * closest achievable analog: a brief kick immediately followed by a
 * hard, complete cutoff (not a soft ramp-down), which is standard
 * practice for ERM-style motors without dedicated brake circuitry.
 *
 * Envelope per pad, driven entirely by services/expression.c calling
 * the three functions below at the same points it already drives
 * midi_out.c (not touch/Hall directly -- one state machine decides note
 * timing, not two):
 *
 *   KICK (tiles_haptics_trigger_kick, brief, velocity-mapped)
 *     -> GAP (hard zero -- the "brake" moment)
 *       -> SUSTAIN (aftertouch-mapped, live-updated via
 *          tiles_haptics_set_sustain_level while held)
 *         -> hard cutoff to 0 on tiles_haptics_stop (note-off)
 *
 * Respects services/power.h's max_haptic_voices ceiling: a new kick is
 * silently dropped if the ceiling is already reached, rather than
 * stealing another pad's voice or blocking the actual MIDI note --
 * matches this codebase's "a failed/limited subsystem disables itself,
 * it doesn't block the rest" stance.
 *
 * Shares both PCA9685 chip instances with services/buttons.c (that file
 * owns their init/wake sequence) via tiles_buttons_pca9685_for_addr().
 *
 * V1 caveats, same spirit as expression.c's: the kick/sustain duty
 * curves and timing constants in haptics.c are unmeasured starting
 * guesses (no per-motor current/duty data exists yet -- see the board
 * map's "measured_current_required" TODOs), and the hardware doc's
 * "stagger motor starts >= 15ms" guidance (inrush current across many
 * motors starting at once) is NOT implemented here -- max_haptic_voices
 * caps how many can be active at once, but nothing staggers simultaneous
 * kick triggers yet.
 */

#include <stdint.h>

/* Zeroes internal per-pad state. Motor channels themselves are already
 * "full off" from tiles_buttons_init()'s blanket ALL_LED reset -- this
 * doesn't write any PCA9685 register itself, so it can run any time
 * after tiles_buttons_init(). */
void tiles_haptics_init(void);

/* Advances each pad's KICK -> GAP -> SUSTAIN phase timing. Call every
 * main-loop iteration, after services/expression.c's scan so a kick
 * triggered this iteration is timestamped against a consistent clock. */
void tiles_haptics_scan(void);

/* Called by expression.c when a note-on commits, with the exact same
 * velocity value sent as MIDI velocity. Silently dropped (no haptic
 * feedback for this touch, MIDI note-on is unaffected) if
 * services/power.h's current max_haptic_voices ceiling is already
 * reached. */
void tiles_haptics_trigger_kick(uint8_t logical_pad, uint8_t velocity_0_127);

/* Called by expression.c whenever aftertouch changes while a note is
 * held, with the same value sent as MIDI poly aftertouch. Only actually
 * reaches the motor once this pad's kick+gap window has elapsed --
 * harmless to call during KICK/GAP, the value is just cached for when
 * SUSTAIN begins. */
void tiles_haptics_set_sustain_level(uint8_t logical_pad, uint8_t aftertouch_0_127);

/* Called by expression.c at note-off. Immediately hard-cuts the motor
 * to 0 (no fade -- see the file header on why a fade isn't meaningfully
 * achievable here anyway) and frees this pad's voice slot. */
void tiles_haptics_stop(uint8_t logical_pad);
