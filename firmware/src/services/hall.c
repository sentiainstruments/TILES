#include "hall.h"

#include "board_pins.h"
#include "pad_config.h"

#include "tca9548a.h"
#include "tmag5273.h"

#include "touch.h"

#include "pico/time.h"

static tiles_tca9548a_t s_hall_muxes[TILES_NUM_HALL_MUXES]; /* index 0/1/2 = mux 1/2/3 */
static bool s_pad_init_ok[TILES_NUM_PADS];
static tiles_hall_sample_t s_pad_sample[TILES_NUM_PADS];
static int16_t s_pad_baseline_z[TILES_NUM_PADS];
static uint8_t s_scan_cursor;

static int mux_index_for_addr(uint8_t mux_i2c_addr) {
    if (mux_i2c_addr == TILES_I2C0_ADDR_HALL_MUX1) {
        return 0;
    }
    if (mux_i2c_addr == TILES_I2C0_ADDR_HALL_MUX2) {
        return 1;
    }
    if (mux_i2c_addr == TILES_I2C0_ADDR_HALL_MUX3) {
        return 2;
    }
    return -1;
}

static void disable_all_hall_muxes(void) {
    for (uint8_t i = 0; i < TILES_NUM_HALL_MUXES; i++) {
        tiles_tca9548a_disable_all(&s_hall_muxes[i]);
    }
}

/* Disables every Hall mux channel, then enables exactly this pad's
 * channel on its one mux -- the other two muxes stay disabled, so at
 * most one channel across all three is ever open at once. */
static bool select_pad(const tiles_pad_config_t *cfg) {
    disable_all_hall_muxes();

    int idx = mux_index_for_addr(cfg->hall.mux_i2c_addr);
    if (idx < 0) {
        return false;
    }
    return tiles_tca9548a_select_channel(&s_hall_muxes[idx], cfg->hall.mux_channel);
}

/* Selects, reads, and deselects one pad's sensor, recording the result
 * (with a timestamp) into s_pad_sample. Used by both the touched-pad
 * priority pass and the background round-robin. */
static void read_pad(uint8_t pad_index /* 0-23 */) {
    const tiles_pad_config_t *cfg = board_pad_config((uint8_t)(pad_index + 1u));
    if (cfg == NULL || !select_pad(cfg)) {
        s_pad_sample[pad_index].valid = false;
        disable_all_hall_muxes();
        return;
    }

    tiles_tmag5273_t dev = {.bus = i2c0, .addr = cfg->hall.sensor_i2c_addr};
    tiles_tmag5273_sample_t raw;
    bool ok = tiles_tmag5273_read_xyz(&dev, &raw);

    disable_all_hall_muxes();

    if (ok) {
        s_pad_sample[pad_index].x = raw.x;
        s_pad_sample[pad_index].y = raw.y;
        s_pad_sample[pad_index].z = raw.z;
        s_pad_sample[pad_index].sample_time_ms = to_ms_since_boot(get_absolute_time());
    }
    s_pad_sample[pad_index].valid = ok;
}

bool tiles_hall_init(void) {
    tiles_tca9548a_init(&s_hall_muxes[0], i2c0, TILES_I2C0_ADDR_HALL_MUX1);
    tiles_tca9548a_init(&s_hall_muxes[1], i2c0, TILES_I2C0_ADDR_HALL_MUX2);
    tiles_tca9548a_init(&s_hall_muxes[2], i2c0, TILES_I2C0_ADDR_HALL_MUX3);
    disable_all_hall_muxes();

    bool all_ok = true;
    s_scan_cursor = 0;

    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_pad_sample[i] = (tiles_hall_sample_t){0};
        s_pad_init_ok[i] = false;
        s_pad_baseline_z[i] = 0;

        const tiles_pad_config_t *cfg = board_pad_config((uint8_t)(i + 1u));
        if (cfg == NULL || !select_pad(cfg)) {
            all_ok = false;
            disable_all_hall_muxes();
            continue;
        }

        bool ok = tiles_tmag5273_identify(i2c0, cfg->hall.sensor_i2c_addr);
        if (ok) {
            tiles_tmag5273_t dev;
            ok = tiles_tmag5273_init(&dev, i2c0, cfg->hall.sensor_i2c_addr);

            /* Baseline capture: this pad is assumed at rest right now
             * (see the header comment's caveat about power-on-time
             * assembly state). */
            if (ok) {
                tiles_tmag5273_sample_t raw;
                if (tiles_tmag5273_read_xyz(&dev, &raw)) {
                    s_pad_baseline_z[i] = raw.z;
                }
            }
        }

        s_pad_init_ok[i] = ok;
        all_ok = all_ok && ok;

        disable_all_hall_muxes();
    }

    return all_ok;
}

bool tiles_hall_last_init_ok(uint8_t logical_pad) {
    if (logical_pad < 1u || logical_pad > TILES_NUM_PADS) {
        return false;
    }
    return s_pad_init_ok[logical_pad - 1u];
}

void tiles_hall_scan(void) {
    /* Priority pass: every currently-touched, successfully-initialized
     * pad gets read this call -- see the header comment for why a pure
     * round-robin can't sample fast enough to catch a strike. */
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        if (s_pad_init_ok[i] && tiles_touch_is_touched((uint8_t)(i + 1u))) {
            read_pad(i);
        }
    }

    /* Background pass: advance the round-robin by exactly one
     * untouched, initialized pad, so idle pads still get periodic
     * coverage without competing with the priority pass above. */
    for (uint8_t attempts = 0; attempts < TILES_NUM_PADS; attempts++) {
        uint8_t pad_index = s_scan_cursor;
        s_scan_cursor = (uint8_t)((s_scan_cursor + 1u) % TILES_NUM_PADS);

        if (!s_pad_init_ok[pad_index] || tiles_touch_is_touched((uint8_t)(pad_index + 1u))) {
            continue; /* already covered by the priority pass, or not initialized */
        }

        read_pad(pad_index);
        return;
    }
    /* No untouched, initialized pad found (either everything is
     * touched right now, or nothing initialized) -- nothing to do for
     * the background pass this call. */
}

tiles_hall_sample_t tiles_hall_get_sample(uint8_t logical_pad) {
    if (logical_pad < 1u || logical_pad > TILES_NUM_PADS) {
        return (tiles_hall_sample_t){0};
    }
    return s_pad_sample[logical_pad - 1u];
}

bool tiles_hall_recapture_baseline(void) {
    bool all_ok = true;
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        if (!s_pad_init_ok[i]) {
            continue;
        }
        read_pad(i);
        if (s_pad_sample[i].valid) {
            s_pad_baseline_z[i] = s_pad_sample[i].z;
        } else {
            all_ok = false;
        }
    }
    return all_ok;
}

uint16_t tiles_hall_get_depth(uint8_t logical_pad) {
    if (logical_pad < 1u || logical_pad > TILES_NUM_PADS) {
        return 0u;
    }
    uint8_t i = (uint8_t)(logical_pad - 1u);
    if (!s_pad_init_ok[i] || !s_pad_sample[i].valid) {
        return 0u;
    }
    int32_t delta = (int32_t)s_pad_sample[i].z - (int32_t)s_pad_baseline_z[i];
    return (uint16_t)(delta < 0 ? -delta : delta);
}
