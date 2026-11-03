#pragma once

/* TMAG5273A1 3-axis Hall sensor (datasheet SLYS045C): continuous X/Y/Z,
 * +/-80 mT, set averaging, no CRC. All 24 share address 0x35 behind the
 * muxes; services/hall selects the channel. */

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

/* Writes the device and sensor config described above, with `averaging`
 * (1, 2, 4, 8, 16 or 32; rounded down). The caller must have selected this
 * sensor's mux channel. */
bool tiles_tmag5273_init(tiles_tmag5273_t *dev, i2c_inst_t *bus, uint8_t addr, uint8_t averaging);

/* Changes only the averaging (one register write); same mux rule. */
bool tiles_tmag5273_set_averaging(const tiles_tmag5273_t *dev, uint8_t averaging);

/* Reads X/Y/Z (6 bytes from X_MSB_RESULT) as signed raw counts (no mT
 * conversion). Doesn't check CONV_STATUS, so reading faster than the
 * conversion rate returns the previous sample instead of blocking. */
bool tiles_tmag5273_read_xyz(const tiles_tmag5273_t *dev, tiles_tmag5273_sample_t *out);

/* Z only (2 bytes from Z_MSB_RESULT): a third of the read, for a key at
 * rest, which needs no tilt. */
bool tiles_tmag5273_read_z(const tiles_tmag5273_t *dev, int16_t *z);

/* True if the sensor still has the config init wrote (one register read:
 * DEVICE_CONFIG_2). A sensor that lost power resets to standby and stops
 * measuring while still answering; false then, or on a failed read. */
bool tiles_tmag5273_config_ok(const tiles_tmag5273_t *dev);
