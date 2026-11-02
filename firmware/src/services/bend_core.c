#include "bend_core.h"

#include <math.h>

/* Uses the field's direction (component / |B|), which cancels depth to
 * first order. Magnitude from X and Y, sign from X alone. */

/* Deadzone, as a soft knee. Straight presses moved the cosine up to ~0.05;
 * depth-gated recentring removed most of that, so 0.025. Changed alone:
 * revert first if plain presses start bending. */
#define BEND_DEADZONE_COSINE_DELTA 0.025f

/* Settle time before the bend baseline is taken (bend stays centred): 25 ms
 * read the transient as bend, 250 ms started bends late. Unmeasured. */
#define BEND_SETTLE_MS 120u

/* Slow baseline recentring toward the smoothed X/Y while no bend run is
 * active, snapping fast while depth changes (pressure only couples into
 * tilt then). A bend held many seconds may relax. Unmeasured. */
#define BEND_BASELINE_RECENTER_ALPHA_SLOW 0.002f
/* Baseline rate while depth is actively changing (depth_activity ~1): a
 * bit faster than one smoothing stage (0.08), so it keeps up with a strike
 * without being as noisy as raw samples. Unmeasured. */
#define BEND_BASELINE_RECENTER_ALPHA_FAST 0.25f
/* Smoothed signed depth rate that counts as fully pressing (signed: depth
 * wobbles during a tilt). First guess. */
#define BEND_DEPTH_ACTIVITY_FULL_SCALE 8.0f

/* A deviation ramps to full weight over this long, so mechanical give
 * (flickers within 100-200 ms) stays small. Unmeasured; lower only with a
 * fresh capture. */
#define BEND_ARM_MS 30u

/* Two-stage EMA on X/Y/magnitude against 6-15 Hz tremor. Invariant: the
 * live delta and the recentre target read the same, fully cascaded stage
 * (splitting it, twice, brought back pressure bend). */
#define BEND_SMOOTHING_ALPHA 0.08f

/* Separate vibrato detector: reads the gap between the two smoothing stages
 * and adds on top, leaving the tilt path alone. The smoothing kills fast
 * wiggles (12% of a tilt's bend); captured gap: still ~7.5, tilt ~8.5, fast
 * wiggle ~16. */
#define VIBRATO_ENERGY_NOISE_FLOOR 8.0f
/* Energy for full vibrato depth (a real wiggle sustains ~16). 8..16 is a
 * soft knee, so a borderline gesture fades in rather than clicking on. */
#define VIBRATO_ENERGY_FULL_SCALE 16.0f
/* Wiggle energy must stay above the floor this long first (a released tilt
 * blips for one tick). */
#define VIBRATO_ARM_MS 80u
/* Fixed rate, not tracked from the gesture (too little signal for a
 * stable estimate). 5.5 Hz is inside natural string/voice vibrato
 * (4-8 Hz). */
#define VIBRATO_RATE_HZ 5.5f
/* Peak added depth in wire units: ~0.9 semitone (after the wire
 * compensation, 12 semitones ~ 2047 units, ~171 per semitone). A design
 * choice; tune by ear. */
#define VIBRATO_MAX_WIRE_DEPTH 150.0f
/* Local pi: M_PI isn't guaranteed on this newlib target. */
#define BEND_PI 3.14159265358979323846f

/* component / |B|; a near-zero |B| returns 0. */
static float direction_cosine_from(float x_component, float magnitude) {
    if (magnitude < 1.0f) {
        return 0.0f;
    }
    return x_component / magnitude;
}

/* 0..1 over BEND_ARM_MS of a run, then 1. `hold_ms` = how long the current
 * run has lasted (0 = none). */
static float confidence_multiplier(uint32_t hold_ms) {
    if (hold_ms >= BEND_ARM_MS) {
        return 1.0f;
    }
    return (float)hold_ms / (float)BEND_ARM_MS;
}

/* Cosine delta -> 14-bit bend: soft-knee deadzone, then the confidence
 * ramp; linear on purpose (an ease-out curve amplified the ramp and
 * steepened the low end). Max tilt = the declared range even on a receiver
 * stuck at 48 (common synths ignore RPN 0). Updates the run tracking. */
