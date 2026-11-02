#include "light_effects.h"

#include <math.h>
#include <string.h>

/* The approved designs (the TILES Light Effects preview, 2026-10-22), in
 * seconds and pads. First guesses on the board. */
#define FX_ROWS 4
#define FX_COLS 6
#define FX_PADS 24
#define FX_MAX_HITS 16
#define FX_HIT_LIFE_S 2.0f
#define FX_PI 3.14159265f
/* Sequencer versions: the brightness of every effect, and of a playhead
 * hit next to a pad pressed live (first guesses). */
#define FX_SEQ_LEVEL 0.6f
#define FX_PLAYBACK_LEVEL 0.45f

typedef struct {
    uint8_t pad; /* 1-24, 0 = free */
    float v;     /* velocity 0-1 */
    float level; /* 1 pressed live, FX_PLAYBACK_LEVEL from the playhead */
    uint32_t t_ms;
    uint32_t seed;
} fx_hit_t;

static tiles_fx_t s_fx;
static fx_hit_t s_hits[FX_MAX_HITS];
static uint8_t s_next_hit;
static bool s_held[FX_PADS];
static float s_vel[FX_PADS];
static float s_level[FX_PADS]; /* the last hit's level (fx_hit_t) */
static uint32_t s_up_ms[FX_PADS];
static bool s_released[FX_PADS]; /* released at least once (afterglow) */
static float s_heat[FX_PADS];
static float s_ink[FX_PADS][3];
static uint32_t s_step_ms;
static bool s_seq; /* sequencer versions */

static float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static void hsv(float h, float s, float v, float out[3]) {
    h = fmodf(h, 360.0f);
    if (h < 0.0f) {
        h += 360.0f;
    }
    h /= 60.0f;
    float c = v * s, x = c * (1.0f - fabsf(fmodf(h, 2.0f) - 1.0f)), m = v - c;
    float r = 0.0f, g = 0.0f, b = 0.0f;
    switch ((int)h) {
    case 0:
        r = c;
        g = x;
        break;
    case 1:
        r = x;
        g = c;
        break;
    case 2:
        g = c;
        b = x;
        break;
    case 3:
        g = x;
        b = c;
        break;
    case 4:
        r = x;
        b = c;
        break;
    default:
        r = c;
        b = x;
        break;
    }
    out[0] = r + m;
    out[1] = g + m;
    out[2] = b + m;
}

/* out = screen(out, c * k): light over light, never past full. */
static void add_light(float out[3], const float c[3], float k) {
    for (int i = 0; i < 3; i++) {
        out[i] = 1.0f - (1.0f - out[i]) * (1.0f - clampf(c[i] * k, 0.0f, 1.0f));
    }
}

static int row_of(uint8_t pad) {
    return (pad - 1) / FX_COLS;
}

static int col_of(uint8_t pad) {
    return (pad - 1) % FX_COLS;
}

static float dist(uint8_t a, uint8_t b) {
    float dr = (float)(row_of(a) - row_of(b)), dc = (float)(col_of(a) - col_of(b));
    return sqrtf(dr * dr + dc * dc);
}

/* The colour effects' hue, degrees: soft a cool blue, through violet and
 * magenta, hard a hot pink (first guess). */
static float velocity_hue(float v) {
    return 205.0f + 130.0f * v;
}

static float rand01(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return (float)(x & 0xFFFFu) / 65535.0f;
}

/* Touch colour: Sentia blue (soft) through violet to magenta (hard). */
static void velocity_colour(float v, float out[3]) {
    float h = v < 0.5f ? 226.0f + (265.0f - 226.0f) * (v / 0.5f) : 265.0f + (300.0f - 265.0f) * ((v - 0.5f) / 0.5f);
    hsv(h, v < 0.5f ? 0.45f + v : 1.0f, 1.0f, out);
}

