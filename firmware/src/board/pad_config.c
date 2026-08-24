#include "pad_config.h"

#include <stddef.h>

/* Transcribed field-for-field from docs/hardware/sentia_tiles_board_map_v1.json.
 * Row-major: pads 1-6 top row, ..., 19-24 bottom row. */

const tiles_pad_config_t g_tiles_pad_config[TILES_NUM_PADS] = {
    {
        .logical_pad = 1, .row = 1, .col = 1, .center_x_mm = -65.000f, .center_y_mm = 38.886f, .fpc_index = 1,
        .touch = {.mpr121_i2c_addr = TILES_I2C0_ADDR_TOUCH1, .electrode = 11},
        .hall = {.mux_i2c_addr = TILES_I2C0_ADDR_HALL_MUX1, .mux_channel = 4, .sensor_i2c_addr = TILES_I2C0_ADDR_HALL_SENSOR},
        .led = {.mux_index = 1, .mux_channel = 4, .tca9554_enable_port = 3, .enable_active_level = TILES_ACTIVE_LOW},
        .haptic = {.pca9685_i2c_addr = TILES_I2C1_ADDR_HAPTIC_PCA9685_1, .channel = 3, .active_level = TILES_ACTIVE_HIGH},
    },
    {
        .logical_pad = 2, .row = 1, .col = 2, .center_x_mm = -39.000f, .center_y_mm = 38.886f, .fpc_index = 2,
        .touch = {.mpr121_i2c_addr = TILES_I2C0_ADDR_TOUCH1, .electrode = 10},
        .hall = {.mux_i2c_addr = TILES_I2C0_ADDR_HALL_MUX1, .mux_channel = 3, .sensor_i2c_addr = TILES_I2C0_ADDR_HALL_SENSOR},
        .led = {.mux_index = 1, .mux_channel = 2, .tca9554_enable_port = 3, .enable_active_level = TILES_ACTIVE_LOW},
        .haptic = {.pca9685_i2c_addr = TILES_I2C1_ADDR_HAPTIC_PCA9685_1, .channel = 4, .active_level = TILES_ACTIVE_HIGH},
    },
    {
        .logical_pad = 3, .row = 1, .col = 3, .center_x_mm = -13.000f, .center_y_mm = 38.886f, .fpc_index = 3,
        .touch = {.mpr121_i2c_addr = TILES_I2C0_ADDR_TOUCH1, .electrode = 8},
        .hall = {.mux_i2c_addr = TILES_I2C0_ADDR_HALL_MUX2, .mux_channel = 4, .sensor_i2c_addr = TILES_I2C0_ADDR_HALL_SENSOR},
        .led = {.mux_index = 2, .mux_channel = 4, .tca9554_enable_port = 4, .enable_active_level = TILES_ACTIVE_LOW},
        .haptic = {.pca9685_i2c_addr = TILES_I2C1_ADDR_HAPTIC_PCA9685_1, .channel = 5, .active_level = TILES_ACTIVE_HIGH},
    },
    {
        .logical_pad = 4, .row = 1, .col = 4, .center_x_mm = 13.000f, .center_y_mm = 38.886f, .fpc_index = 4,
        .touch = {.mpr121_i2c_addr = TILES_I2C0_ADDR_TOUCH2, .electrode = 3},
        .hall = {.mux_i2c_addr = TILES_I2C0_ADDR_HALL_MUX2, .mux_channel = 3, .sensor_i2c_addr = TILES_I2C0_ADDR_HALL_SENSOR},
        .led = {.mux_index = 2, .mux_channel = 2, .tca9554_enable_port = 4, .enable_active_level = TILES_ACTIVE_LOW},
        .haptic = {.pca9685_i2c_addr = TILES_I2C1_ADDR_HAPTIC_PCA9685_2, .channel = 0, .active_level = TILES_ACTIVE_HIGH},
    },
    {
        .logical_pad = 5, .row = 1, .col = 5, .center_x_mm = 39.000f, .center_y_mm = 38.886f, .fpc_index = 5,
        .touch = {.mpr121_i2c_addr = TILES_I2C0_ADDR_TOUCH2, .electrode = 2},
        .hall = {.mux_i2c_addr = TILES_I2C0_ADDR_HALL_MUX3, .mux_channel = 4, .sensor_i2c_addr = TILES_I2C0_ADDR_HALL_SENSOR},
        .led = {.mux_index = 3, .mux_channel = 4, .tca9554_enable_port = 5, .enable_active_level = TILES_ACTIVE_LOW},
        .haptic = {.pca9685_i2c_addr = TILES_I2C1_ADDR_HAPTIC_PCA9685_2, .channel = 1, .active_level = TILES_ACTIVE_HIGH},
    },
    {
        .logical_pad = 6, .row = 1, .col = 6, .center_x_mm = 65.000f, .center_y_mm = 38.886f, .fpc_index = 6,
        .touch = {.mpr121_i2c_addr = TILES_I2C0_ADDR_TOUCH2, .electrode = 1},
        .hall = {.mux_i2c_addr = TILES_I2C0_ADDR_HALL_MUX3, .mux_channel = 3, .sensor_i2c_addr = TILES_I2C0_ADDR_HALL_SENSOR},
        .led = {.mux_index = 3, .mux_channel = 2, .tca9554_enable_port = 5, .enable_active_level = TILES_ACTIVE_LOW},
        .haptic = {.pca9685_i2c_addr = TILES_I2C1_ADDR_HAPTIC_PCA9685_2, .channel = 6, .active_level = TILES_ACTIVE_HIGH},
    },
    {
        .logical_pad = 7, .row = 2, .col = 1, .center_x_mm = -65.000f, .center_y_mm = 12.886f, .fpc_index = 7,
        .touch = {.mpr121_i2c_addr = TILES_I2C0_ADDR_TOUCH1, .electrode = 9},
        .hall = {.mux_i2c_addr = TILES_I2C0_ADDR_HALL_MUX1, .mux_channel = 5, .sensor_i2c_addr = TILES_I2C0_ADDR_HALL_SENSOR},
        .led = {.mux_index = 1, .mux_channel = 6, .tca9554_enable_port = 3, .enable_active_level = TILES_ACTIVE_LOW},
        .haptic = {.pca9685_i2c_addr = TILES_I2C1_ADDR_HAPTIC_PCA9685_1, .channel = 2, .active_level = TILES_ACTIVE_HIGH},
    },
    {
        .logical_pad = 8, .row = 2, .col = 2, .center_x_mm = -39.000f, .center_y_mm = 12.886f, .fpc_index = 8,
        .touch = {.mpr121_i2c_addr = TILES_I2C0_ADDR_TOUCH1, .electrode = 6},
        .hall = {.mux_i2c_addr = TILES_I2C0_ADDR_HALL_MUX1, .mux_channel = 2, .sensor_i2c_addr = TILES_I2C0_ADDR_HALL_SENSOR},
        .led = {.mux_index = 1, .mux_channel = 1, .tca9554_enable_port = 3, .enable_active_level = TILES_ACTIVE_LOW},
        .haptic = {.pca9685_i2c_addr = TILES_I2C1_ADDR_HAPTIC_PCA9685_1, .channel = 15, .active_level = TILES_ACTIVE_HIGH},
    },
    {
        .logical_pad = 9, .row = 2, .col = 3, .center_x_mm = -13.000f, .center_y_mm = 12.886f, .fpc_index = 9,
        .touch = {.mpr121_i2c_addr = TILES_I2C0_ADDR_TOUCH1, .electrode = 7},
        .hall = {.mux_i2c_addr = TILES_I2C0_ADDR_HALL_MUX2, .mux_channel = 5, .sensor_i2c_addr = TILES_I2C0_ADDR_HALL_SENSOR},
        .led = {.mux_index = 2, .mux_channel = 6, .tca9554_enable_port = 4, .enable_active_level = TILES_ACTIVE_LOW},
        .haptic = {.pca9685_i2c_addr = TILES_I2C1_ADDR_HAPTIC_PCA9685_1, .channel = 7, .active_level = TILES_ACTIVE_HIGH},
    },
    {
        .logical_pad = 10, .row = 2, .col = 4, .center_x_mm = 13.000f, .center_y_mm = 12.886f, .fpc_index = 10,
        .touch = {.mpr121_i2c_addr = TILES_I2C0_ADDR_TOUCH2, .electrode = 5},
        .hall = {.mux_i2c_addr = TILES_I2C0_ADDR_HALL_MUX2, .mux_channel = 2, .sensor_i2c_addr = TILES_I2C0_ADDR_HALL_SENSOR},
        .led = {.mux_index = 2, .mux_channel = 1, .tca9554_enable_port = 4, .enable_active_level = TILES_ACTIVE_LOW},
        .haptic = {.pca9685_i2c_addr = TILES_I2C1_ADDR_HAPTIC_PCA9685_2, .channel = 15, .active_level = TILES_ACTIVE_HIGH},
    },
    {
        .logical_pad = 11, .row = 2, .col = 5, .center_x_mm = 39.000f, .center_y_mm = 12.886f, .fpc_index = 11,
        .touch = {.mpr121_i2c_addr = TILES_I2C0_ADDR_TOUCH2, .electrode = 4},
        .hall = {.mux_i2c_addr = TILES_I2C0_ADDR_HALL_MUX3, .mux_channel = 5, .sensor_i2c_addr = TILES_I2C0_ADDR_HALL_SENSOR},
        .led = {.mux_index = 3, .mux_channel = 6, .tca9554_enable_port = 5, .enable_active_level = TILES_ACTIVE_LOW},
        .haptic = {.pca9685_i2c_addr = TILES_I2C1_ADDR_HAPTIC_PCA9685_2, .channel = 14, .active_level = TILES_ACTIVE_HIGH},
    },
    {
        .logical_pad = 12, .row = 2, .col = 6, .center_x_mm = 65.000f, .center_y_mm = 12.886f, .fpc_index = 12,
        .touch = {.mpr121_i2c_addr = TILES_I2C0_ADDR_TOUCH2, .electrode = 0},
        .hall = {.mux_i2c_addr = TILES_I2C0_ADDR_HALL_MUX3, .mux_channel = 2, .sensor_i2c_addr = TILES_I2C0_ADDR_HALL_SENSOR},
        .led = {.mux_index = 3, .mux_channel = 1, .tca9554_enable_port = 5, .enable_active_level = TILES_ACTIVE_LOW},
        .haptic = {.pca9685_i2c_addr = TILES_I2C1_ADDR_HAPTIC_PCA9685_2, .channel = 7, .active_level = TILES_ACTIVE_HIGH},
    },
    {
        .logical_pad = 13, .row = 3, .col = 1, .center_x_mm = -65.000f, .center_y_mm = -13.114f, .fpc_index = 13,
        .touch = {.mpr121_i2c_addr = TILES_I2C0_ADDR_TOUCH1, .electrode = 3},
        .hall = {.mux_i2c_addr = TILES_I2C0_ADDR_HALL_MUX1, .mux_channel = 6, .sensor_i2c_addr = TILES_I2C0_ADDR_HALL_SENSOR},
        .led = {.mux_index = 1, .mux_channel = 7, .tca9554_enable_port = 3, .enable_active_level = TILES_ACTIVE_LOW},
        .haptic = {.pca9685_i2c_addr = TILES_I2C1_ADDR_HAPTIC_PCA9685_1, .channel = 14, .active_level = TILES_ACTIVE_HIGH},
    },
    {
        .logical_pad = 14, .row = 3, .col = 2, .center_x_mm = -39.000f, .center_y_mm = -13.114f, .fpc_index = 14,
        .touch = {.mpr121_i2c_addr = TILES_I2C0_ADDR_TOUCH1, .electrode = 4},
        .hall = {.mux_i2c_addr = TILES_I2C0_ADDR_HALL_MUX1, .mux_channel = 1, .sensor_i2c_addr = TILES_I2C0_ADDR_HALL_SENSOR},
        .led = {.mux_index = 1, .mux_channel = 0, .tca9554_enable_port = 3, .enable_active_level = TILES_ACTIVE_LOW},
        .haptic = {.pca9685_i2c_addr = TILES_I2C1_ADDR_HAPTIC_PCA9685_1, .channel = 13, .active_level = TILES_ACTIVE_HIGH},
    },
    {
        .logical_pad = 15, .row = 3, .col = 3, .center_x_mm = -13.000f, .center_y_mm = -13.114f, .fpc_index = 15,
        .touch = {.mpr121_i2c_addr = TILES_I2C0_ADDR_TOUCH1, .electrode = 5},
        .hall = {.mux_i2c_addr = TILES_I2C0_ADDR_HALL_MUX2, .mux_channel = 6, .sensor_i2c_addr = TILES_I2C0_ADDR_HALL_SENSOR},
        .led = {.mux_index = 2, .mux_channel = 7, .tca9554_enable_port = 4, .enable_active_level = TILES_ACTIVE_LOW},
        .haptic = {.pca9685_i2c_addr = TILES_I2C1_ADDR_HAPTIC_PCA9685_1, .channel = 8, .active_level = TILES_ACTIVE_HIGH},
    },
    {
        .logical_pad = 16, .row = 3, .col = 4, .center_x_mm = 13.000f, .center_y_mm = -13.114f, .fpc_index = 16,
        .touch = {.mpr121_i2c_addr = TILES_I2C0_ADDR_TOUCH2, .electrode = 6},
        .hall = {.mux_i2c_addr = TILES_I2C0_ADDR_HALL_MUX2, .mux_channel = 1, .sensor_i2c_addr = TILES_I2C0_ADDR_HALL_SENSOR},
        .led = {.mux_index = 2, .mux_channel = 0, .tca9554_enable_port = 4, .enable_active_level = TILES_ACTIVE_LOW},
        .haptic = {.pca9685_i2c_addr = TILES_I2C1_ADDR_HAPTIC_PCA9685_2, .channel = 13, .active_level = TILES_ACTIVE_HIGH},
    },
    {
        .logical_pad = 17, .row = 3, .col = 5, .center_x_mm = 39.000f, .center_y_mm = -13.114f, .fpc_index = 17,
        .touch = {.mpr121_i2c_addr = TILES_I2C0_ADDR_TOUCH2, .electrode = 8},
        .hall = {.mux_i2c_addr = TILES_I2C0_ADDR_HALL_MUX3, .mux_channel = 6, .sensor_i2c_addr = TILES_I2C0_ADDR_HALL_SENSOR},
        .led = {.mux_index = 3, .mux_channel = 7, .tca9554_enable_port = 5, .enable_active_level = TILES_ACTIVE_LOW},
        .haptic = {.pca9685_i2c_addr = TILES_I2C1_ADDR_HAPTIC_PCA9685_2, .channel = 11, .active_level = TILES_ACTIVE_HIGH},
    },
    {
        .logical_pad = 18, .row = 3, .col = 6, .center_x_mm = 65.000f, .center_y_mm = -13.114f, .fpc_index = 18,
        .touch = {.mpr121_i2c_addr = TILES_I2C0_ADDR_TOUCH2, .electrode = 10},
        .hall = {.mux_i2c_addr = TILES_I2C0_ADDR_HALL_MUX3, .mux_channel = 1, .sensor_i2c_addr = TILES_I2C0_ADDR_HALL_SENSOR},
        .led = {.mux_index = 3, .mux_channel = 0, .tca9554_enable_port = 5, .enable_active_level = TILES_ACTIVE_LOW},
        .haptic = {.pca9685_i2c_addr = TILES_I2C1_ADDR_HAPTIC_PCA9685_2, .channel = 9, .active_level = TILES_ACTIVE_HIGH},
    },
    {
        .logical_pad = 19, .row = 4, .col = 1, .center_x_mm = -65.000f, .center_y_mm = -39.114f, .fpc_index = 19,
        .touch = {.mpr121_i2c_addr = TILES_I2C0_ADDR_TOUCH1, .electrode = 0},
        .hall = {.mux_i2c_addr = TILES_I2C0_ADDR_HALL_MUX1, .mux_channel = 7, .sensor_i2c_addr = TILES_I2C0_ADDR_HALL_SENSOR},
        .led = {.mux_index = 1, .mux_channel = 5, .tca9554_enable_port = 3, .enable_active_level = TILES_ACTIVE_LOW},
        .haptic = {.pca9685_i2c_addr = TILES_I2C1_ADDR_HAPTIC_PCA9685_1, .channel = 12, .active_level = TILES_ACTIVE_HIGH},
    },
    {
        .logical_pad = 20, .row = 4, .col = 2, .center_x_mm = -39.000f, .center_y_mm = -39.114f, .fpc_index = 20,
        .touch = {.mpr121_i2c_addr = TILES_I2C0_ADDR_TOUCH1, .electrode = 1},
        .hall = {.mux_i2c_addr = TILES_I2C0_ADDR_HALL_MUX1, .mux_channel = 0, .sensor_i2c_addr = TILES_I2C0_ADDR_HALL_SENSOR},
        .led = {.mux_index = 1, .mux_channel = 3, .tca9554_enable_port = 3, .enable_active_level = TILES_ACTIVE_LOW},
        .haptic = {.pca9685_i2c_addr = TILES_I2C1_ADDR_HAPTIC_PCA9685_1, .channel = 11, .active_level = TILES_ACTIVE_HIGH},
    },
    {
        .logical_pad = 21, .row = 4, .col = 3, .center_x_mm = -13.000f, .center_y_mm = -39.114f, .fpc_index = 21,
        .touch = {.mpr121_i2c_addr = TILES_I2C0_ADDR_TOUCH1, .electrode = 2},
        .hall = {.mux_i2c_addr = TILES_I2C0_ADDR_HALL_MUX2, .mux_channel = 7, .sensor_i2c_addr = TILES_I2C0_ADDR_HALL_SENSOR},
        .led = {.mux_index = 2, .mux_channel = 5, .tca9554_enable_port = 4, .enable_active_level = TILES_ACTIVE_LOW},
        .haptic = {.pca9685_i2c_addr = TILES_I2C1_ADDR_HAPTIC_PCA9685_1, .channel = 9, .active_level = TILES_ACTIVE_HIGH},
    },
    {
        .logical_pad = 22, .row = 4, .col = 4, .center_x_mm = 13.000f, .center_y_mm = -39.114f, .fpc_index = 22,
        .touch = {.mpr121_i2c_addr = TILES_I2C0_ADDR_TOUCH2, .electrode = 7},
        .hall = {.mux_i2c_addr = TILES_I2C0_ADDR_HALL_MUX2, .mux_channel = 0, .sensor_i2c_addr = TILES_I2C0_ADDR_HALL_SENSOR},
        .led = {.mux_index = 2, .mux_channel = 3, .tca9554_enable_port = 4, .enable_active_level = TILES_ACTIVE_LOW},
        .haptic = {.pca9685_i2c_addr = TILES_I2C1_ADDR_HAPTIC_PCA9685_2, .channel = 12, .active_level = TILES_ACTIVE_HIGH},
    },
    {
        .logical_pad = 23, .row = 4, .col = 5, .center_x_mm = 39.000f, .center_y_mm = -39.114f, .fpc_index = 23,
        .touch = {.mpr121_i2c_addr = TILES_I2C0_ADDR_TOUCH2, .electrode = 9},
        .hall = {.mux_i2c_addr = TILES_I2C0_ADDR_HALL_MUX3, .mux_channel = 7, .sensor_i2c_addr = TILES_I2C0_ADDR_HALL_SENSOR},
        .led = {.mux_index = 3, .mux_channel = 5, .tca9554_enable_port = 5, .enable_active_level = TILES_ACTIVE_LOW},
        .haptic = {.pca9685_i2c_addr = TILES_I2C1_ADDR_HAPTIC_PCA9685_2, .channel = 10, .active_level = TILES_ACTIVE_HIGH},
    },
    {
        .logical_pad = 24, .row = 4, .col = 6, .center_x_mm = 65.000f, .center_y_mm = -39.114f, .fpc_index = 24,
        .touch = {.mpr121_i2c_addr = TILES_I2C0_ADDR_TOUCH2, .electrode = 11},
        .hall = {.mux_i2c_addr = TILES_I2C0_ADDR_HALL_MUX3, .mux_channel = 0, .sensor_i2c_addr = TILES_I2C0_ADDR_HALL_SENSOR},
        .led = {.mux_index = 3, .mux_channel = 3, .tca9554_enable_port = 5, .enable_active_level = TILES_ACTIVE_LOW},
        .haptic = {.pca9685_i2c_addr = TILES_I2C1_ADDR_HAPTIC_PCA9685_2, .channel = 8, .active_level = TILES_ACTIVE_HIGH},
    },
};

const tiles_pad_config_t *board_pad_config(uint8_t logical_pad) {
    if (logical_pad < 1 || logical_pad > TILES_NUM_PADS) {
        return NULL;
    }
    return &g_tiles_pad_config[logical_pad - 1];
}