static uint16_t bend_14bit_from_cosine_delta(tiles_bend_t *b, const tiles_bend_config_t *cfg, float delta,
                                             uint32_t now_ms) {
    float magnitude = fabsf(delta);
    bool positive = delta >= 0.0f;
    float sign = positive ? 1.0f : -1.0f;
    if (magnitude <= BEND_DEADZONE_COSINE_DELTA) {
        b->run_active = false;
        magnitude = 0.0f;
    } else {
        /* A run starts on the first deviation past the deadzone, not on each sign
         * flip (a held tilt flips constantly); only the deadzone ends it. */
        if (!b->run_active) {
            b->run_active = true;
            b->run_start_ms = now_ms;
        }
        b->run_positive = positive;
        magnitude -= BEND_DEADZONE_COSINE_DELTA;
    }

    float usable_range = cfg->max_cosine_deviation - BEND_DEADZONE_COSINE_DELTA;
    if (usable_range < 0.01f) {
        /* Sensitivity at or below the deadzone: keep a small range. */
        usable_range = 0.01f;
    }
    float normalized = sign * (magnitude / usable_range);

    uint32_t hold_ms = b->run_active ? (now_ms - b->run_start_ms) : 0u;
    normalized *= confidence_multiplier(hold_ms);

    if (normalized > 1.0f) {
        normalized = 1.0f;
    }
    if (normalized < -1.0f) {
        normalized = -1.0f;
    }
    float wire_compensation = cfg->wire_range_semitones / TILES_BEND_RECEIVER_DEFAULT_RANGE_SEMITONES;
    int32_t bend = (int32_t)b->center + (int32_t)(normalized * 8191.0f * wire_compensation);
    if (bend < 0) {
        bend = 0;
    }
    if (bend > 16383) {
        bend = 16383;
    }
    return (uint16_t)bend;
}

/* Adds the vibrato on top of an already computed bend. */
static uint16_t apply_vibrato(tiles_bend_t *b, uint16_t bend, uint32_t now_ms) {
    bool above_floor = b->wiggle_energy > VIBRATO_ENERGY_NOISE_FLOOR;
    if (above_floor) {
        if (!b->wiggle_active) {
            b->wiggle_active = true;
            b->wiggle_start_ms = now_ms;
        }
    } else {
        b->wiggle_active = false;
    }

    uint32_t hold_ms = b->wiggle_active ? (now_ms - b->wiggle_start_ms) : 0u;
    float confirm = (hold_ms >= VIBRATO_ARM_MS) ? 1.0f : (float)hold_ms / (float)VIBRATO_ARM_MS;

    float energy_above_floor = b->wiggle_energy - VIBRATO_ENERGY_NOISE_FLOOR;
    if (energy_above_floor < 0.0f) {
        energy_above_floor = 0.0f;
    }
    float depth = energy_above_floor / (VIBRATO_ENERGY_FULL_SCALE - VIBRATO_ENERGY_NOISE_FLOOR);
    if (depth > 1.0f) {
        depth = 1.0f;
    }
    depth *= confirm;
    if (depth <= 0.0f) {
        return bend;
    }

    /* Integer phase first: uptime in ms as a float drifts the LFO. */
    uint32_t period_ms = (uint32_t)(1000.0f / VIBRATO_RATE_HZ);
    uint32_t phase_ms = now_ms % period_ms;
    float lfo = sinf(2.0f * BEND_PI * (float)phase_ms / (float)period_ms);

    int32_t with_vibrato = (int32_t)bend + (int32_t)(depth * VIBRATO_MAX_WIRE_DEPTH * lfo);
    if (with_vibrato < 0) {
        with_vibrato = 0;
    }
    if (with_vibrato > 16383) {
        with_vibrato = 16383;
    }
    return (uint16_t)with_vibrato;
}

void tiles_bend_start(tiles_bend_t *b, int16_t x, int16_t y, int16_t z, float depth, uint16_t center,
                      uint32_t now_ms) {
    float fx = (float)x;
    float fy = (float)y;
    float fz = (float)z;
    float magnitude = sqrtf(fx * fx + fy * fy + fz * fz);
    b->center = center;
    b->x1 = fx;
    b->x2 = fx;
    b->y1 = fy;
    b->y2 = fy;
    b->magnitude = magnitude;
    b->run_magnitude = magnitude;
    b->prev_depth = depth;
    b->depth_rate = 0.0f;
    b->delta = 0.0f;
    b->settled = false;
    b->start_ms = now_ms;
    b->run_active = false;
    b->wiggle_energy = 0.0f;
    b->wiggle_active = false;
}

