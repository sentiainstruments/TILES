#pragma once

/*
 * Pedal input (1/4" TRS jack, GP26/ADC0) -- a single physical jack with
 * only one signal wired to the ADC, so it can do sustain OR expression,
 * never genuinely both -- the same "one assignable pedal input" shape
 * a lot of real synths/keyboards ship (a single jack you assign to one
 * function at a time), not two independent simultaneous features. Real
 * feedback: "enable those two as how they would work standard and lets
 * keep sustain pedal as the default." Both are now implemented to their
 * own real MIDI standard:
 *
 *   - Sustain (MIDI CC64): binary, debounced, hysteresis around
 *     midscale -- matches how a plain footswitch is universally wired
 *     and read.
 *   - Expression (MIDI CC11): continuous, linear across the ADC's full
 *     range, heel-down (low raw reading) = 0, toe-down (high raw
 *     reading) = 127 -- the standard convention for a TRS expression
 *     pedal (Roland EV-5/Yamaha FC7-style wiring). Implemented TO that
 *     standard, but -- same as before this change -- not yet confirmed
 *     against a real expression pedal on this specific circuit (only a
 *     sustain footswitch has been); real feedback's own "we can edit
 *     this in control software later" is exactly the plan for that
 *     confirmation, once a real pedal is on hand to check the polarity/
 *     range against.
 *
 * Sustain is the default mode -- switching to expression, or back,
 * cleanly resets whatever the OTHER mode last left mid-air (releases
 * sustain with a real CC64=0 rather than leaving a synth's dampers
 * stuck down; resets expression to the MIDI-spec default of 127, full
 * expression, rather than leaving playback stuck at whatever level the
 * pedal happened to be at) -- see tiles_pedal_set_mode()'s own comment.
 * tiles_pedal_set_mode() is a firmware-level runtime call for now, and
 * the intended hook for the companion app to control the same thing
 * once usb_vendor/ exists -- no on-device gesture calls it yet.
 *
 * Polarity defaults to the usual convention for a generic sustain
 * footswitch (normally-open: unpressed reads high via the board's
 * pull-up, pressed pulls low) -- switchable at runtime for a
 * differently-wired pedal, since "usual" isn't "guaranteed." Real
 * auto-sensing/calibration (reading rest state at boot to infer
 * polarity and detect a disconnected pedal) is a later layer -- see
 * docs/architecture/defaults-and-safeguards.md "Pedal polarity".
 */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TILES_PEDAL_POLARITY_NORMALLY_OPEN = 0,   /* usual: unpressed=high, pressed=low */
    TILES_PEDAL_POLARITY_NORMALLY_CLOSED = 1, /* unpressed=low, pressed=high */
} tiles_pedal_polarity_t;

#define TILES_PEDAL_DEFAULT_POLARITY TILES_PEDAL_POLARITY_NORMALLY_OPEN

typedef enum {
    TILES_PEDAL_MODE_SUSTAIN = 0,
    TILES_PEDAL_MODE_EXPRESSION = 1,
} tiles_pedal_mode_t;

#define TILES_PEDAL_DEFAULT_MODE TILES_PEDAL_MODE_SUSTAIN

/* Who does the sustaining (settings key `pedal.sustain_style`). Real
 * feedback: "well ideally the harmonics dont have sustain thats the thing.
 * can we release those ?" -> "make it a setting, flash it off by default."
 *   - SYNTH (default, standard MIDI): the pedal sends CC64 and the synth
 *     holds released notes. MIDI sustain applies to every note on the
 *     channel (in MPE, the whole zone) -- there is no per-note exemption,
 *     so harmonic plucks get held too.
 *   - HOLD: no CC64 is ever sent; services/expression.c keeps each real
 *     note ON itself while the pedal is down and sends its Note-Off when
 *     the pedal lifts (tiles_pedal_is_holding_notes()). Harmonic plucks
 *     still end after their own pluck time. The synth never sees a pedal
 *     (no half-pedal/resonance effects, no sustain lane when recording --
 *     recorded notes are simply longer). */
typedef enum {
    TILES_PEDAL_SUSTAIN_SYNTH = 0,
    TILES_PEDAL_SUSTAIN_HOLD = 1,
} tiles_pedal_sustain_style_t;

#define TILES_PEDAL_DEFAULT_SUSTAIN_STYLE TILES_PEDAL_SUSTAIN_SYNTH

/* Configures GP26 as an ADC input. Must run after board_init(). */
void tiles_pedal_init(void);

/* Reads the ADC and, depending on the current mode, either updates
 * debounced sustain state (sending CC64 only on a change) or computes
 * and sends CC11 on a meaningful change -- never both from the same
 * scan, see this file's own header comment for why. Call every
 * main-loop iteration. */
void tiles_pedal_scan(void);

void tiles_pedal_set_polarity(tiles_pedal_polarity_t polarity);
tiles_pedal_polarity_t tiles_pedal_get_polarity(void);

/* Switches which function the one physical jack currently drives.
 * Cleanly winds down whichever mode is being LEFT before switching --
 * see this file's own header comment for exactly what that means for
 * each direction -- so flipping mid-performance can never leave a
 * synth's sustain stuck down or its expression stuck at a stale level. */
void tiles_pedal_set_mode(tiles_pedal_mode_t mode);
tiles_pedal_mode_t tiles_pedal_get_mode(void);

/* Switches who does the sustaining -- see tiles_pedal_sustain_style_t. Safe
 * mid-performance: the next scan sends CC64=0 if switching to HOLD with the
 * pedal down (the synth stops holding), or CC64=127 if switching to SYNTH
 * with it down; services/expression.c releases its own held notes the
 * moment tiles_pedal_is_holding_notes() goes false. */
void tiles_pedal_set_sustain_style(tiles_pedal_sustain_style_t style);
tiles_pedal_sustain_style_t tiles_pedal_get_sustain_style(void);

/* Debounced sustain state (the physical pedal), already polarity-corrected,
 * in either sustain style. Always false while tiles_pedal_get_mode() !=
 * TILES_PEDAL_MODE_SUSTAIN. */
bool tiles_pedal_is_sustained(void);

/* True while TILES itself must hold released notes: the pedal is down AND
 * the style is TILES_PEDAL_SUSTAIN_HOLD. */
bool tiles_pedal_is_holding_notes(void);

/* Latest raw ADC reading (0-4095), for diagnostics. */
uint16_t tiles_pedal_get_raw(void);
