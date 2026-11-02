#pragma once

/* Pedal input (1/4" TRS, GP26/ADC0): sustain (CC64, default) or expression
 * (CC11, heel 0 to toe 127; untested with a real pedal). Mode, polarity
 * and style are settings only (pedal.*); polarity defaults to normally
 * open. */

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

/* Sustain style: SYNTH (default) sends CC64 and the synth holds notes,
 * harmonics included; HOLD sends no CC64 and expression.c keeps real notes
 * on itself. */
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
