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

/* Gated slow drift tracker -- see docs/architecture/defaults-and-
 * safeguards.md's "Pad baseline calibration and drift compensation"
 * section, which specs exactly this design (this is its first
 * implementation, not a deviation from it). A pad's baseline nudges
 * toward its current reading only while ALL of: untouched, no active
 * note (redundant with "untouched" given services/expression.c's tight
 * touch/note coupling -- a note is never active on a pad that reads
 * untouched in this codebase, so checking touched alone already
 * captures both conditions), and the reading has been consistent
 * (within DRIFT_NOISE_THRESHOLD of the previous background read, not a
 * fixed anchor -- letting the comparison point itself slide is what
 * lets genuine slow drift accumulate over many readings without ever
 * looking "unstable" in any single step) for DRIFT_DWELL_MS. Only
 * touches pads read via the background round-robin pass in
 * tiles_hall_scan() below, since touched pads never reach here anyway. */
#define DRIFT_NOISE_THRESHOLD 8    /* raw Z counts a step can move and still count as "stable" */
#define DRIFT_DWELL_MS 400u        /* how long a stable streak must hold before nudging starts */
#define DRIFT_SLEW_DENOMINATOR 128 /* nudge by ~1/128 (under 1%) of the remaining gap per qualifying read, not a snap */

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

static void reset_drift_tracker(uint8_t pad_index) {
    s_pad_drift_ref_valid[pad_index] = false;
    s_pad_drift_last_z[pad_index] = 0;
    s_pad_drift_stable_since_ms[pad_index] = 0;
}

/* Called only for a pad just read via the background (untouched)
 * round-robin pass -- see this file's header comment above for the
 * full gating design. Nudges s_pad_baseline_z toward the current
 * reading once it's held consistent long enough; otherwise just tracks
 * the running "last stable read" state for next time. */
static void update_drift_tracker(uint8_t pad_index) {
    if (!s_pad_init_ok[pad_index] || !s_pad_sample[pad_index].valid) {
        return;
    }
    if (tiles_touch_is_touched((uint8_t)(pad_index + 1u))) {
        /* Shouldn't happen (background pass only reads untouched pads),
         * but if it ever did, never drift-track while touched -- reset
         * rather than let a stale streak resume once released. */
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
    /* The comparison point slides to this reading either way (whether
     * or not it counted as stable) -- see the header comment on why a
     * sliding reference, not a fixed one, is what lets real slow drift
     * accumulate over many readings without ever looking like a single
     * disqualifying jump. */
    s_pad_drift_last_z[pad_index] = z;

    if (step > DRIFT_NOISE_THRESHOLD) {
        s_pad_drift_stable_since_ms[pad_index] = now_ms;
        return;
    }

    if (now_ms - s_pad_drift_stable_since_ms[pad_index] < DRIFT_DWELL_MS) {
        return;
    }

    /* Stable long enough -- nudge, don't snap. Once due, guarantee at
     * least 1 count of progress even on a tiny remaining gap, so a
     * long-stable pad with just a few counts left to close doesn't
     * stall forever at integer-division-truncates-to-zero. */
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
        update_drift_tracker(pad_index);
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
            /* Fresh baseline -- start the drift tracker clean rather
             * than let stale pre-recapture stability state immediately
             * nudge away from the value just forced here. */
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
