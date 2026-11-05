#include "bend_core.h"

#include <math.h>
#include <stddef.h>

/* Bend = how far the field's direction has moved from where the note
 * settled. Magnitude from X and Y, sign from X alone. Three ways to keep
 * depth out of it (tiles_bend_mode_t). */

const char *const TILES_BEND_MODE_NAMES[TILES_BEND_MODE_COUNT] = {"legacy", "cosine", "path"};

/* Deadzone, as a soft knee. Straight presses moved the cosine up to ~0.05;
 * depth-gated recentring removed most of that, so 0.025. Changed alone:
 * revert first if plain presses start bending. */
#define BEND_DEADZONE_COSINE_DELTA 0.025f

/* Settle time before the bend baseline is taken (bend stays centred): 25 ms
 * read the transient as bend, 250 ms started bends late. Unmeasured. */
#define BEND_SETTLE_MS 120u
/* COSINE/PATH: past BEND_SETTLE_MS the baseline also waits for the key to
 * stop moving (the gap between the smoothing stages, against |B|), at most
 * BEND_SETTLE_MAX_MS: a landing finger rocks the key for ~200 ms (unit 2,
 * 2026-11-03), and a baseline taken mid-rock stayed bent all note. First
 * guesses. */
#define BEND_SETTLE_MAX_MS 400u
#define BEND_SETTLE_STILL 0.012f
/* COSINE/PATH: the deadzone widens by this X/Y noise (raw counts) over |B|,
 * so a weak field (light pressure, unit 2's ~1 mT rest) needs a bigger
 * lean. ~X/Y noise at 8x averaging (44 uT ~ 18 counts). */
#define BEND_NOISE_XY 20.0f
/* COSINE/PATH: below this depth, while the key comes up, the bend holds its
 * last value: near rest the direction swings, and unit 2's release tails
 * reached full bend. 0 = off. First guess. */
#define BEND_RELEASE_DEPTH 250.0f
/* Smoothed depth change per pass that counts as coming up. */
#define BEND_RELEASE_RATE (-0.5f)

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

/* Press path learning (PATH). A stroke is learned only if it spans a real
 * press (the strike threshold, raw Z) and stays close to a straight line:
 * RMS off the line ~2.5x the X/Y noise at 1x averaging (~50). First
 * guesses; tighten the residual if averaging goes up. */
#define PATH_STROKE_MIN_SAMPLES 4u
#define PATH_STROKE_MAX_SAMPLES 500u /* a resting finger restarts the stroke */
#define PATH_STROKE_MIN_Z_RANGE 150.0f
#define PATH_STROKE_MAX_RESIDUAL 120.0f
/* Sanity bound on |dX/dZ|: about 2 x offset / gap, so 1.0 is a magnet 2 mm
 * off at a 4 mm gap (the studio's sim reaches 0.44 at 0.8 mm). */
#define PATH_MAX_SLOPE 1.0f
/* Below this many strokes a note uses its own direction at settle (the
 * COSINE assumption) instead. */
#define PATH_MIN_STROKES 3u
/* Strokes are averaged up to this many, then each new one moves the slope
 * by 1/this, so a few tilted strikes can't retrain it. */
#define PATH_AVERAGE_STROKES 16u

const tiles_bend_tuning_t TILES_BEND_TUNING_DEFAULT = {
    .deadzone = BEND_DEADZONE_COSINE_DELTA,
    .settle_ms = BEND_SETTLE_MS,
    .settle_max_ms = BEND_SETTLE_MAX_MS,
    .settle_still = BEND_SETTLE_STILL,
    .noise_xy = BEND_NOISE_XY,
    .release_depth = BEND_RELEASE_DEPTH,
    .smoothing = BEND_SMOOTHING_ALPHA,
    .recenter_slow = BEND_BASELINE_RECENTER_ALPHA_SLOW,
    .recenter_fast = BEND_BASELINE_RECENTER_ALPHA_FAST,
    .depth_activity_full = BEND_DEPTH_ACTIVITY_FULL_SCALE,
    .arm_ms = BEND_ARM_MS,
    .vibrato_floor = VIBRATO_ENERGY_NOISE_FLOOR,
    .vibrato_full = VIBRATO_ENERGY_FULL_SCALE,
    .vibrato_arm_ms = VIBRATO_ARM_MS,
    .vibrato_rate_hz = VIBRATO_RATE_HZ,
    .vibrato_depth = VIBRATO_MAX_WIRE_DEPTH,
    .path_min_z_range = PATH_STROKE_MIN_Z_RANGE,
    .path_max_residual = PATH_STROKE_MAX_RESIDUAL,
    .path_max_slope = PATH_MAX_SLOPE,
    .path_min_strokes = PATH_MIN_STROKES,
    .path_average_strokes = PATH_AVERAGE_STROKES,
};