bool tiles_bend_step(tiles_bend_t *b, const tiles_bend_config_t *cfg, int16_t x, int16_t y, int16_t z,
                     float smoothed_depth, uint32_t now_ms, uint16_t *out_bend) {
    float fx = (float)x;
    float fy = (float)y;
    float fz = (float)z;
    float magnitude = sqrtf(fx * fx + fy * fy + fz * fz);

    b->x1 += BEND_SMOOTHING_ALPHA * (fx - b->x1);
    b->x2 += BEND_SMOOTHING_ALPHA * (b->x1 - b->x2);
    b->y1 += BEND_SMOOTHING_ALPHA * (fy - b->y1);
    b->y2 += BEND_SMOOTHING_ALPHA * (b->y1 - b->y2);
    b->magnitude += BEND_SMOOTHING_ALPHA * (magnitude - b->magnitude);

    /* Vibrato energy from the gap between the two stages, every pass. */
    {
        float wiggle_x = b->x1 - b->x2;
        float wiggle_y = b->y1 - b->y2;
        float wiggle = sqrtf(wiggle_x * wiggle_x + wiggle_y * wiggle_y);
        b->wiggle_energy += BEND_SMOOTHING_ALPHA * (wiggle - b->wiggle_energy);
    }

    b->depth_rate += BEND_SMOOTHING_ALPHA * ((smoothed_depth - b->prev_depth) - b->depth_rate);
    /* 0 (depth steady) .. 1 (changing fast). */
    float depth_activity = fabsf(b->depth_rate) / BEND_DEPTH_ACTIVITY_FULL_SCALE;
    if (depth_activity > 1.0f) {
        depth_activity = 1.0f;
    }
    b->prev_depth = smoothed_depth;

    if (!b->settled) {
        /* Centred until the settle time, then the baseline from the cascade. */
        if ((now_ms - b->start_ms) >= BEND_SETTLE_MS) {
            b->baseline_x = b->x2;
            b->baseline_y = b->y2;
            b->settled = true;
            b->delta = 0.0f;
        }
        return false;
    }

    /* Recentre only while no run is confirmed (last pass's state), blending
     * SLOW..FAST by depth activity, cubed: FAST is ~125x SLOW, and a linear
     * blend let sensor noise erase real tilts. */
    if (!b->run_active) {
        float activity_shaped = depth_activity * depth_activity * depth_activity;
        float recenter_alpha = BEND_BASELINE_RECENTER_ALPHA_SLOW +
                               (BEND_BASELINE_RECENTER_ALPHA_FAST - BEND_BASELINE_RECENTER_ALPHA_SLOW) *
                                   activity_shaped;
        b->baseline_x += recenter_alpha * (b->x2 - b->baseline_x);
        b->baseline_y += recenter_alpha * (b->y2 - b->baseline_y);
        b->run_magnitude = b->magnitude;
    }

    /* Depth compensation: compare against the baseline cosine predicted at the
     * current magnitude, so a pure depth change gives exactly 0. Invariants
     * (each broke things): both terms divide by this pass's same magnitude;
     * both use the fully cascaded x2/y2; during a confirmed run the magnitude
     * is frozen. Sign: predicted - current. */
    float delta_x = direction_cosine_from(b->baseline_x, b->run_magnitude) - direction_cosine_from(b->x2, b->run_magnitude);
    float delta_y = direction_cosine_from(b->baseline_y, b->run_magnitude) - direction_cosine_from(b->y2, b->run_magnitude);
    float combined = sqrtf(delta_x * delta_x + delta_y * delta_y);
    /* No third smoothing stage: it made bends too slow to start. */
    b->delta = (delta_x >= 0.0f) ? combined : -combined;

    uint16_t bend = bend_14bit_from_cosine_delta(b, cfg, b->delta, now_ms);
    *out_bend = apply_vibrato(b, bend, now_ms);
    return true;
}