/* Heatmap: off, Sentia blue, violet, magenta, pale pink. */
static void heat_colour(float x, float out[3]) {
    static const float STOPS[5][4] = {
        {0.0f, 0.0f, 0.0f, 0.0f},
        {0.25f, 0.58f, 0.67f, 0.98f},
        {0.5f, 0.35f, 0.0f, 1.0f},
        {0.78f, 1.0f, 0.0f, 1.0f},
        {1.0f, 1.0f, 0.78f, 1.0f},
    };
    for (int i = 1; i < 5; i++) {
        if (x <= STOPS[i][0]) {
            float k = (x - STOPS[i - 1][0]) / (STOPS[i][0] - STOPS[i - 1][0]);
            for (int c = 0; c < 3; c++) {
                out[c] = STOPS[i - 1][c + 1] + (STOPS[i][c + 1] - STOPS[i - 1][c + 1]) * k;
            }
            return;
        }
    }
    out[0] = STOPS[4][1];
    out[1] = STOPS[4][2];
    out[2] = STOPS[4][3];
}

static const float WHITE[3] = {1.0f, 1.0f, 1.0f};

void tiles_fx_set(tiles_fx_t fx) {
    if ((unsigned)fx >= (unsigned)TILES_FX_COUNT) {
        fx = TILES_FX_OFF;
    }
    s_fx = fx;
    memset(s_hits, 0, sizeof(s_hits));
    memset(s_heat, 0, sizeof(s_heat));
    memset(s_ink, 0, sizeof(s_ink));
    memset(s_released, 0, sizeof(s_released));
}

tiles_fx_t tiles_fx_get(void) {
    return s_fx;
}

void tiles_fx_set_sequencer(bool on) {
    s_seq = on;
}

/* A hit's travelling light, heat and ink. False if there's no effect. */
static bool add_hit(uint8_t pad, uint8_t velocity, float level, uint32_t now_ms) {
    if (s_fx == TILES_FX_OFF || pad < 1u || pad > FX_PADS) {
        return false;
    }
    uint8_t i = (uint8_t)(pad - 1u);
    float v = clampf((float)velocity / 127.0f, 0.0f, 1.0f);
    fx_hit_t *h = &s_hits[s_next_hit];
    s_next_hit = (uint8_t)((s_next_hit + 1u) % FX_MAX_HITS);
    h->pad = pad;
    h->v = v;
    h->level = level;
    h->t_ms = now_ms;
    h->seed = now_ms * 2654435761u + pad;
    /* Heatmap: one hard hit reaches violet-magenta, a soft one stays blue. */
    float heat = s_fx == TILES_FX_HEATMAP ? 0.1f + 0.45f * v : 0.22f + 0.2f * v;
    s_heat[i] = clampf(s_heat[i] + heat * level, 0.0f, 1.0f);
    float c[3];
    hsv(velocity_hue(v), 1.0f, 1.0f, c);
    for (int k = 0; k < 3; k++) {
        s_ink[i][k] += c[k] * v * level;
    }
    return true;
}

void tiles_fx_hit(uint8_t pad, uint8_t velocity, uint32_t now_ms) {
    if (add_hit(pad, velocity, 1.0f, now_ms)) {
        s_held[pad - 1u] = true;
        s_vel[pad - 1u] = clampf((float)velocity / 127.0f, 0.0f, 1.0f);
        s_level[pad - 1u] = 1.0f;
    }
}

void tiles_fx_playback_hit(uint8_t pad, uint8_t velocity, uint32_t now_ms) {
    if (add_hit(pad, velocity, FX_PLAYBACK_LEVEL, now_ms) && !s_held[pad - 1u]) {
        uint8_t i = (uint8_t)(pad - 1u);
        s_vel[i] = clampf((float)velocity / 127.0f, 0.0f, 1.0f);
        s_level[i] = FX_PLAYBACK_LEVEL;
        s_up_ms[i] = now_ms;
        s_released[i] = true;
    }
}

void tiles_fx_release(uint8_t pad, uint32_t now_ms) {
    if (pad < 1u || pad > FX_PADS) {
        return;
    }
    uint8_t i = (uint8_t)(pad - 1u);
    if (s_held[i]) {
        s_held[i] = false;
        s_up_ms[i] = now_ms;
        s_released[i] = true;
    }
}

