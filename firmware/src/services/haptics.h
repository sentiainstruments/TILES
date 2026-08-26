#pragma once

/*
 * Per-pad haptic feedback: a strike gives a brief, strong "kick" pulse
 * mapped to velocity, then a continuous SUSTAIN while held, blending
 * that same strike velocity with ongoing pressure (key travel/
 * aftertouch) -- real feedback: "map haptics to velocity and key
 * travel, this is a mix... should feel stronger with more pressure and
 * ease off when pressure is released slowly." Pressure is the dominant,
 * real-time driver; velocity just gives a harder strike a fuller
 * baseline throughout the hold (see haptics.c's SUSTAIN_VELOCITY_WEIGHT
 * and sustain_target_duty()). The "ease off... slowly" half comes from
 * an asymmetric slew on the applied motor duty (fast attack, ~30ms to
 * full swing; slow release, ~200ms -- SUSTAIN_ATTACK_PER_MS/
 * _RELEASE_PER_MS), run every scan tick so release keeps progressing in
 * real time even while pressure sits still or updates infrequently.
 *
 * SUSTAIN was previously built but disabled
 * (TILES_HAPTICS_SUSTAIN_ENABLED 0): on real hardware it read as
 * continuous buzzing rather than a real pressure signal. Two things
 * changed since: the magnets are now seated (previously not, meaning
 * the depth/aftertouch signal it would have tracked was noise against a
 * meaningless baseline), and services/expression.c's aftertouch is now
 * calibrated from a real capture session and smoothed (previously raw
 * and unscaled) -- plausibly the actual cause of the old "buzzing"
 * complaint, not sustain as a concept. Re-enabled and reworked into the
 * velocity+pressure mix above rather than a simple retry of the
 * original aftertouch-only design.
 *
 * The kick itself was also boosted -- real feedback: "the haptic kick
 * is too soft for the touch... boost it a lot." KICK_DURATION_MS,
 * KICK_OVERDRIVE_MS, and MIN_KICK_DUTY (the floor even the weakest
 * strike gets) were all raised; see their own comments in haptics.c.
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
 * Overdrive is the real, physically-achievable technique this hardware
 * *does* support for a snappier attack, and is used here: every kick
 * opens with a brief spike at full duty regardless of velocity (see
 * KICK_OVERDRIVE_MS in haptics.c), overcoming the motor's static
 * friction/inertia faster than the velocity-mapped duty alone would, and
 * only then settles to that velocity-mapped level for the rest of the
 * kick window. This is about starting fast, not stopping fast -- it
 * doesn't substitute for braking (still impossible here).
 *
 * Envelope per pad, driven entirely by services/expression.c calling
 * the three functions below at the same points it already drives
 * midi_out.c (not touch/Hall directly -- one state machine decides note
 * timing, not two):
 *
 *   KICK (tiles_haptics_trigger_kick, overdrive spike -> velocity-mapped)
 *     -> GAP (hard zero -- the "brake" moment)
 *       -> SUSTAIN (velocity+pressure mix, live-updated via
 *          tiles_haptics_set_sustain_level while held, slewed toward its
 *          target every scan tick rather than jumping)
 *          -> hard cutoff to 0 on tiles_haptics_stop (note-off)
 *   (TILES_HAPTICS_SUSTAIN_ENABLED 0 in haptics.c falls back to KICK ->
 *   GAP -> IDLE, a single click with no sustain at all, if SUSTAIN ever
 *   needs disabling again -- the code path stays in place for that.)
 *
 * A kick may first sit briefly in an internal PENDING state (see
 * haptics.c) if another kick started too recently -- see
 * KICK_STAGGER_MIN_GAP_MS there for the "stagger motor starts" handling
 * and why it doesn't add latency to normal single-note play.
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
 * V1 caveat, same spirit as expression.c's: the kick/sustain duty curves
 * and every timing constant in haptics.c are unmeasured starting
 * guesses -- no per-motor current/duty data exists yet (see the board
 * map's "measured_current_required" TODOs).
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
 * held, with the same value sent as MIDI poly aftertouch. Only updates
 * this pad's target -- the actual motor duty is driven continuously by
 * tiles_haptics_scan()'s per-tick attack/release slew toward that
 * target (blended with this strike's velocity), not written directly
 * here. Harmless to call during KICK/GAP; the value is just cached for
 * when SUSTAIN begins. */
void tiles_haptics_set_sustain_level(uint8_t logical_pad, uint8_t aftertouch_0_127);

/* Called by expression.c at note-off. Immediately hard-cuts the motor
 * to 0 (no fade -- see the file header on why a fade isn't meaningfully
 * achievable here anyway) and frees this pad's voice slot. */
void tiles_haptics_stop(uint8_t logical_pad);
