#pragma once

#include <stdbool.h>

/*
 * Real feedback: "we need an indicator for crash now that we skip boot
 * sequence so turn underglow a pulsing red to indicate crash that can
 * be cancelled or aknowledged by presing shift for 2 secodns on its
 * own." Context: main.c's crash-recovery boot now skips the power-on
 * animation entirely (see services/README.md's boot-time-printf-skip
 * entry) specifically so recovery is fast -- but that animation used to
 * be the only visible "something just happened" signal on a normal
 * fresh boot, and skipping it left a genuine crash-recovery reboot
 * looking completely silent from the outside. This is the replacement
 * signal, deliberately cheap and non-blocking (unlike the animation it
 * replaces) so it coexists with normal playing instead of holding up
 * boot.
 *
 * "shift" is SW6/circle -- see services/expression_control.h's own
 * real-feedback quote: "our shift and power button is circle."
 * Dismiss is a single click of circle alone (real feedback, after a
 * first hardware pass tried a 2-second hold instead: "dismiss is a
 * single shift click not a hold") -- "alone" requires every other
 * function button to have been up for the entire press, not just at
 * the moment of release, so this can never fire off the tail end of
 * the debug-mode combo (diamond+square+circle) or the expression-mute
 * combo (circle+square) -- both of those also involve circle, and a
 * hand lifting off one finger at a time, circle last, would otherwise
 * look identical to this click at the exact instant of release. See
 * the .c file for the actual press-tracking.
 *
 * Rendering lives in services/lighting.c (tiles_lighting_service()
 * checks tiles_crash_indicator_is_active() and, if true, overrides the
 * underglow directly -- the same bypass-standby-active-entirely pattern
 * already established for services/debug_mode.c's own pulse, see that
 * file's write_debug_underglow() comment for the fuller reasoning).
 * This module only owns the activation state and the dismiss gesture,
 * not any LED writes itself.
 */

/* Call once at boot, after crash_recovered is known (main.c's own
 * watchdog_enable_caused_reboot() read -- see main.c's opening comment)
 * and before the first tiles_crash_indicator_scan(). Activates the
 * indicator iff crash_recovered is true; otherwise this module stays
 * fully inert, indistinguishable from not existing. */
void tiles_crash_indicator_init(bool crash_recovered);

/* Call every main-loop iteration, after tiles_buttons_scan() (needs
 * fresh press state) and before tiles_lighting_service() (so this
 * tick's dismiss, if it just happened, is already reflected in that
 * same tick's render). A no-op once inactive -- cheap to call
 * unconditionally every scan regardless of whether a crash was ever
 * pending. */
void tiles_crash_indicator_scan(void);

/* True while the indicator is active (a crash-recovery boot happened
 * and hasn't been acknowledged yet this session). See services/
 * lighting.c for the only current reader. */
bool tiles_crash_indicator_is_active(void);
