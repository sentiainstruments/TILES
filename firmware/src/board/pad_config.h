#pragma once

/*
 * Canonical 24-pad routing table.
 *
 * This is the one place that maps a logical pad (1-24) to its physical
 * touch electrode, Hall mux/channel, LED mux/channel, and haptic PWM
 * channel. Every other module (drivers, services, midi) works in terms
 * of logical pad IDs and calls board_pad_config() to find out how that
 * pad is actually wired — nothing else hard-codes a mux channel or I2C
 * address for a specific pad.
 *
 * Source of truth: docs/hardware/sentia_tiles_board_map_v1.json. If the
 * board changes, that file changes first and this table is regenerated
 * from it.
 */

#include <stdint.h>
#include "board_pins.h"

typedef enum {
    TILES_ACTIVE_LOW = 0,
    TILES_ACTIVE_HIGH = 1,
} tiles_active_level_t;

typedef struct {
    uint8_t mpr121_i2c_addr; /* TILES_I2C0_ADDR_TOUCH1 or TOUCH2 */
    uint8_t electrode;       /* 0-11 */
} tiles_touch_route_t;

typedef struct {
    uint8_t mux_i2c_addr;    /* one of TILES_I2C0_ADDR_HALL_MUX{1,2,3} */
    uint8_t mux_channel;     /* 0-7 */
    uint8_t sensor_i2c_addr; /* always TILES_I2C0_ADDR_HALL_SENSOR behind the mux */
} tiles_hall_route_t;

typedef struct {
    uint8_t mux_index;              /* 1, 2, or 3 -- which CD74HCT4051 */
    uint8_t mux_channel;            /* 0-7, the S0-S2 select value */
    uint8_t tca9554_enable_port;    /* TCA9554 port gating this mux's active-low /EN */
    tiles_active_level_t enable_active_level;
} tiles_led_route_t;

typedef struct {
    uint8_t pca9685_i2c_addr; /* TILES_I2C1_ADDR_HAPTIC_PCA9685_{1,2} */
    uint8_t channel;          /* 0-15 */
    tiles_active_level_t active_level;
} tiles_haptic_route_t;

typedef struct {
    uint8_t logical_pad; /* 1-24, row-major */
    uint8_t row;         /* 1-4 */
    uint8_t col;         /* 1-6 */
    float center_x_mm;
    float center_y_mm;
    uint8_t fpc_index; /* 1-24, matches logical_pad on this revision but kept distinct */

    tiles_touch_route_t touch;
    tiles_hall_route_t hall;
    tiles_led_route_t led;
    tiles_haptic_route_t haptic;
} tiles_pad_config_t;

extern const tiles_pad_config_t g_tiles_pad_config[TILES_NUM_PADS];

/* Returns NULL if logical_pad is outside 1..TILES_NUM_PADS. */
const tiles_pad_config_t *board_pad_config(uint8_t logical_pad);
