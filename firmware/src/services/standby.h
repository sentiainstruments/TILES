#pragma once

/* Standby and deep sleep, lighting only (playing still works and wakes it).
 * Standby after 1 min idle: rotating ambient animations. Deep sleep after
 * 20 min: dark, circle pulsing, haptics silenced. Sequencer mode: 20 and 30
 * min. Incoming notes or clock hold off and wake an automatic one, never a
 * manual one. Circle held 4 s: screensaver now ("-"/"+" scroll, deep sleep
 * after 30 min); 8 s: MIDI panic and deep sleep. */

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
