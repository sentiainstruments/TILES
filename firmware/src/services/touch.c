#include "touch.h"

#include "board_pins.h"
#include "pad_config.h"

#include "mpr121.h"
#include "lighting.h"

static tiles_mpr121_t s_touch1; /* TILES_I2C0_ADDR_TOUCH1 */
static tiles_mpr121_t s_touch2; /* TILES_I2C0_ADDR_TOUCH2 */
static bool s_touch1_ok;
static bool s_touch2_ok;
static bool s_pad_touched[TILES_NUM_PADS];

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

        /* MIDI note on/off/velocity/aftertouch are owned by
         * services/expression.c, which reads tiles_touch_is_touched()
         * itself -- this module is touch state + lighting only. */
        s_pad_touched[i] = touched;
        tiles_lighting_set_pad_press(cfg->logical_pad, touched ? 1.0f : 0.0f);
    }
}

bool tiles_touch_is_touched(uint8_t logical_pad) {
    if (logical_pad < 1u || logical_pad > TILES_NUM_PADS) {
        return false;
    }
    return s_pad_touched[logical_pad - 1u];
}
