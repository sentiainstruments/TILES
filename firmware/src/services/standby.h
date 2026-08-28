#pragma once

/*
 * Standby (idle) animations: after 1 minute with no touch/Hall/button/
 * pedal activity, the pad grid + function buttons + underglow stop
 * reflecting real input and instead run one of several rotating ambient
 * animations (see standby.c's s_animations[] -- includes ambient fades,
 * Snake/Tetris/Pong "attract mode" demos, and falling dots; weighted so
 * plain ambient ones show up roughly twice as often as the game demos,
 * see s_animation_weight[]/pick_random_animation()), switching to a new
 * random one (never the current animation or the one before it) every
 * couple of minutes. Any real activity exits standby immediately and
 * hands rendering back to touch.c/buttons.c's normal behavior. Hall
 * depth is included alongside touch specifically because real hardware
 * showed MPR121 touch alone not reliably waking standby while the pad
 * animation is running -- see TILES_STANDBY_HALL_WAKE_DEPTH in
 * standby.c.
 *
 * After TILES_STANDBY_DEEP_SLEEP_TIMEOUT_MS (20 minutes) of TOTAL
 * inactivity, standby's animations stop and the board drops to deep
 * sleep: everything dark except the circle function button pulsing
 * slowly, the one indicator that it's in this state rather than fully
 * off. Same wake conditions as standby (any touch/button/pedal activity,
 * or Hall depth once that's re-enabled). Haptics are hard-silenced for
 * the duration (services/haptics.h's tiles_haptics_set_sleep_silenced())
 * -- real feedback: "in sleep mode haptics should be off." Regular
 * standby above is untouched by this -- only deep sleep silences.
 *
 * Sequencer mode gets its OWN, longer pair of timeouts --
 * TILES_STANDBY_SEQUENCER_IDLE_TIMEOUT_MS (20 minutes) before standby,
 * TILES_STANDBY_SEQUENCER_DEEP_SLEEP_TIMEOUT_MS (30 minutes total) before
 * deep sleep -- via services/op_mode.h's tiles_op_mode_is_sequencer_
 * active(), since a sequence can legitimately run unattended in a way
 * plain melodic idle isn't expected to. Real feedback: "sleep screensaver
 * should be set to 20 minute in sequencer mode since its a more stratic
 * thing so 20 minutes and then screensaver and 10 later sleep."
 *
 * ---- Circle button (SW6) long-press gestures --------------------------
 * Holding circle for TILES_CIRCLE_SCREENSAVER_HOLD_MS (4s) manually
 * forces standby's screensaver to start immediately (skips the 1-minute
 * idle wait) and marks it "manual" (s_manual_screensaver): while in this
 * mode, SW1/SW2 step through animations sequentially, forward/backward,
 * WITHOUT waking the device -- see handle_manual_scroll_input() and
 * tiles_standby_owns_octave_buttons() (checked by octave_control.c so it
 * doesn't also step the octave/transpose key underneath the same
 * presses). Manually-entered screensaver also gets a longer runway
 * before dropping to deep sleep: TILES_STANDBY_MANUAL_DEEP_SLEEP_TIMEOUT_MS
 * (30 minutes) instead of the normal 20, since the user is actively
 * choosing to watch it. Holding circle further, to
 * TILES_CIRCLE_DEEP_SLEEP_HOLD_MS (8s), escalates straight into deep
 * sleep -- the *exact same* state the normal inactivity timeout above
 * reaches, not a separate one; real feedback: "the sleep mode after 10
 * secs is the same as the timeout of the animations, not two separate
 * things... both behave as sleep with a single circle light indicator
 * pulsing slowly... rename that to deep sleep." (An earlier version had
 * the 10s hold jump to a second, fully-blank state instead -- removed;
 * both thresholds were later shortened again, real feedback: "hold sleep
 * 4sec not 6 and hold 8 for deep sleep," 6s/10s -> 4s/8s.)
 * Both thresholds are edge-latched per hold (see handle_circle_hold()'s
 * *_fired flags) so a single long hold can't re-fire, and both are
 * handled unconditionally every scan regardless of current state.
 * A hold released before EITHER threshold fires -- an ordinary short
 * tap -- wakes the board from STANDBY/DEEP_SLEEP exactly like any other
 * button, on release: real feedback "circle... not waking the instrument
 * up from sleep." Circle is deliberately excluded from real_input_
 * active()'s own generic wake check (needed so a hold building toward
 * 4s/8s doesn't wake standby on its very first tick, before either
 * threshold can fire), so handle_circle_hold() itself is what has to
 * recognize the released-early case instead.
 *
 * Circle is also this board's general-purpose "shift"/power button --
 * real feedback: "our shift and power button is circle" -- reserved for
 * the screensaver/deep-sleep gestures above; any other modifier role
 * lives elsewhere (see services/expression_control.h for square/
 * "sentia"'s role).
 *
 * Deliberately a lighting-only concept: touch/Hall/expression/MIDI keep
 * running completely unaware standby exists (see standby.c's header for
 * why) -- playing during "standby" still works exactly as it always
 * does, only the idle LED behavior changes.
 *
 * Buttons and pads are treated as one 5-row x 6-col logical grid (row 0
 * = the 6 function buttons, physically just above pad row 1; rows 1-4 =
 * the pad grid) for animation purposes -- see board/board_layout.h
 * (shared with services/boot_sequence.c) for the mapping from board
 * columns to button ids, and from board pad numbers to the 4 underglow
 * anchor points, both currently based on the user's verbal description
 * of the physical layout rather than a hardware doc, so flagged there as
 * easy to correct once seen lit on the real board.
 * Buttons are plain monochrome PCA9685 PWM, not addressable RGB like the
 * pads/underglow -- every animation collapses color to a single
 * brightness for the button row, additionally scaled down
 * (BUTTON_STANDBY_BRIGHTNESS_SCALE in standby.c) since buttons read
 * noticeably brighter than pads at the same commanded duty on real
 * hardware. Underglow normally mirrors whatever the pad grid's animation
 * shows at its 4 anchor points, but some animations (the graphic
 * equalizer, the circular underglow wave) need underglow to do something
 * genuinely different from any one pad -- see
 * s_animation_underglow_override[] in standby.c.
 */

