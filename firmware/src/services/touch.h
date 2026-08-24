#pragma once

/*
 * Touch event service: reads both MPR121 controllers and derives each
 * logical pad's touched state from board_pad_config()'s touch route.
 *
 * This module owns touch state and pad LED brightness only (a touched
 * pad brightens to the lighting service's ceiling, an untouched pad
 * sits at idle baseline -- see
 * docs/architecture/defaults-and-safeguards.md). MIDI note on/off/
 * velocity/aftertouch are owned by services/expression.c, which reads
 * tiles_touch_is_touched() (below) itself rather than this module
 * reaching into MIDI -- keeps touch's own responsibility narrow.
 */

#include <stdbool.h>
#include <stdint.h>

/* Initializes both MPR121 controllers (0x5A, 0x5B on I2C0). Must run
 * after board_i2c_init(). Returns false if either controller failed
 * init -- the other one still gets used by tiles_touch_scan(). */
bool tiles_touch_init(void);

/* Rereads both controllers' touch status, updates every pad's touched
 * state, and pushes each pad's state into the lighting service
 * (tiles_lighting_set_pad_press: touched -> 1.0, untouched -> 0.0).
 * Two I2C register reads total per call, not per-pad -- call every
 * main-loop iteration. */
void tiles_touch_scan(void);

/* Latest touched state for one pad (1-24). False if out of range or if
 * that pad's controller failed init. */
bool tiles_touch_is_touched(uint8_t logical_pad);
