/*
 * Host-buildable integrity test for the canonical pad table.
 *
 * Per SENTIA_FIRMWARE_CODEX_START.md: before writing implementation code,
 * assert that all 24 touch, Hall, LED, haptic and FPC routes are unique.
 * No Pico SDK / hardware dependency -- build and run directly:
 *
 *   cc -std=c11 -I../src/board test_pad_config.c ../src/board/pad_config.c -o /tmp/test_pad_config
 *   /tmp/test_pad_config
 */

#include <stdio.h>

#include "pad_config.h"

static int g_failures = 0;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL: " __VA_ARGS__);                           \
            fprintf(stderr, "\n");                                           \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static void check_logical_pad_sequence(void) {
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        const tiles_pad_config_t *p = &g_tiles_pad_config[i];
        CHECK(p->logical_pad == i + 1, "pad index %u has logical_pad=%u, expected %u", i, p->logical_pad, i + 1);

        uint8_t expected_row = (uint8_t)(i / 6) + 1;
        uint8_t expected_col = (uint8_t)(i % 6) + 1;
        CHECK(p->row == expected_row, "pad %u has row=%u, expected %u", p->logical_pad, p->row, expected_row);
        CHECK(p->col == expected_col, "pad %u has col=%u, expected %u", p->logical_pad, p->col, expected_col);

        uint8_t expected_fpc = i + 1;
        CHECK(p->fpc_index == expected_fpc, "pad %u has fpc_index=%u, expected %u", p->logical_pad, p->fpc_index, expected_fpc);
    }
}

static void check_touch_routes_unique(void) {
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        for (uint8_t j = i + 1; j < TILES_NUM_PADS; j++) {
            const tiles_touch_route_t *a = &g_tiles_pad_config[i].touch;
            const tiles_touch_route_t *b = &g_tiles_pad_config[j].touch;
            CHECK(!(a->mpr121_i2c_addr == b->mpr121_i2c_addr && a->electrode == b->electrode),
                  "pads %u and %u share touch route (addr=0x%02X electrode=%u)",
                  g_tiles_pad_config[i].logical_pad, g_tiles_pad_config[j].logical_pad, a->mpr121_i2c_addr, a->electrode);
        }
        CHECK(g_tiles_pad_config[i].touch.electrode <= 11, "pad %u touch electrode %u out of range 0-11",
              g_tiles_pad_config[i].logical_pad, g_tiles_pad_config[i].touch.electrode);
    }
}

static void check_hall_routes_unique(void) {
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        for (uint8_t j = i + 1; j < TILES_NUM_PADS; j++) {
            const tiles_hall_route_t *a = &g_tiles_pad_config[i].hall;
            const tiles_hall_route_t *b = &g_tiles_pad_config[j].hall;
            CHECK(!(a->mux_i2c_addr == b->mux_i2c_addr && a->mux_channel == b->mux_channel),
                  "pads %u and %u share Hall route (mux=0x%02X channel=%u)",
                  g_tiles_pad_config[i].logical_pad, g_tiles_pad_config[j].logical_pad, a->mux_i2c_addr, a->mux_channel);
        }
        CHECK(g_tiles_pad_config[i].hall.sensor_i2c_addr == TILES_I2C0_ADDR_HALL_SENSOR,
              "pad %u Hall sensor address 0x%02X does not match the shared TMAG5273 address",
              g_tiles_pad_config[i].logical_pad, g_tiles_pad_config[i].hall.sensor_i2c_addr);
        CHECK(g_tiles_pad_config[i].hall.mux_channel <= 7, "pad %u Hall mux channel %u out of range 0-7",
              g_tiles_pad_config[i].logical_pad, g_tiles_pad_config[i].hall.mux_channel);
    }
}

