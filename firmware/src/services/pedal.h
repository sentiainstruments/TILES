#pragma once

/* Pedal input: 1/4" TRS jack, GP26/ADC0. One signal, so the jack does
 * sustain OR expression, like most keyboards' single assignable pedal input.
 *
 *   - Sustain (CC64, default): binary, hysteresis + debounce.
 *   - Expression (CC11): linear, heel (low reading) = 0, toe = 127, the
 *     usual TRS expression-pedal wiring. Not yet verified with a real
 *     expression pedal on this circuit.
 *
 * Mode, polarity and sustain style are settings (`pedal.*`, see
 * profiles/settings_table.c); there is no on-device gesture for them.
 * Polarity defaults to normally-open (unpressed reads high via the pull-up).
 * Auto-sensing polarity/disconnect is not built; see
 * docs/architecture/defaults-and-safeguards.md "Pedal polarity". */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TILES_PEDAL_POLARITY_NORMALLY_OPEN = 0,   /* unpressed=high, pressed=low */
    TILES_PEDAL_POLARITY_NORMALLY_CLOSED = 1, /* unpressed=low, pressed=high */
} tiles_pedal_polarity_t;

#define TILES_PEDAL_DEFAULT_POLARITY TILES_PEDAL_POLARITY_NORMALLY_OPEN

typedef enum {
    TILES_PEDAL_MODE_SUSTAIN = 0,
    TILES_PEDAL_MODE_EXPRESSION = 1,
} tiles_pedal_mode_t;

#define TILES_PEDAL_DEFAULT_MODE TILES_PEDAL_MODE_SUSTAIN

/* Who does the sustaining (setting `pedal.sustain_style`):
 *   - SYNTH (default, standard MIDI): CC64; the synth holds released notes.
 *     Sustain covers the whole channel/zone, harmonic plucks included.
 *   - HOLD: no CC64; services/expression.c keeps real notes on while the
 *     pedal is down (tiles_pedal_is_holding_notes()). Harmonic plucks still
 *     end on time. The synth never sees a pedal, so recorded notes are just
 *     longer. */
typedef enum {
    TILES_PEDAL_SUSTAIN_SYNTH = 0,
    TILES_PEDAL_SUSTAIN_HOLD = 1,
} tiles_pedal_sustain_style_t;

#define TILES_PEDAL_DEFAULT_SUSTAIN_STYLE TILES_PEDAL_SUSTAIN_SYNTH

/* Configures GP26 as an ADC input. Must run after board_init(). */
void tiles_pedal_init(void);

/* Reads the ADC and runs the current mode: sustain (CC64 on a debounced
 * change) or expression (CC11 on a value change). Call every main-loop pass. */
void tiles_pedal_scan(void);

void tiles_pedal_set_polarity(tiles_pedal_polarity_t polarity);
tiles_pedal_polarity_t tiles_pedal_get_polarity(void);

/* Switches the jack's function. Winds down the mode being left first
 * (CC64=0, or CC11=127), so nothing is left stuck. */
void tiles_pedal_set_mode(tiles_pedal_mode_t mode);
tiles_pedal_mode_t tiles_pedal_get_mode(void);

/* Switches who sustains. Safe mid-performance: the next scan sends CC64=0
 * (to HOLD) or CC64=127 (to SYNTH) if the pedal is down, and expression.c
 * releases its held notes once tiles_pedal_is_holding_notes() goes false. */
void tiles_pedal_set_sustain_style(tiles_pedal_sustain_style_t style);
tiles_pedal_sustain_style_t tiles_pedal_get_sustain_style(void);

/* Debounced, polarity-corrected pedal state (either style). Always false
 * outside sustain mode. */
bool tiles_pedal_is_sustained(void);

/* True while TILES itself must hold released notes: the pedal is down AND
 * the style is TILES_PEDAL_SUSTAIN_HOLD. */
bool tiles_pedal_is_holding_notes(void);

/* Latest raw ADC reading (0-4095), for diagnostics. */
uint16_t tiles_pedal_get_raw(void);