static const tiles_bend_tuning_t *tuning_of(const tiles_bend_config_t *cfg) {
    return cfg->tuning != NULL ? cfg->tuning : &TILES_BEND_TUNING_DEFAULT;
}

/* component / |B|; a near-zero |B| returns 0. */
static float direction_cosine_from(float x_component, float magnitude) {
    if (magnitude < 1.0f) {
        return 0.0f;
    }
    return x_component / magnitude;
}

/* 0..1 over arm_ms of a run, then 1. `hold_ms` = how long the current run
 * has lasted (0 = none). */
static float confidence_multiplier(uint32_t hold_ms, uint32_t arm_ms) {
    if (hold_ms >= arm_ms) {
        return 1.0f;
    }
    return (float)hold_ms / (float)arm_ms;
}

/* Cosine delta -> 14-bit bend: soft-knee deadzone, then the confidence
 * ramp; linear on purpose (an ease-out curve amplified the ramp and
 * steepened the low end). Max tilt = the declared range even on a receiver
 * stuck at 48 (common synths ignore RPN 0). Updates the run tracking. */
static uint16_t bend_14bit_from_cosine_delta(tiles_bend_t *b, const tiles_bend_config_t *cfg, float delta,
                                             uint32_t now_ms) {
    const tiles_bend_tuning_t *t = tuning_of(cfg);
    float magnitude = fabsf(delta);
    float dead = t->deadzone;
    if (b->mode != TILES_BEND_MODE_LEGACY && t->noise_xy > 0.0f && b->magnitude >= 1.0f) {
        dead += t->noise_xy / b->magnitude; /* the whole curve moves out: same range to full bend */
    }
    bool positive = delta >= 0.0f;
    float sign = positive ? 1.0f : -1.0f;
    if (magnitude <= dead) {
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
        magnitude -= dead;
    }

    float usable_range = cfg->max_cosine_deviation - t->deadzone;
    if (usable_range < 0.01f) {
        /* Sensitivity at or below the deadzone: keep a small range. */
        usable_range = 0.01f;
    }
    float normalized = sign * (magnitude / usable_range);

    uint32_t hold_ms = b->run_active ? (now_ms - b->run_start_ms) : 0u;
    normalized *= confidence_multiplier(hold_ms, t->arm_ms);

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
static uint16_t apply_vibrato(tiles_bend_t *b, const tiles_bend_tuning_t *t, uint16_t bend, uint32_t now_ms) {
    bool above_floor = b->wiggle_energy > t->vibrato_floor;
    if (above_floor) {
        if (!b->wiggle_active) {
            b->wiggle_active = true;
            b->wiggle_start_ms = now_ms;
        }
    } else {
        b->wiggle_active = false;
    }

    uint32_t hold_ms = b->wiggle_active ? (now_ms - b->wiggle_start_ms) : 0u;
    float confirm = (hold_ms >= t->vibrato_arm_ms) ? 1.0f : (float)hold_ms / (float)t->vibrato_arm_ms;

    float energy_above_floor = b->wiggle_energy - t->vibrato_floor;
    if (energy_above_floor < 0.0f) {
        energy_above_floor = 0.0f;
    }
    float span = t->vibrato_full > t->vibrato_floor ? t->vibrato_full - t->vibrato_floor : 1.0f;
    float depth = energy_above_floor / span;
    if (depth > 1.0f) {
        depth = 1.0f;
    }
    depth *= confirm;
    if (depth <= 0.0f || t->vibrato_depth <= 0.0f) {
        return bend;
    }

    /* Integer phase first: uptime in ms as a float drifts the LFO. */
    uint32_t period_ms = t->vibrato_rate_hz > 0.0f ? (uint32_t)(1000.0f / t->vibrato_rate_hz) : 0u;
    if (period_ms == 0u) {
        return bend;
    }
    uint32_t phase_ms = now_ms % period_ms;
    float lfo = sinf(2.0f * BEND_PI * (float)phase_ms / (float)period_ms);

    int32_t with_vibrato = (int32_t)bend + (int32_t)(depth * t->vibrato_depth * lfo);
    if (with_vibrato < 0) {
        with_vibrato = 0;
    }
    if (with_vibrato > 16383) {
        with_vibrato = 16383;
    }
    return (uint16_t)with_vibrato;
}

void tiles_bend_start(tiles_bend_t *b, tiles_bend_mode_t mode, const tiles_press_path_t *path, int16_t x,
                      int16_t y, int16_t z, float depth, uint16_t center, uint32_t now_ms) {
    float fx = (float)x;
    float fy = (float)y;
    float fz = (float)z;
    float magnitude = sqrtf(fx * fx + fy * fy + fz * fz);
    b->mode = (uint8_t)(mode < TILES_BEND_MODE_COUNT ? mode : TILES_BEND_MODE_LEGACY);
    b->center = center;
    if (b->mode == TILES_BEND_MODE_COSINE) {
        fx = direction_cosine_from(fx, magnitude);
        fy = direction_cosine_from(fy, magnitude);
    }
    b->x1 = fx;
    b->x2 = fx;
    b->y1 = fy;
    b->y2 = fy;
    b->z1 = fz;
    b->z2 = fz;
    b->magnitude = magnitude;
    b->run_magnitude = magnitude;
    b->path_strokes = path != NULL ? path->strokes : 0u;
    b->path_bx = path != NULL ? path->bx : 0.0f;
    b->path_by = path != NULL ? path->by : 0.0f;
    b->path_learned = false;
    b->anchor_x = 0.0f;
    b->anchor_y = 0.0f;
    b->prev_depth = depth;
    b->depth_rate = 0.0f;
    b->delta = 0.0f;
    b->settled = false;
    b->start_ms = now_ms;
    b->run_active = false;
    b->wiggle_energy = 0.0f;
    b->wiggle_active = false;
    b->last_bend = center;
}

/* PATH at settle: the slope (learned, or this note's own direction) and
 * the anchor a = x - b*z, then the stages move to the residual, which is 0
 * here by construction. */
static void anchor_path(tiles_bend_t *b, const tiles_bend_tuning_t *t) {
    b->path_learned = b->path_strokes >= t->path_min_strokes;
    if (!b->path_learned) {
        float z = fabsf(b->z2) >= 1.0f ? b->z2 : 1.0f;
        b->path_bx = b->x2 / z;
        b->path_by = b->y2 / z;
    }
    b->anchor_x = b->x2 - b->path_bx * b->z2;
    b->anchor_y = b->y2 - b->path_by * b->z2;
    b->x1 = b->x2 = 0.0f;
    b->y1 = b->y2 = 0.0f;
}

bool tiles_bend_step(tiles_bend_t *b, const tiles_bend_config_t *cfg, int16_t x, int16_t y, int16_t z,
                     float smoothed_depth, uint32_t now_ms, uint16_t *out_bend) {
    const tiles_bend_tuning_t *t = tuning_of(cfg);
    const float alpha = t->smoothing;
    float fx = (float)x;
    float fy = (float)y;
    float fz = (float)z;
    float magnitude = sqrtf(fx * fx + fy * fy + fz * fz);

    /* What the stages smooth: raw X/Y (LEGACY, and PATH until settle), or a
     * per-sample cosine, divided before filtering so |B| can't lag it. */
    float wiggle_scale = 1.0f; /* back to raw units for the vibrato thresholds */
    if (b->mode == TILES_BEND_MODE_COSINE) {
        fx = direction_cosine_from(fx, magnitude);
        fy = direction_cosine_from(fy, magnitude);
        wiggle_scale = b->magnitude;
    } else if (b->mode == TILES_BEND_MODE_PATH) {
        if (b->settled) {
            fx = direction_cosine_from(fx - b->anchor_x - b->path_bx * fz, magnitude);
            fy = direction_cosine_from(fy - b->anchor_y - b->path_by * fz, magnitude);
            wiggle_scale = b->magnitude;
        } else {
            b->z1 += alpha * (fz - b->z1);
            b->z2 += alpha * (b->z1 - b->z2);
        }
    }

    b->x1 += alpha * (fx - b->x1);
    b->x2 += alpha * (b->x1 - b->x2);
    b->y1 += alpha * (fy - b->y1);
    b->y2 += alpha * (b->y1 - b->y2);
    b->magnitude += alpha * (magnitude - b->magnitude);

    /* Vibrato energy from the gap between the two stages, every pass. */
    {
        float wiggle_x = b->x1 - b->x2;
        float wiggle_y = b->y1 - b->y2;
        float wiggle = sqrtf(wiggle_x * wiggle_x + wiggle_y * wiggle_y) * wiggle_scale;
        b->wiggle_energy += alpha * (wiggle - b->wiggle_energy);
    }

    b->depth_rate += alpha * ((smoothed_depth - b->prev_depth) - b->depth_rate);
    /* 0 (depth steady) .. 1 (changing fast). */
    float depth_activity = fabsf(b->depth_rate) / t->depth_activity_full;
    if (depth_activity > 1.0f) {
        depth_activity = 1.0f;
    }
    b->prev_depth = smoothed_depth;

    if (!b->settled) {
        /* Centred until the settle time (and, COSINE/PATH, until the key is
         * still), then the baseline from the cascade. */
        uint32_t elapsed = now_ms - b->start_ms;
        bool ready = elapsed >= t->settle_ms;
        if (ready && b->mode != TILES_BEND_MODE_LEGACY && elapsed < t->settle_max_ms) {
            float gap_x = b->x1 - b->x2;
            float gap_y = b->y1 - b->y2;
            ready = sqrtf(gap_x * gap_x + gap_y * gap_y) * wiggle_scale < t->settle_still * b->magnitude;
        }
        if (ready) {
            if (b->mode == TILES_BEND_MODE_PATH) {
                anchor_path(b, t);
            }
            b->baseline_x = b->x2;
            b->baseline_y = b->y2;
            b->settled = true;
            b->delta = 0.0f;
        }
        return false;
    }

    float delta_x;
    float delta_y;
    if (b->mode == TILES_BEND_MODE_PATH) {
        /* Depth is predicted by the path, so nothing chases it: only slow
         * creep is pulled out, and only while no bend is held. */
        if (!b->run_active) {
            b->baseline_x += t->recenter_slow * (b->x2 - b->baseline_x);
            b->baseline_y += t->recenter_slow * (b->y2 - b->baseline_y);
        }
        delta_x = b->baseline_x - b->x2;
        delta_y = b->baseline_y - b->y2;
    } else {
        /* Recentre only while no run is confirmed (last pass's state), blending
         * SLOW..FAST by depth activity, cubed: FAST is ~125x SLOW, and a linear
         * blend let sensor noise erase real tilts. */
        if (!b->run_active) {
            float activity_shaped = depth_activity * depth_activity * depth_activity;
            float recenter_alpha = t->recenter_slow + (t->recenter_fast - t->recenter_slow) * activity_shaped;
            b->baseline_x += recenter_alpha * (b->x2 - b->baseline_x);
            b->baseline_y += recenter_alpha * (b->y2 - b->baseline_y);
            b->run_magnitude = b->magnitude;
        }
        if (b->mode == TILES_BEND_MODE_COSINE) {
            delta_x = b->baseline_x - b->x2;
            delta_y = b->baseline_y - b->y2;
        } else {
            /* LEGACY depth compensation: compare against the baseline cosine
             * predicted at the current magnitude. Invariants (each broke
             * things): both terms divide by this pass's same magnitude; both
             * use the fully cascaded x2/y2; during a confirmed run the
             * magnitude is frozen. Sign: predicted - current. */
            delta_x = direction_cosine_from(b->baseline_x, b->run_magnitude) -
                      direction_cosine_from(b->x2, b->run_magnitude);
            delta_y = direction_cosine_from(b->baseline_y, b->run_magnitude) -
                      direction_cosine_from(b->y2, b->run_magnitude);
        }
    }
    float combined = sqrtf(delta_x * delta_x + delta_y * delta_y);
    /* No third smoothing stage: it made bends too slow to start. */
    b->delta = (delta_x >= 0.0f) ? combined : -combined;

    uint16_t bend = apply_vibrato(b, t, bend_14bit_from_cosine_delta(b, cfg, b->delta, now_ms), now_ms);
    if (b->mode != TILES_BEND_MODE_LEGACY && smoothed_depth < t->release_depth &&
        b->depth_rate < BEND_RELEASE_RATE) {
        bend = b->last_bend; /* coming up near rest: hold */
    }
    b->last_bend = bend;
    *out_bend = bend;
    return true;
}

/* ---- Press path learning ---- */

void tiles_press_path_reset(tiles_press_path_t *p) {
    *p = (tiles_press_path_t){0};
}

void tiles_press_path_stroke_begin(tiles_press_path_t *p) {
    p->n = 0u;
}

void tiles_press_path_stroke_sample(tiles_press_path_t *p, int16_t x, int16_t y, int16_t z) {
    float fx = (float)x;
    float fy = (float)y;
    float fz = (float)z;
    if (p->n >= PATH_STROKE_MAX_SAMPLES) {
        p->n = 0u;
    }
    if (p->n == 0u) {
        p->x0 = fx;
        p->y0 = fy;
        p->z0 = fz;
        p->sx = p->sy = p->sz = 0.0f;
        p->sxx = p->syy = p->szz = p->szx = p->szy = 0.0f;
        p->zmin = p->zmax = fz;
    }
    float dx = fx - p->x0;
    float dy = fy - p->y0;
    float dz = fz - p->z0;
    p->sx += dx;
    p->sy += dy;
    p->sz += dz;
    p->sxx += dx * dx;
    p->syy += dy * dy;
    p->szz += dz * dz;
    p->szx += dz * dx;
    p->szy += dz * dy;
    p->zmin = fz < p->zmin ? fz : p->zmin;
    p->zmax = fz > p->zmax ? fz : p->zmax;
    p->n++;
}

/* Least-squares X and Y against Z over the stroke; false if it is too
 * short, too shallow, or not a straight line. */
static bool fit_stroke(const tiles_press_path_t *p, const tiles_bend_tuning_t *t, float *bx, float *by) {
    if (p->n < PATH_STROKE_MIN_SAMPLES || p->zmax - p->zmin < t->path_min_z_range) {
        return false;
    }
    float n = (float)p->n;
    float mx = p->sx / n;
    float my = p->sy / n;
    float mz = p->sz / n;
    float var_z = p->szz / n - mz * mz;
    if (var_z <= 0.0f) {
        return false;
    }
    float cov_zx = p->szx / n - mz * mx;
    float cov_zy = p->szy / n - mz * my;
    /* Variance left once the line is taken out. */
    float rest_x = (p->sxx / n - mx * mx) - cov_zx * cov_zx / var_z;
    float rest_y = (p->syy / n - my * my) - cov_zy * cov_zy / var_z;
    float limit = t->path_max_residual * t->path_max_residual;
    if (rest_x > limit || rest_y > limit) {
        return false;
    }
    *bx = cov_zx / var_z;
    *by = cov_zy / var_z;
    return fabsf(*bx) <= t->path_max_slope && fabsf(*by) <= t->path_max_slope;
}

bool tiles_press_path_stroke_end(tiles_press_path_t *p, const tiles_bend_tuning_t *tuning) {
    const tiles_bend_tuning_t *t = tuning != NULL ? tuning : &TILES_BEND_TUNING_DEFAULT;
    float bx;
    float by;
    bool ok = fit_stroke(p, t, &bx, &by);
    p->n = 0u;
    if (!ok) {
        return false;
    }
    if (p->strokes < UINT16_MAX) {
        p->strokes++;
    }
    uint32_t average = t->path_average_strokes > 0u ? t->path_average_strokes : 1u;
    float weight = 1.0f / (float)(p->strokes < average ? p->strokes : average);
    p->bx += weight * (bx - p->bx);
    p->by += weight * (by - p->by);
    return true;
}
