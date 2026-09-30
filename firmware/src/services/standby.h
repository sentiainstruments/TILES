#pragma once

/* Standby (screensaver) and deep sleep. Lighting only: touch, Hall,
 * expression and MIDI keep running, so playing during standby works
 * normally (and wakes it).
 *
 * ---- Standby -------------------------------------------------------------
 * After 1 minute without touch/button/pedal activity, the pads, buttons and
 * underglow run rotating ambient animations (standby.c s_animations[]:
 * fades, game "attract mode" demos, falling dots, etc.; ambient ones about
 * twice as often as the demos). A new random animation (never the current
 * or previous one) every couple of minutes. Any real activity wakes it.
 * (Hall-depth wake exists but is disabled until a light-touch threshold is
 * measured; see TILES_STANDBY_HALL_WAKE_DEPTH.)
 *
 * ---- Deep sleep --------------------------------------------------------
 * After 20 minutes of total inactivity: everything dark except circle
 * pulsing slowly. Haptics are silenced (tiles_haptics_set_sleep_silenced()).
 * Same wake conditions as standby.
 *
 * Sequencer mode uses longer timeouts (20 min to standby, 30 min to deep
 * sleep), since a sequence can run unattended.
 *
 * ---- Circle (SW6) holds ------------------------------------------------
 *   - 4 s (TILES_CIRCLE_SCREENSAVER_HOLD_MS): start the screensaver now,
 *     in "manual" mode: "-"/"+" step through the animations without waking
 *     (tiles_standby_owns_octave_buttons()), and deep sleep comes after 30
 *     min instead of 20.
 *   - 8 s (TILES_CIRCLE_DEEP_SLEEP_HOLD_MS): MIDI panic, then deep sleep
 *     (the same state the timeout reaches).
 * Both are edge-latched per hold. A short tap (released before 4 s) wakes
 * the board like any button; circle is excluded from the generic wake
 * check so a building hold doesn't wake on its first tick, so
 * handle_circle_hold() handles the tap itself.
 *
 * ---- Rendering -----------------------------------------------------------
 * Buttons and pads form one 5x6 grid (row 0 = buttons; see
 * board/board_layout.h). Buttons are monochrome PWM: animations collapse to
 * one brightness for them, scaled down (BUTTON_STANDBY_BRIGHTNESS_SCALE)
 * since they look brighter than pads at the same duty. Underglow mirrors
 * the pads at its 4 anchor points, except animations that draw their own
 * (s_animation_underglow_override[]). */

#include <stdbool.h>

/* Seeds the idle clock and the pseudo-random source. Call once, after
 * lighting, buttons, touch and pedal init. */
void tiles_standby_init(void);

/* Checks touch/button/pedal activity, runs the awake/standby/deep-sleep
 * state machine and renders the current frame. Call every main-loop pass,
 * after the button, touch and pedal scans. */
void tiles_standby_scan(void);

/* True for the animated standby state only (not deep sleep). */
bool tiles_standby_is_active(void);

/* True in deep sleep (timeout, or holding circle 8 s): all dark, circle
 * pulsing. */
bool tiles_standby_is_deep_sleep(void);

/* True while a manually entered screensaver (circle held 4 s) is showing:
 * "-"/"+" scroll animations, so octave_control.c ignores them. */
bool tiles_standby_owns_octave_buttons(void);