void tiles_fx_step(uint32_t now_ms) {
    float dt = (float)(now_ms - s_step_ms) / 1000.0f;
    s_step_ms = now_ms;
    if (dt <= 0.0f) {
        return;
    }
    if (dt > 0.1f) {
        dt = 0.1f;
    }
    float cool = expf(-dt / 2.6f);
    for (int i = 0; i < FX_PADS; i++) {
        s_heat[i] *= cool;
    }
    if (s_fx == TILES_FX_INK) {
        /* Bleeds into the four neighbours and fades. */
        float next[FX_PADS][3];
        float k = clampf(dt * 2.2f, 0.0f, 1.0f), decay = expf(-dt / 2.4f);
        for (int i = 0; i < FX_PADS; i++) {
            int r = i / FX_COLS, c = i % FX_COLS, n = 0;
            float sum[3] = {0.0f, 0.0f, 0.0f};
            static const int D[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
            for (int d = 0; d < 4; d++) {
                int r2 = r + D[d][0], c2 = c + D[d][1];
                if (r2 < 0 || r2 >= FX_ROWS || c2 < 0 || c2 >= FX_COLS) {
                    continue;
                }
                for (int ch = 0; ch < 3; ch++) {
                    sum[ch] += s_ink[r2 * FX_COLS + c2][ch];
                }
                n++;
            }
            for (int ch = 0; ch < 3; ch++) {
                next[i][ch] = (s_ink[i][ch] * (1.0f - k) + sum[ch] / (float)n * k) * decay;
            }
        }
        memcpy(s_ink, next, sizeof(s_ink));
    }
}

/* One hit's light on `pad`, t seconds after it. The sequencer versions
 * keep the travelling ones near the hit (tiles_fx_set_sequencer()). */
static void hit_light(const fx_hit_t *h, uint8_t pad, float t, float out[3]) {
    float d = dist(pad, h->pad);
    int same_row = row_of(pad) == row_of(h->pad), same_col = col_of(pad) == col_of(h->pad);
    float lv = h->level, c[3];
    switch (s_fx) {
    case TILES_FX_RIPPLE: {
        if (t > 1.0f || (s_seq && d > 1.5f)) {
            return;
        }
        float w = expf(-(d - t * 9.0f) * (d - t * 9.0f) / (2.0f * 0.42f * 0.42f));
        add_light(out, WHITE, lv * h->v * w * (1.0f - t));
        return;
    }
    case TILES_FX_CROSS: {
        if (!(same_row || same_col) || d == 0.0f) {
            return;
        }
        float k = fabsf((float)(row_of(pad) - row_of(h->pad))) + fabsf((float)(col_of(pad) - col_of(h->pad)));
        float ta = k / 16.0f;
        if (t < ta || (s_seq && k > 2.0f)) {
            return;
        }
        add_light(out, WHITE, lv * h->v * expf(-(t - ta) / 0.2f) * (1.0f - k * 0.09f));
        return;
    }
    case TILES_FX_HALO:
        if (d == 0.0f || d > 1.9f) {
            return;
        }
        add_light(out, WHITE, lv * 0.8f * h->v * (1.0f - d / 2.0f) * expf(-t / 0.3f));
        return;
    case TILES_FX_PRISM_RIPPLE: {
        if (t > 1.1f || (s_seq && d > 1.5f)) {
            return;
        }
        float w = expf(-(d - t * 8.0f) * (d - t * 8.0f) / (2.0f * 0.48f * 0.48f));
        hsv(velocity_hue(h->v) + d * 30.0f, 1.0f, 1.0f, c);
        add_light(out, c, lv * h->v * w * (1.0f - t / 1.1f));
        return;
    }
    case TILES_FX_FIREWORKS: {
        /* Four sparks up to two pads away; two next to it in a sequencer. */
        uint32_t sparks = s_seq ? 2u : 4u;
        float spread = s_seq ? 2.0f : 4.0f;
        for (uint32_t s = 0; s < sparks; s++) {
            int sr = row_of(h->pad) + (int)lroundf((rand01(h->seed + s * 7u) - 0.5f) * spread);
            int sc = col_of(h->pad) + (int)lroundf((rand01(h->seed + s * 13u) - 0.5f) * (spread + 1.0f));
            if (sr != row_of(pad) || sc != col_of(pad)) {
                continue;
            }
            float ta = d / 12.0f;
            if (t < ta) {
                continue;
            }
            hsv(velocity_hue(h->v) + (rand01(h->seed + s) - 0.5f) * 90.0f, 1.0f, 1.0f, c);
            add_light(out, c, lv * h->v * expf(-(t - ta) / 0.28f));
        }
        return;
    }
    case TILES_FX_RISE: {
        if (!same_col) {
            return;
        }
        float top = 1.0f + h->v * (s_seq ? 1.0f : 3.0f);
        float env = t < 0.06f ? t / 0.06f : expf(-(t - 0.06f) / 0.45f);
        float height = (float)(FX_ROWS - row_of(pad)); /* 1 = bottom row */
        float l = clampf(top * env - (height - 1.0f), 0.0f, 1.0f);
        hsv(velocity_hue(h->v), 1.0f, 1.0f, c);
        add_light(out, c, lv * l * (0.55f + 0.45f * h->v));
        return;
    }
    default:
        return;
    }
}

bool tiles_fx_pad_light(uint8_t pad, uint32_t now_ms, float rgb[3]) {
    rgb[0] = rgb[1] = rgb[2] = 0.0f;
    if (s_fx == TILES_FX_OFF || pad < 1u || pad > FX_PADS) {
        return false;
    }
    uint8_t i = (uint8_t)(pad - 1u);
    for (int k = 0; k < FX_MAX_HITS; k++) {
        const fx_hit_t *h = &s_hits[k];
        if (h->pad == 0u) {
            continue;
        }
        float t = (float)(now_ms - h->t_ms) / 1000.0f;
        if (t < FX_HIT_LIFE_S) {
            hit_light(h, pad, t, rgb);
        }
    }
    float c[3];
    switch (s_fx) {
    case TILES_FX_AFTERGLOW:
        if (!s_held[i] && s_released[i]) {
            float k = (float)(now_ms - s_up_ms[i]) / 1000.0f;
            if (k < 2.0f) {
                add_light(rgb, WHITE, s_level[i] * s_vel[i] * expf(-k / 0.32f));
            }
        }
        break;
    case TILES_FX_TOUCH_COLOUR:
        if (!s_held[i] && s_released[i]) {
            float k = (float)(now_ms - s_up_ms[i]) / 1000.0f;
            if (k < 2.0f) {
                velocity_colour(s_vel[i], c);
                add_light(rgb, c, s_level[i] * expf(-k / 0.4f));
            }
        }
        break;
    case TILES_FX_WARMTH:
        if (s_heat[i] > 0.01f) {
            add_light(rgb, WHITE, 0.85f * powf(s_heat[i], 1.3f));
        }
        break;
    case TILES_FX_HEATMAP:
        if (s_heat[i] > 0.01f) {
            heat_colour(s_heat[i], c);
            add_light(rgb, c, 1.0f);
        }
        break;
    case TILES_FX_INK:
        for (int ch = 0; ch < 3; ch++) {
            rgb[ch] = 1.0f - (1.0f - rgb[ch]) * (1.0f - clampf(s_ink[i][ch], 0.0f, 1.0f));
        }
        break;
    default:
        break;
    }
    if (s_seq) {
        for (int ch = 0; ch < 3; ch++) {
            rgb[ch] *= FX_SEQ_LEVEL;
        }
    }
    return rgb[0] + rgb[1] + rgb[2] > 0.004f;
}

bool tiles_fx_pressed_colour(uint8_t pad, float rgb[3]) {
    if (s_fx != TILES_FX_TOUCH_COLOUR || pad < 1u || pad > FX_PADS || !s_held[pad - 1u]) {
        return false;
    }
    velocity_colour(s_vel[pad - 1u], rgb);
    return true;
}

bool tiles_fx_busy(uint32_t now_ms) {
    if (s_fx == TILES_FX_OFF) {
        return false;
    }
    for (int k = 0; k < FX_MAX_HITS; k++) {
        if (s_hits[k].pad != 0u && (float)(now_ms - s_hits[k].t_ms) / 1000.0f < FX_HIT_LIFE_S) {
            return true;
        }
    }
    bool heat = s_fx == TILES_FX_WARMTH || s_fx == TILES_FX_HEATMAP;
    bool fade = s_fx == TILES_FX_AFTERGLOW || s_fx == TILES_FX_TOUCH_COLOUR;
    for (int i = 0; i < FX_PADS; i++) {
        if ((heat && s_heat[i] > 0.01f) || (s_fx == TILES_FX_INK && s_ink[i][0] + s_ink[i][1] + s_ink[i][2] > 0.02f) ||
            (fade && s_released[i] && !s_held[i] && now_ms - s_up_ms[i] < 2000u)) {
            return true;
        }
    }
    return false;
}
