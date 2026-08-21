#pragma once

/*
 * TI TMAG5273A1 3-axis Hall-effect sensor driver.
 *
 * Register map transcribed from the real TI datasheet (SLYS045C, local
 * copy at /Users/matiascevallos/Downloads/tmag5273.pdf, Section 8) --
 * not guessed from memory, since a wrong bitfield here (e.g. the wrong
 * magnetic range) would silently misconfigure the sensor rather than
 * fail loudly.
 *
 * Configures: continuous-measure mode, X/Y/Z magnetic channels enabled,
 * +/-80mT range on every axis (per
 * docs/hardware/SENTIA_TILES_FIRMWARE_HANDOFF.md's calibration
 * requirement to start at +/-80mT to avoid saturation), 1x conversion
 * averaging (fastest -- revisit once real noise is measured), no CRC,
 * standard sequential register reads. No angle/gain/offset/threshold
 * features are configured; those belong to the calibration layer once
 * it exists, not this driver.
 *
 * Every one of the 24 physical sensors shares I2C address 0x35 and is
 * reachable one at a time only via its Hall mux channel. This driver
 * doesn't know about muxing -- that's services/hall, which selects a
 * channel before calling in here and deselects after.
 */

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

/* Confirms the manufacturer ID registers read back as TI's expected
 * values. Read-only -- touches no configuration, safe to call before
 * tiles_tmag5273_init() as a "is this actually a TMAG5273" check. */
bool tiles_tmag5273_identify(i2c_inst_t *bus, uint8_t addr);

/* Writes DEVICE_CONFIG_1/2 and SENSOR_CONFIG_1/2 per the V1 defaults
 * described above. Caller must have already selected this sensor's
 * Hall mux channel. */
bool tiles_tmag5273_init(tiles_tmag5273_t *dev, i2c_inst_t *bus, uint8_t addr);

/* Reads X/Y/Z result registers (6 bytes starting at X_MSB_RESULT) in
 * one block read and converts each to a signed 16-bit raw count -- no
 * mT conversion here, that's calibration's job once it exists. In
 * continuous-measure mode a result is always available; this does not
 * check CONV_STATUS.RESULT_STATUS first, so calling faster than the
 * sensor's internal conversion rate can return a repeat of the previous
 * sample rather than blocking. */
bool tiles_tmag5273_read_xyz(const tiles_tmag5273_t *dev, tiles_tmag5273_sample_t *out);
