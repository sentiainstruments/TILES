#pragma once

/*
 * TCA9554 8-bit I2C GPIO expander, scoped to how SENTIA TILES actually
 * uses it: controlling the three CD74HCT4051 pad-LED muxes' shared
 * select lines (P0-P2 = S0-S2) and their three independent active-low
 * enables (P3/P4/P5 = mux 1/2/3 /EN). P6/P7 are unused and left as
 * inputs. See docs/hardware/.../led_systems.pad_leds.mux_control.
 */

#include <stdbool.h>
#include <stdint.h>

#include "hardware/i2c.h"

#define TILES_TCA9554_PORT_S0 0u
#define TILES_TCA9554_PORT_S1 1u
#define TILES_TCA9554_PORT_S2 2u
#define TILES_TCA9554_PORT_MUX1_ENABLE_N 3u
#define TILES_TCA9554_PORT_MUX2_ENABLE_N 4u
#define TILES_TCA9554_PORT_MUX3_ENABLE_N 5u

typedef struct {
    i2c_inst_t *bus;
    uint8_t addr;
    uint8_t shadow_output; /* mirrors the Output Port register so writes can read-modify-write without an I2C read */
} tiles_tca9554_t;

/* Sets P3-P5 high (all mux banks disabled) and P0-P2 low (select=0) in
 * the Output Port register *before* configuring P0-P5 as outputs, so
 * there's no glitch through an undefined level when direction changes.
 * P6/P7 are left as inputs. Returns false on I2C failure. */
bool tiles_tca9554_init(tiles_tca9554_t *dev, i2c_inst_t *bus, uint8_t addr);

/* Disables all three mux banks (P3-P5 driven high). This is the
 * required "disable every mux /EN before changing S0-S2" step, and is
 * also called internally by tiles_tca9554_enable_mux(). */
bool tiles_tca9554_disable_all_muxes(tiles_tca9554_t *dev);

/* Sets the shared S0-S2 select lines to `channel` (0-7). Call only while
 * all mux banks are disabled (see above). */
bool tiles_tca9554_set_select(tiles_tca9554_t *dev, uint8_t channel);

/* Disables all three banks, then enables exactly one (mux_index 1, 2,
 * or 3), so at most one bank is ever enabled at a time regardless of
 * prior state. Call tiles_tca9554_set_select() first. */
bool tiles_tca9554_enable_mux(tiles_tca9554_t *dev, uint8_t mux_index);