#include <stdbool.h>

/* Seeds the idle clock from the current time and seeds this module's
 * pseudo-random source (used by the shooting-stars animation). Call
 * once, after tiles_lighting_init()/tiles_buttons_init() (both are
 * required by the standby render path) and after tiles_touch_init()/
 * tiles_pedal_init() (both are polled for activity detection). */
void tiles_standby_init(void);

/* Polls touch/button/pedal state for activity, drives the
 * awake/standby/power-saving state machine, and -- while in standby or
 * power-saving -- advances and renders the current frame at its own
 * internal frame rate. Call every main-loop iteration, after
 * tiles_buttons_scan()/tiles_touch_scan()/tiles_pedal_scan() so this
 * iteration's activity check sees fresh state. */
void tiles_standby_scan(void);

/* For diagnostics. True only for the animated standby state, not deep
 * sleep -- see tiles_standby_is_deep_sleep() below for that. */
bool tiles_standby_is_active(void);

/* For diagnostics. True for deep sleep -- the single dormant state
 * reached either by 20 minutes of total inactivity past entering
 * standby (30 if that standby was manually entered, see
 * tiles_standby_owns_octave_buttons() below), or directly by holding
 * SW6 (circle) for 10 seconds. Everything dark except the circle button
 * pulsing slowly. */
bool tiles_standby_is_deep_sleep(void);

/* True only while a screensaver manually entered by holding SW6
 * (circle) for 6 seconds is showing -- see standby.c's
 * s_manual_screensaver and handle_circle_hold(). While true, SW1/SW2
 * are repurposed here as animation-scroll controls instead of their
 * normal octave_control.c function, and octave_control.c must skip its
 * own SW1/SW2 handling entirely (checked at the top of
 * tiles_octave_control_scan(), the same way it already skips it while
 * tiles_game_mode_is_active()) so a scroll press doesn't *also*
 * silently step the octave/transpose key underneath. */
bool tiles_standby_owns_octave_buttons(void);