static void check_led_routes_unique(void) {
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        for (uint8_t j = i + 1; j < TILES_NUM_PADS; j++) {
            const tiles_led_route_t *a = &g_tiles_pad_config[i].led;
            const tiles_led_route_t *b = &g_tiles_pad_config[j].led;
            CHECK(!(a->mux_index == b->mux_index && a->mux_channel == b->mux_channel),
                  "pads %u and %u share LED route (mux=%u channel=%u)",
                  g_tiles_pad_config[i].logical_pad, g_tiles_pad_config[j].logical_pad, a->mux_index, a->mux_channel);
        }
        const tiles_led_route_t *led = &g_tiles_pad_config[i].led;
        CHECK(led->mux_index >= 1 && led->mux_index <= TILES_NUM_LED_MUXES, "pad %u LED mux_index %u out of range 1-3",
              g_tiles_pad_config[i].logical_pad, led->mux_index);
        CHECK(led->mux_channel <= 7, "pad %u LED mux_channel %u out of range 0-7", g_tiles_pad_config[i].logical_pad, led->mux_channel);
        /* mux N always gates through TCA9554 port (N + 2), per the board map. */
        CHECK(led->tca9554_enable_port == (uint8_t)(led->mux_index + 2),
              "pad %u LED enable_port %u does not match mux_index %u (expected %u)",
              g_tiles_pad_config[i].logical_pad, led->tca9554_enable_port, led->mux_index, led->mux_index + 2);
        CHECK(led->enable_active_level == TILES_ACTIVE_LOW, "pad %u LED enable_active_level should be active-low",
              g_tiles_pad_config[i].logical_pad);
    }
}

static void check_haptic_routes_unique(void) {
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        for (uint8_t j = i + 1; j < TILES_NUM_PADS; j++) {
            const tiles_haptic_route_t *a = &g_tiles_pad_config[i].haptic;
            const tiles_haptic_route_t *b = &g_tiles_pad_config[j].haptic;
            CHECK(!(a->pca9685_i2c_addr == b->pca9685_i2c_addr && a->channel == b->channel),
                  "pads %u and %u share haptic route (addr=0x%02X channel=%u)",
                  g_tiles_pad_config[i].logical_pad, g_tiles_pad_config[j].logical_pad, a->pca9685_i2c_addr, a->channel);
        }
        CHECK(g_tiles_pad_config[i].haptic.channel <= 15, "pad %u haptic channel %u out of range 0-15",
              g_tiles_pad_config[i].logical_pad, g_tiles_pad_config[i].haptic.channel);
        CHECK(g_tiles_pad_config[i].haptic.active_level == TILES_ACTIVE_HIGH, "pad %u haptic active_level should be active-high",
              g_tiles_pad_config[i].logical_pad);
    }
}

static void check_fpc_and_notes_unique(void) {
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        for (uint8_t j = i + 1; j < TILES_NUM_PADS; j++) {
            CHECK(g_tiles_pad_config[i].fpc_index != g_tiles_pad_config[j].fpc_index,
                  "pads %u and %u share fpc_index %u", g_tiles_pad_config[i].logical_pad,
                  g_tiles_pad_config[j].logical_pad, g_tiles_pad_config[i].fpc_index);
            CHECK(g_tiles_pad_config[i].demo_chromatic_note != g_tiles_pad_config[j].demo_chromatic_note,
                  "pads %u and %u share demo_chromatic_note %u", g_tiles_pad_config[i].logical_pad,
                  g_tiles_pad_config[j].logical_pad, g_tiles_pad_config[i].demo_chromatic_note);
        }
    }
}

static void check_board_pad_config_accessor(void) {
    CHECK(board_pad_config(0) == NULL, "board_pad_config(0) should return NULL");
    CHECK(board_pad_config(25) == NULL, "board_pad_config(25) should return NULL");
    for (uint8_t pad = 1; pad <= TILES_NUM_PADS; pad++) {
        const tiles_pad_config_t *cfg = board_pad_config(pad);
        CHECK(cfg != NULL && cfg->logical_pad == pad, "board_pad_config(%u) did not return the matching entry", pad);
    }
}

int main(void) {
    check_logical_pad_sequence();
    check_touch_routes_unique();
    check_hall_routes_unique();
    check_led_routes_unique();
    check_haptic_routes_unique();
    check_fpc_and_notes_unique();
    check_board_pad_config_accessor();

    if (g_failures == 0) {
        printf("PASS: pad table integrity (%u pads checked)\n", (unsigned)TILES_NUM_PADS);
        return 0;
    }

    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
}
