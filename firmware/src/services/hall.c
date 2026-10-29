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

/* Slow drift tracker (docs/architecture/defaults-and-safeguards.md "Pad
 * baseline calibration and drift compensation"). A pad's baseline creeps
 * toward its reading only while it is untouched (which also means no note)
 * and each read stays within DRIFT_NOISE_THRESHOLD of the PREVIOUS one for
 * DRIFT_DWELL_MS. The reference slides with every read, so slow drift
 * accumulates without any single step looking unstable. Only background
 * (untouched) reads get here. */
#define DRIFT_NOISE_THRESHOLD 8    /* raw Z counts a step may move and still be "stable" */
#define DRIFT_DWELL_MS 400u        /* how long a stable streak must last before nudging */
#define DRIFT_SLEW_DENOMINATOR 128 /* nudge ~1/128 of the gap per qualifying read, never snap */

static int16_t s_pad_drift_last_z[TILES_NUM_PADS];
static uint32_t s_pad_drift_stable_since_ms[TILES_NUM_PADS];
static bool s_pad_drift_ref_valid[TILES_NUM_PADS];

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

/* Disables every Hall channel, then opens only this pad's channel, so at
 * most one channel across the three muxes is open. */
static bool select_pad(const tiles_pad_config_t *cfg) {
    disable_all_hall_muxes();

    int idx = mux_index_for_addr(cfg->hall.mux_i2c_addr);
    if (idx < 0) {
        return false;
    }
    return tiles_tca9548a_select_channel(&s_hall_muxes[idx], cfg->hall.mux_channel);
}

/* Select, read, deselect one pad; stores the timestamped sample. Used by
 * both the touched-pad pass and the round-robin. */
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

static void reset_drift_tracker(uint8_t pad_index) {
    s_pad_drift_ref_valid[pad_index] = false;
    s_pad_drift_last_z[pad_index] = 0;
    s_pad_drift_stable_since_ms[pad_index] = 0;
}

/* For a pad just read by the background pass: nudges the baseline once
 * the reading has held steady long enough, else updates the streak state. */
static void update_drift_tracker(uint8_t pad_index) {
    if (!s_pad_init_ok[pad_index] || !s_pad_sample[pad_index].valid) {
        return;
    }
    if (tiles_touch_is_touched((uint8_t)(pad_index + 1u))) {
        /* Can't normally happen (background reads are untouched pads); if it
         * does, reset so a stale streak doesn't resume after release. */
        reset_drift_tracker(pad_index);
        return;
    }

    int16_t z = s_pad_sample[pad_index].z;
    uint32_t now_ms = s_pad_sample[pad_index].sample_time_ms;

    if (!s_pad_drift_ref_valid[pad_index]) {
        s_pad_drift_last_z[pad_index] = z;
        s_pad_drift_stable_since_ms[pad_index] = now_ms;
        s_pad_drift_ref_valid[pad_index] = true;
        return;
    }

    int16_t step = (int16_t)(z - s_pad_drift_last_z[pad_index]);
    if (step < 0) {
        step = (int16_t)(-step);
    }
    /* The reference slides to this reading whether or not it was stable (see
     * above). */
    s_pad_drift_last_z[pad_index] = z;

    if (step > DRIFT_NOISE_THRESHOLD) {
        s_pad_drift_stable_since_ms[pad_index] = now_ms;
        return;
    }

    if (now_ms - s_pad_drift_stable_since_ms[pad_index] < DRIFT_DWELL_MS) {
        return;
    }

    /* Stable long enough: nudge, don't snap, but by at least 1 count so a
     * small remaining gap doesn't stall at zero from integer division. */
    int32_t gap = (int32_t)z - (int32_t)s_pad_baseline_z[pad_index];
    int32_t nudge = gap / (int32_t)DRIFT_SLEW_DENOMINATOR;
    if (nudge == 0 && gap != 0) {
        nudge = (gap > 0) ? 1 : -1;
    }
    s_pad_baseline_z[pad_index] = (int16_t)(s_pad_baseline_z[pad_index] + nudge);
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
        reset_drift_tracker(i);

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

            /* Baseline: this pad is assumed at rest now (see the header). */
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
    /* Priority pass: every touched, initialized pad is read this call. */
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        if (s_pad_init_ok[i] && tiles_touch_is_touched((uint8_t)(i + 1u))) {
            read_pad(i);
        }
    }

    /* Background pass: one untouched, initialized pad per call. */
    for (uint8_t attempts = 0; attempts < TILES_NUM_PADS; attempts++) {
        uint8_t pad_index = s_scan_cursor;
        s_scan_cursor = (uint8_t)((s_scan_cursor + 1u) % TILES_NUM_PADS);

        if (!s_pad_init_ok[pad_index] || tiles_touch_is_touched((uint8_t)(pad_index + 1u))) {
            continue; /* covered by the priority pass, or not initialized */
        }

        read_pad(pad_index);
        update_drift_tracker(pad_index);
        return;
    }
    /* Everything touched or nothing initialized: no background read this call. */
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
            /* Fresh baseline: restart the drift tracker so stale streak state can't
             * nudge it away immediately. */
            reset_drift_tracker(i);
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
