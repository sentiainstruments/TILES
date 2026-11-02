#pragma once

/* MPR121 12-electrode capacitive touch (datasheet Rev 4). Quickstart
 * baseline filters; thresholds 12/9 (mpr121.c). */

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

/* True if the chip is still in the Run Mode init set (one register read:
 * ECR). After a reset (a power glitch, static) it sits in Stop Mode and
 * reports nothing touched while still answering; false then, or on a
 * failed read. */
bool tiles_mpr121_running(tiles_mpr121_t *dev);
