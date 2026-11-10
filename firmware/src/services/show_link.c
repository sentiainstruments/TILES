#include "show_link.h"

#include "board_layout.h"
#include "board_pins.h"
#include "lighting.h"
#include "midi_in.h"
#include "product_identity.h"

#include "pico/rand.h"
#include "pico/time.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#define SHOW_SYSEX_SUB_ID 0x02u
#define SHOW_MSG_MODE 0x01u
#define SHOW_MSG_FRAME 0x02u
#define SHOW_MODE_LEN 11u  /* 7D 02 01 on anim a.r a.g a.b b.r b.g b.b */
#define SHOW_FRAME_LEN 12u /* 7D 02 02 kick accent level band1-6 */

/* Without frames for this long the LEDs go back to TILES (app quit, cable
 * out, music stopped and the sender went quiet). */
#define SHOW_TIMEOUT_MS 2000u
/* Render step; the pads are paced further by the lighting service. */
#define SHOW_STEP_MS 20u

/* Decays (first guesses, to judge on the board): the strobe has to read as
 * a hit, the meter as a meter. */
#define SHOW_KICK_DECAY_MS 70.0f
#define SHOW_ACCENT_DECAY_MS 50.0f
#define SHOW_BAND_FALL_MS 140.0f
#define SHOW_SPARK_DECAY_MS 260.0f
#define SHOW_RIPPLE_SPEED 14.0f /* grid cells a second */
#define SHOW_LIT_LEVEL 0.85f    /* the standby equalizer's */

typedef enum {
    SHOW_ANIM_METER = 0, /* the standby equalizer, from the music's bands */
    SHOW_ANIM_PULSE,     /* the whole grid on each kick */
    SHOW_ANIM_RIPPLE,    /* a ring from the centre on each kick */
    SHOW_ANIM_SPARKLE,   /* pads spark on snares and hats, a wash on kicks */
    SHOW_ANIM_COUNT,
} show_anim_t;

static bool s_on;
static bool s_active;
static uint8_t s_anim;
static float s_a[3] = {1.0f, 0.0f, 1.0f}; /* main colour */
static float s_b[3] = {0.58f, 0.67f, 0.98f}; /* second colour */
static uint32_t s_last_frame_ms;
static uint32_t s_last_step_ms;

static float s_kick;   /* envelope 0-1, up at a kick, falling */
static float s_accent; /* the same for snares and hats */
static float s_level;  /* overall loudness 0-1 */
static float s_band[6];
static float s_band_shown[6];
static uint32_t s_ripple_ms;
static float s_ripple_strength;
static float s_spark[TILES_NUM_PADS];

static float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static float from7(uint8_t v) {
    return (float)(v & 0x7Fu) / 127.0f;
}

static void push_underglow(void) {
    float k = s_kick, a = s_accent * 0.35f;
    float v = k > a ? k : a;
    /* A white strobe, tinted toward the main colour as it falls. */
    float r = v * (k * 1.0f + (1.0f - k) * s_a[0]);
    float g = v * (k * 1.0f + (1.0f - k) * s_a[1]);
    float b = v * (k * 1.0f + (1.0f - k) * s_a[2]);
    tiles_lighting_set_show_underglow_rgb(r, g, b);
}

static void on_sysex(tiles_midi_port_t port, const uint8_t *data, size_t len) {
    (void)port; /* from the main or the DAW port */
    if (len < 3u || data[0] != TILES_SYSEX_MANUFACTURER_ID || data[1] != SHOW_SYSEX_SUB_ID) {
        return;
    }
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (data[2] == SHOW_MSG_MODE && len == SHOW_MODE_LEN) {
        bool on = data[3] != 0u;
        if (on != s_on) {
            printf("[show] %s\n", on ? "on" : "off");
        }
        s_on = on;
        s_anim = data[4] < SHOW_ANIM_COUNT ? data[4] : SHOW_ANIM_METER;
        for (uint8_t ch = 0; ch < 3u; ch++) {
            s_a[ch] = from7(data[5u + ch]);
            s_b[ch] = from7(data[8u + ch]);
        }
        s_last_frame_ms = now_ms;
    } else if (data[2] == SHOW_MSG_FRAME && len == SHOW_FRAME_LEN) {
        s_last_frame_ms = now_ms;
        float kick = from7(data[3]), accent = from7(data[4]);
        s_level = from7(data[5]);
        for (uint8_t i = 0; i < 6u; i++) {
            s_band[i] = from7(data[6u + i]);
        }
        if (kick > 0.0f) {
            s_kick = fmaxf(s_kick, kick);
            s_ripple_ms = now_ms;
            s_ripple_strength = kick;
        }
        if (accent > 0.0f) {
            s_accent = fmaxf(s_accent, accent);
            if (s_anim == SHOW_ANIM_SPARKLE) {
                s_spark[get_rand_32() % TILES_NUM_PADS] = accent;
            }
        }
        /* The strobe goes out now, not at the next step: it's the beat. */
        if (s_active && kick > 0.0f) {
            push_underglow();
        }
    }
}

