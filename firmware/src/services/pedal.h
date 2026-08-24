#pragma once

/*
 * Pedal input (1/4" TRS jack, GP26/ADC0). Two independent functions on
 * the same raw ADC reading:
 *
 *   - Sustain (MIDI CC64): binary, debounced, enabled by default.
 *   - Expression (MIDI CC11): continuous, DISABLED by default -- built
 *     so it's ready to enable, but the polarity/range for a real
 *     expression pot on this circuit hasn't been characterized against
 *     hardware yet (only a sustain footswitch has). Toggle it with
 *     tiles_pedal_set_expression_enabled(); this is a firmware-level
 *     runtime flag for now, and the intended hook for the companion
 *     app to control the same thing later once usb_vendor/ exists.
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
#define TILES_PEDAL_DEFAULT_EXPRESSION_ENABLED false

/* Configures GP26 as an ADC input. Must run after board_init(). */
void tiles_pedal_init(void);

/* Reads the ADC, updates debounced sustain state (sends CC64 only on a
 * change), and -- if expression is enabled -- sends CC11 on a
 * meaningful change. Call every main-loop iteration. */
void tiles_pedal_scan(void);

void tiles_pedal_set_polarity(tiles_pedal_polarity_t polarity);
tiles_pedal_polarity_t tiles_pedal_get_polarity(void);

/* The runtime on/off switch for the expression (CC11) path. */
void tiles_pedal_set_expression_enabled(bool enabled);
bool tiles_pedal_get_expression_enabled(void);

/* Debounced sustain state, already polarity-corrected. */
bool tiles_pedal_is_sustained(void);

/* Latest raw ADC reading (0-4095), for diagnostics. */
uint16_t tiles_pedal_get_raw(void);
