#include "grain_voice.h"

/* The granular factory sounds' grain settings; their patches (place,
 * filter, envelopes, levels) are in factory_presets.c. In flash: a track
 * plays a RAM copy (presets.h). Tuned on rendered demos (2026-11-23): clouds of
 * 5-6 grains rather than a full 8, smoother and cheaper. */
const tiles_grain_params_t g_grain_factory[TILES_GRAIN_FACTORY_COUNT] = {
    /* A wide pad of saw and square grains, an octave and a fifth above
     * now and then; pressure makes them shorter and denser, slide
     * scatters them. */
    [TILES_GRAIN_CLOUD] = {
        .source = TILES_GRAIN_SRC_WAVE,
        .size_ms = 240.0f, .density = 24.0f, .jitter = 0.6f, .spray = 0.12f,
        .scatter_cents = 8.0f, .octave_up = 0.15f, .fifth = 0.05f,
        .spread = 0.9f,
        .size_pressure = -0.6f, .density_pressure = 1.0f, .spray_pressure = 0.2f, .spray_slide = 0.3f,
        .scatter_slide = 30.0f,
    },
    /* Short vowel grains at random, drifting; pressure fills them in. */
    [TILES_GRAIN_DUST] = {
        .source = TILES_GRAIN_SRC_WAVE,
        .size_ms = 35.0f, .density = 26.0f, .jitter = 1.0f, .spray = 0.35f, .scan = 0.08f,
        .scatter_cents = 25.0f, .octave_up = 0.2f, .fifth = 0.1f,
        .spread = 1.0f, .skew = 0.6f,
        .size_pressure = 0.6f, .density_pressure = 1.6f, .spray_slide = 0.4f,
    },
    /* Glass grains with octaves and fifths above. */
    [TILES_GRAIN_SHIMMER] = {
        .source = TILES_GRAIN_SRC_WAVE,
        .size_ms = 260.0f, .density = 20.0f, .jitter = 0.4f, .spray = 0.1f,
        .scatter_cents = 6.0f, .octave_up = 0.35f, .fifth = 0.12f,
        .spread = 1.0f, .skew = 0.2f,
        .size_pressure = -0.5f, .density_pressure = 0.8f,
    },
    /* Drum slot 8 (or the last one loaded) stretched to a quarter of its
     * speed; slide scrubs through it, pressure lengthens the grains. */
    [TILES_GRAIN_SCATTER] = {
        .source = TILES_GRAIN_SRC_TAPE, .slot = 7u, .root = 60u,
        .size_ms = 120.0f, .density = 28.0f, .jitter = 0.7f, .spray = 0.15f, .scan = 0.25f,
        .scatter_cents = 10.0f, .reverse = 0.25f,
        .spread = 0.8f, .skew = 0.15f,
        .size_pressure = 0.7f, .density_pressure = 1.0f, .spray_pressure = 0.3f,
    },
};