void tiles_show_link_init(void) {
    (void)tiles_midi_in_register_sysex_callback(on_sysex);
}

bool tiles_show_link_active(void) {
    return s_active;
}

/* The meter: each column a bar from its band, rows 3-4 white, 2 yellow,
 * 1 red, like the standby equalizer. */
static void meter_pad(uint8_t row, uint8_t col, float rgb[3]) {
    float level = s_band_shown[col - TILES_GRID_MIN_COL];
    float threshold = (float)(5u - row) / 4.0f; /* row 4 = 0.25 ... row 1 = 1.0 */
    rgb[0] = rgb[1] = rgb[2] = 0.0f;
    if (level < threshold - 0.05f) {
        return;
    }
    float on = level >= threshold ? 1.0f : (level - (threshold - 0.05f)) / 0.05f;
    float c[3] = {1.0f, 1.0f, 1.0f};
    if (row == 1u) {
        c[1] = 0.0f, c[2] = 0.0f;
    } else if (row == 2u) {
        c[2] = 0.0f;
    }
    for (uint8_t ch = 0; ch < 3u; ch++) {
        rgb[ch] = c[ch] * SHOW_LIT_LEVEL * on;
    }
}

static void render(uint32_t now_ms, float dt_ms) {
    s_kick *= expf(-dt_ms / SHOW_KICK_DECAY_MS);
    s_accent *= expf(-dt_ms / SHOW_ACCENT_DECAY_MS);
    float fall = expf(-dt_ms / SHOW_BAND_FALL_MS);
    for (uint8_t i = 0; i < 6u; i++) {
        /* A meter: up at once, down smoothly. */
        s_band_shown[i] = s_band[i] > s_band_shown[i] ? s_band[i] : s_band[i] + (s_band_shown[i] - s_band[i]) * fall;
    }
    float spark_fall = expf(-dt_ms / SHOW_SPARK_DECAY_MS);
    float radius = (float)(now_ms - s_ripple_ms) / 1000.0f * SHOW_RIPPLE_SPEED;

    for (uint8_t row = 1u; row <= TILES_GRID_MAX_ROW; row++) {
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            uint8_t pad = board_pad_for_row_col(row, col);
            float rgb[3] = {0.0f, 0.0f, 0.0f};
            float floor = 0.04f + 0.10f * s_level; /* a dim bed in the second colour */
            switch (s_anim) {
            case SHOW_ANIM_PULSE:
                for (uint8_t ch = 0; ch < 3u; ch++) {
                    rgb[ch] = s_b[ch] * floor + s_a[ch] * s_kick;
                }
                break;
            case SHOW_ANIM_RIPPLE: {
                float dr = (float)row - 2.5f, dc = (float)col - 3.5f;
                float ring = 1.0f - fabsf(sqrtf(dr * dr + dc * dc) - radius) / 0.9f;
                float lit = ring > 0.0f ? ring * s_ripple_strength * clamp01(1.0f - radius / 6.0f) : 0.0f;
                for (uint8_t ch = 0; ch < 3u; ch++) {
                    rgb[ch] = s_b[ch] * floor + s_a[ch] * lit;
                }
                break;
            }
            case SHOW_ANIM_SPARKLE:
                s_spark[pad - 1u] *= spark_fall;
                for (uint8_t ch = 0; ch < 3u; ch++) {
                    rgb[ch] = s_a[ch] * s_kick * 0.4f + s_b[ch] * s_spark[pad - 1u];
                }
                break;
            default:
                meter_pad(row, col, rgb);
                break;
            }
            tiles_lighting_set_show_pad_rgb(pad, clamp01(rgb[0]), clamp01(rgb[1]), clamp01(rgb[2]));
        }
    }
    push_underglow();
}

void tiles_show_link_service(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    bool active = s_on && now_ms - s_last_frame_ms < SHOW_TIMEOUT_MS;
    if (active != s_active) {
        s_active = active;
        s_kick = s_accent = 0.0f;
        tiles_lighting_set_show_active(active);
        s_last_step_ms = now_ms;
    }
    if (!active || now_ms - s_last_step_ms < SHOW_STEP_MS) {
        return;
    }
    render(now_ms, (float)(now_ms - s_last_step_ms));
    s_last_step_ms = now_ms;
}
