#pragma once

/* NXP/Freescale MPR121 12-electrode capacitive touch controller.
 *
 * Register map and init per the datasheet (Rev 4, 02/2013). Baseline
 * filter values are Freescale's quickstart configuration (AN3891 covers
 * tuning). Touch/release thresholds are 12/9 (see mpr121.c). Final
 * per-electrode sensitivity is a calibration task on the assembled unit;
 * this driver's job is reliable detection. */

#include <stdbool.h>
#include <stdint.h>

#include "hardware/i2c.h"

typedef struct {
    i2c_inst_t *bus;
    uint8_t addr;
} tiles_mpr121_t;

/* Soft-resets, configures filtering and thresholds, then enters Run Mode
 * with all 12 electrodes (no proximity channel). False on I2C failure. */
bool tiles_mpr121_init(tiles_mpr121_t *dev, i2c_inst_t *bus, uint8_t addr);

/* Returns the 12-bit touch mask (bit N = electrode N). A plain register
 * read, safe to call continuously. On I2C failure returns 0 and sets
 * *ok=false (if ok != NULL). */
uint16_t tiles_mpr121_read_touched(tiles_mpr121_t *dev, bool *ok);
