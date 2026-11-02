#include "touch.h"

#include "board_pins.h"
#include "pad_config.h"

#include "lighting.h"
#include "mpr121.h"

#include "pico/time.h"

#include <stdio.h>

static tiles_mpr121_t s_touch1; /* TILES_I2C0_ADDR_TOUCH1 */
static tiles_mpr121_t s_touch2; /* TILES_I2C0_ADDR_TOUCH2 */
static bool s_touch1_ok;
static bool s_touch2_ok;
static bool s_pad_touched[TILES_NUM_PADS];

/* Upkeep (touch_upkeep()): each controller is checked every
 * TOUCH_CHECK_MS (one register read); one that fails two checks in a row
 * (reset, or never set up) is set up again. Two, so a single bad read
 * can't reset a working controller under the fingers. */
#define TOUCH_CHECK_MS 1000u
static uint32_t s_next_check_ms;
static uint8_t s_failed_checks[2];
static tiles_touch_info_t s_info;
static void touch_upkeep(uint32_t now_ms);

bool tiles_touch_init(void) {
    s_touch1_ok = tiles_mpr121_init(&s_touch1, i2c0, TILES_I2C0_ADDR_TOUCH1);
    s_touch2_ok = tiles_mpr121_init(&s_touch2, i2c0, TILES_I2C0_ADDR_TOUCH2);

    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_pad_touched[i] = false;
    }

    return s_touch1_ok && s_touch2_ok;
}

void tiles_touch_scan(void) {
    uint16_t mask1 = 0;
    uint16_t mask2 = 0;
    bool ok1 = false;
    bool ok2 = false;

    if (s_touch1_ok) {
        mask1 = tiles_mpr121_read_touched(&s_touch1, &ok1);
    }
    if (s_touch2_ok) {
        mask2 = tiles_mpr121_read_touched(&s_touch2, &ok2);
    }

    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        const tiles_pad_config_t *cfg = board_pad_config((uint8_t)(i + 1u));
        if (cfg == NULL) {
            continue;
        }

        bool touched = false;
        if (cfg->touch.mpr121_i2c_addr == TILES_I2C0_ADDR_TOUCH1 && ok1) {
            touched = (mask1 & (1u << cfg->touch.electrode)) != 0;
        } else if (cfg->touch.mpr121_i2c_addr == TILES_I2C0_ADDR_TOUCH2 && ok2) {
            touched = (mask2 & (1u << cfg->touch.electrode)) != 0;
        }

        /* Notes are services/expression.c's job; it reads
         * tiles_touch_is_touched(). */
        s_pad_touched[i] = touched;
        tiles_lighting_set_pad_press(cfg->logical_pad, touched ? 1.0f : 0.0f);
    }
    touch_upkeep(to_ms_since_boot(get_absolute_time()));
}

static void touch_upkeep(uint32_t now_ms) {
    if ((int32_t)(now_ms - s_next_check_ms) >= 0) {
        s_next_check_ms = now_ms + TOUCH_CHECK_MS;
        tiles_mpr121_t *chips[2] = {&s_touch1, &s_touch2};
        bool *oks[2] = {&s_touch1_ok, &s_touch2_ok};
        static const uint8_t addrs[2] = {TILES_I2C0_ADDR_TOUCH1, TILES_I2C0_ADDR_TOUCH2};
        for (uint8_t c = 0; c < 2u; c++) {
            if (*oks[c] && tiles_mpr121_running(chips[c])) {
                s_failed_checks[c] = 0u;
                continue;
            }
            if (*oks[c] && ++s_failed_checks[c] < 2u) {
                continue;
            }
            s_failed_checks[c] = 0u;
            bool was_ok = *oks[c];
            *oks[c] = tiles_mpr121_init(chips[c], i2c0, addrs[c]);
            if (*oks[c]) {
                s_info.recoveries++;
                printf("[touch] controller 0x%02X %s; set up again\n", (unsigned)addrs[c],
                       was_ok ? "had reset" : "came back");
            }
        }
    }
}

tiles_touch_info_t tiles_touch_get_info(void) {
    return s_info;
}

bool tiles_touch_is_touched(uint8_t logical_pad) {
    if (logical_pad < 1u || logical_pad > TILES_NUM_PADS) {
        return false;
    }
    return s_pad_touched[logical_pad - 1u];
}
