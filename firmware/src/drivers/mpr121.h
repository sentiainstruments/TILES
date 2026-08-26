#pragma once

/*
 * NXP/Freescale MPR121 12-electrode capacitive touch controller driver.
 *
 * Register map and init sequence per the real MPR121 datasheet
 * (Freescale MPR121 Rev 4, 02/2013), fetched and read directly rather
 * than recalled from memory. Baseline-filter values (MHD/NHD/NCL/FDL)
 * match Freescale's own published quickstart configuration -- the
 * datasheet itself defers exact filter tuning to a separate app note
 * (AN3891) rather than mandating one value, so these are a reasonable
 * functional starting point, not final-tuned. The touch/release
 * thresholds (12/9, mpr121.c) started at the quickstart's 12/6 but the
 * release side was narrowed once the keycap/pad assembly was actually
 * seated -- see mpr121.c's own comment for the real-hardware feedback
 * ("release is sticking") that motivated it. Per
 * SENTIA_TILES_FIRMWARE_HANDOFF.md, real thresholds still need full
 * per-electrode calibration after the complete keycap/acrylic/flex/
 * enclosure assembly -- this driver's job is "touch detection works
 * well," not final-tuned sensitivity.
 */

#include <stdbool.h>
#include <stdint.h>

#include "hardware/i2c.h"

typedef struct {
    i2c_inst_t *bus;
    uint8_t addr;
} tiles_mpr121_t;

/* Soft-resets the chip, configures baseline filtering and touch/release
 * thresholds for all 12 electrodes, then enters Run Mode with all 12
 * electrodes enabled (no proximity channel -- this board doesn't use
 * it). Returns false on I2C failure. */
bool tiles_mpr121_init(tiles_mpr121_t *dev, i2c_inst_t *bus, uint8_t addr);

/* Reads the touch status registers and returns a 12-bit mask, bit N =
 * electrode N touched. Plain register read, no mode change -- safe to
 * call continuously. Returns 0 and sets *ok=false (if ok != NULL) on
 * I2C failure. */
uint16_t tiles_mpr121_read_touched(tiles_mpr121_t *dev, bool *ok);
