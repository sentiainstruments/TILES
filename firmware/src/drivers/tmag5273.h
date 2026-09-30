#pragma once

/* TI TMAG5273A1 3-axis Hall-effect sensor.
 *
 * Register map from the TI datasheet (SLYS045C, section 8). Configures
 * continuous measurement, X/Y/Z enabled, +/-80 mT on every axis (the
 * handoff's starting range to avoid saturation), 1x averaging (fastest),
 * no CRC, standard sequential reads. Angle/gain/offset/threshold features
 * are left to calibration.
 *
 * All 24 sensors share address 0x35 and are reached one at a time through
 * the Hall muxes. services/hall selects the channel around each call. */

#include <stdbool.h>
#include <stdint.h>

#include "hardware/i2c.h"

typedef struct {
    i2c_inst_t *bus;
    uint8_t addr;
} tiles_tmag5273_t;

typedef struct {
    int16_t x;
    int16_t y;
    int16_t z;
} tiles_tmag5273_sample_t;

/* True if the manufacturer ID reads back as TI's. Read-only, safe before
 * tiles_tmag5273_init(). */
bool tiles_tmag5273_identify(i2c_inst_t *bus, uint8_t addr);

/* Writes the device and sensor config described above. The caller must
 * have selected this sensor's mux channel. */
bool tiles_tmag5273_init(tiles_tmag5273_t *dev, i2c_inst_t *bus, uint8_t addr);

/* Reads X/Y/Z (6 bytes from X_MSB_RESULT) as signed raw counts (no mT
 * conversion). Doesn't check CONV_STATUS, so reading faster than the
 * conversion rate returns the previous sample instead of blocking. */
bool tiles_tmag5273_read_xyz(const tiles_tmag5273_t *dev, tiles_tmag5273_sample_t *out);
