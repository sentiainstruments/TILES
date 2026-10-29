#pragma once

/* Touch: reads both MPR121s and maps electrodes to logical pads through
 * board_pad_config(). Owns touch state and pushes it to lighting (touched
 * pads brighten). Notes and expression live in services/expression.c,
 * which reads tiles_touch_is_touched(). */

#include <stdbool.h>
#include <stdint.h>

/* Initializes both MPR121s (0x5A, 0x5B on I2C0). Call after
 * board_i2c_init(). False if either failed; the other is still used. */
bool tiles_touch_init(void);

/* Rereads both controllers (two register reads total), updates every pad
 * and pushes each to lighting (touched 1.0, untouched 0.0). Call every
 * main-loop pass. */
void tiles_touch_scan(void);

/* Touched state for one pad (1-24). False if out of range or its
 * controller failed init. */
bool tiles_touch_is_touched(uint8_t logical_pad);
