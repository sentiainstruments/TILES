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
#define SHOW_MSG_LOOK 0x03u
#define SHOW_MODE_LEN 11u  /* 7D 02 01 on anim a.rgb b.rgb */
#define SHOW_FRAME_LEN 12u /* 7D 02 02 kick accent level band1-6 */
#define SHOW_LOOK_LEN 14u  /* 7D 02 03 glow len bright tail bed gain speed
                              meter c.rgb */

/* Without messages for this long the LEDs go back to TILES (app quit,
 * cable out, music stopped and the sender went quiet). */
#define SHOW_TIMEOUT_MS 2000u
/* Render step; the pads are paced further by the lighting service. */
#define SHOW_STEP_MS 20u

/* Base decays at "tail" 1 (first guesses, to judge on the board): the
 * pads' kick, accent, the meter's fall, a spark. */
#define SHOW_KICK_DECAY_MS 70.0f
#define SHOW_ACCENT_DECAY_MS 50.0f
#define SHOW_BAND_FALL_MS 140.0f
#define SHOW_SPARK_DECAY_MS 260.0f
#define SHOW_RIPPLE_SPEED 14.0f /* grid cells a second at speed 1 */
#define SHOW_RAIN_STEP_MS 90.0f /* a drop falls a row in this, speed 1 */
#define SHOW_LIT_LEVEL 0.85f    /* the standby equalizer's */
#define SHOW_MAX_DROPS 12u

typedef enum {
    SHOW_ANIM_METER = 0, /* the standby equalizer, from the music's bands */
    SHOW_ANIM_PULSE,     /* the whole grid on each kick */
    SHOW_ANIM_RIPPLE,    /* a ring from the centre on each kick */
    SHOW_ANIM_SPARKLE,   /* pads spark on snares and hats, a wash on kicks */
    SHOW_ANIM_RAIN,      /* drops fall from the top on kicks and accents */
    SHOW_ANIM_CHASE,     /* a lit column steps across on each kick */
    SHOW_ANIM_SPECTRUM,  /* the meter in the colours, main to second */
    SHOW_ANIM_COUNT,
} show_anim_t;

typedef enum {
    GLOW_WHITE = 0,
    GLOW_MAIN,
    GLOW_SECOND,
    GLOW_OFF,
} show_glow_t;

/* What the sender chose (the look message), 7-bit values mapped here. */
typedef struct {
    uint8_t glow;       /* show_glow_t */
    float strobe_ms;    /* the underglow's fall */
    float brightness;   /* the pads, 0-1 */
    float tail;         /* the pads' decays, x */
    float bed;          /* the dim bed under the hits, x */
    float gain;         /* the meter's reach, x */
    float speed;        /* ripple, rain and chase, x */
    bool meter_colours; /* the meter in the colours, not white/yellow/red */
    float c[3];         /* the accent colour */
} show_look_t;

static show_look_t s_look = {GLOW_WHITE, 70.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                             1.0f, false, {1.0f, 1.0f, 1.0f}};

static bool s_on;
static bool s_active;
static uint8_t s_anim;
static float s_a[3] = {1.0f, 0.0f, 1.0f};    /* main colour */
static float s_b[3] = {0.58f, 0.67f, 0.98f}; /* second colour */
static uint32_t s_last_frame_ms;
static uint32_t s_last_step_ms;

static float s_glow;   /* the underglow's strobe 0-1 */
static float s_kick;   /* the pads' kick envelope 0-1 */
static float s_accent; /* the same for snares and hats */
static float s_level;  /* overall loudness 0-1 */
static float s_band[6];
static float s_band_shown[6];
static uint32_t s_ripple_ms;
static float s_ripple_strength;
static float s_spark[TILES_NUM_PADS];
static uint8_t s_chase_col = TILES_GRID_MIN_COL;
static float s_chase[6]; /* each column's light, the newest at 1 */

typedef struct {
    uint8_t col;
    float row; /* 1 top .. past 4 gone */
    float strength;
    bool accent;
} show_drop_t;
static show_drop_t s_drops[SHOW_MAX_DROPS];
static uint8_t s_drop_count;

static float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static float from7(uint8_t v) {
    return (float)(v & 0x7Fu) / 127.0f;
}

/* 0-127 to lo..hi, 64 = mid on a log scale (a ratio setting). */
static float ratio7(uint8_t v, float lo, float hi) {
    return lo * powf(hi / lo, from7(v));
}

static void push_underglow(void) {
    float v = fmaxf(s_glow, s_accent * 0.35f);
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (s_look.glow == GLOW_WHITE) {
        /* White on the hit, falling toward the main colour. */
        float k = s_glow;
        r = v * (k + (1.0f - k) * s_a[0]);
        g = v * (k + (1.0f - k) * s_a[1]);
        b = v * (k + (1.0f - k) * s_a[2]);
    } else if (s_look.glow != GLOW_OFF) {
        const float *c = s_look.glow == GLOW_MAIN ? s_a : s_b;
        r = v * c[0], g = v * c[1], b = v * c[2];
    }
    tiles_lighting_set_show_underglow_rgb(r, g, b);
}

static void drop(bool accent, float strength) {
    if (s_drop_count >= SHOW_MAX_DROPS) {
        return;
    }
    show_drop_t *d = &s_drops[s_drop_count++];
    d->col = (uint8_t)(TILES_GRID_MIN_COL + get_rand_32() % 6u);
    d->row = 1.0f;
    d->strength = strength;
    d->accent = accent;
}

static void on_kick(float kick, uint32_t now_ms) {
    s_glow = fmaxf(s_glow, kick);
    s_kick = fmaxf(s_kick, kick);
    s_ripple_ms = now_ms;
    s_ripple_strength = kick;
    s_chase_col = (uint8_t)(s_chase_col >= TILES_GRID_MAX_COL
                                ? TILES_GRID_MIN_COL
                                : s_chase_col + 1u);
    s_chase[s_chase_col - TILES_GRID_MIN_COL] = kick;
    if (s_anim == SHOW_ANIM_RAIN) {
        drop(false, kick);
        if (kick > 0.6f) {
            drop(false, kick);
        }
    }
}

static void on_accent(float accent) {
    s_accent = fmaxf(s_accent, accent);
    if (s_anim == SHOW_ANIM_SPARKLE) {
        s_spark[get_rand_32() % TILES_NUM_PADS] = accent;
    } else if (s_anim == SHOW_ANIM_RAIN) {
        drop(true, accent);
    }
}

static void read_look(const uint8_t *d) {
    s_look.glow = d[0] <= GLOW_OFF ? d[0] : GLOW_WHITE;
    s_look.strobe_ms = 20.0f + 380.0f * from7(d[1]);
    s_look.brightness = 0.15f + 0.85f * from7(d[2]);
    s_look.tail = ratio7(d[3], 0.25f, 4.0f);
    s_look.bed = 3.0f * from7(d[4]);
    s_look.gain = ratio7(d[5], 0.5f, 2.0f);
    s_look.speed = ratio7(d[6], 0.4f, 2.5f);
    s_look.meter_colours = d[7] != 0u;
    for (uint8_t ch = 0; ch < 3u; ch++) {
        s_look.c[ch] = from7(d[8u + ch]);
    }
}

static void on_sysex(tiles_midi_port_t port, const uint8_t *data,
                     size_t len) {
    (void)port; /* from the main or the DAW port */
    if (len < 3u || data[0] != TILES_SYSEX_MANUFACTURER_ID ||
        data[1] != SHOW_SYSEX_SUB_ID) {
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
    } else if (data[2] == SHOW_MSG_LOOK && len == SHOW_LOOK_LEN) {
        read_look(&data[3]);
        s_last_frame_ms = now_ms;
    } else if (data[2] == SHOW_MSG_FRAME && len == SHOW_FRAME_LEN) {
        s_last_frame_ms = now_ms;
        float kick = from7(data[3]), accent = from7(data[4]);
        s_level = from7(data[5]);
        for (uint8_t i = 0; i < 6u; i++) {
            s_band[i] = from7(data[6u + i]);
        }
        if (kick > 0.0f) {
            on_kick(kick, now_ms);
        }
        if (accent > 0.0f) {
            on_accent(accent);
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

static void mix(const float *x, const float *y, float t, float out[3]) {
    for (uint8_t ch = 0; ch < 3u; ch++) {
        out[ch] = x[ch] + (y[ch] - x[ch]) * t;
    }
}

/* A meter cell: lit when the column's level reaches the row (row 4 at
 * 0.25 .. row 1 at 1.0), a soft edge just below. Classic colours: rows
 * 3-4 white, 2 yellow, 1 red (the standby equalizer); else the bottom
 * the main colour toward the second, the top row the accent colour.
 * Spectrum: the columns main to second, the top row the accent. */
static void meter_pad(uint8_t row, uint8_t col, bool spectrum,
                      float rgb[3]) {
    float level = clamp01(s_band_shown[col - TILES_GRID_MIN_COL] *
                          s_look.gain);
    float threshold = (float)(5u - row) / 4.0f;
    rgb[0] = rgb[1] = rgb[2] = 0.0f;
    if (level < threshold - 0.05f) {
        return;
    }
    float on = level >= threshold ? 1.0f
                                  : (level - (threshold - 0.05f)) / 0.05f;
    float c[3] = {1.0f, 1.0f, 1.0f};
    if (spectrum) {
        float t = (float)(col - TILES_GRID_MIN_COL) / 5.0f;
        mix(s_a, s_b, t, c);
        if (row == 1u) {
            c[0] = s_look.c[0], c[1] = s_look.c[1], c[2] = s_look.c[2];
        }
    } else if (s_look.meter_colours) {
        mix(s_a, s_b, (4.0f - (float)row) / 2.0f, c);
        if (row == 1u) {
            c[0] = s_look.c[0], c[1] = s_look.c[1], c[2] = s_look.c[2];
        }
    } else if (row == 1u) {
        c[1] = 0.0f, c[2] = 0.0f;
    } else if (row == 2u) {
        c[2] = 0.0f;
    }
    for (uint8_t ch = 0; ch < 3u; ch++) {
        rgb[ch] = clamp01(c[ch]) * SHOW_LIT_LEVEL * on;
    }
}

static void step_state(uint32_t now_ms, float dt_ms) {
    float tail = s_look.tail;
    s_glow *= expf(-dt_ms / s_look.strobe_ms);
    s_kick *= expf(-dt_ms / (SHOW_KICK_DECAY_MS * tail));
    s_accent *= expf(-dt_ms / (SHOW_ACCENT_DECAY_MS * tail));
    float fall = expf(-dt_ms / (SHOW_BAND_FALL_MS * tail));
    for (uint8_t i = 0; i < 6u; i++) {
        /* A meter: up at once, down smoothly. */
        float want = s_band[i], *shown = &s_band_shown[i];
        *shown = want > *shown ? want : want + (*shown - want) * fall;
        s_chase[i] *= expf(-dt_ms / (180.0f * tail));
    }
    float spark_fall = expf(-dt_ms / (SHOW_SPARK_DECAY_MS * tail));
    for (uint8_t p = 0; p < TILES_NUM_PADS; p++) {
        s_spark[p] *= spark_fall;
    }
    float rows = dt_ms / SHOW_RAIN_STEP_MS * s_look.speed;
    uint8_t kept = 0;
    for (uint8_t i = 0; i < s_drop_count; i++) {
        s_drops[i].row += rows;
        if (s_drops[i].row < 5.5f) {
            s_drops[kept++] = s_drops[i];
        }
    }
    s_drop_count = kept;
    (void)now_ms;
}

/* The light a drop leaves at a cell: its head and a fading trail. */
static float rain_at(uint8_t row, uint8_t col, float rgb[3]) {
    float best = 0.0f;
    for (uint8_t i = 0; i < s_drop_count; i++) {
        const show_drop_t *d = &s_drops[i];
        if (d->col != col) {
            continue;
        }
        float behind = d->row - (float)row; /* 0 the head, more the trail */
        if (behind < -0.5f || behind > 2.5f * s_look.tail) {
            continue;
        }
        float lit = d->strength * (behind < 0.5f
                                       ? 1.0f
                                       : 1.0f - behind / (2.5f * s_look.tail));
        if (lit > best) {
            best = lit;
            const float *c = d->accent ? s_look.c : s_a;
            rgb[0] = c[0] * lit, rgb[1] = c[1] * lit, rgb[2] = c[2] * lit;
        }
    }
    return best;
}

static void render(uint32_t now_ms, float dt_ms) {
    step_state(now_ms, dt_ms);
    float radius = (float)(now_ms - s_ripple_ms) / 1000.0f *
                   SHOW_RIPPLE_SPEED * s_look.speed;
    float bed = (0.04f + 0.10f * s_level) * s_look.bed;

    for (uint8_t row = 1u; row <= TILES_GRID_MAX_ROW; row++) {
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL;
             col++) {
            uint8_t pad = board_pad_for_row_col(row, col);
            float rgb[3] = {0.0f, 0.0f, 0.0f};
            switch (s_anim) {
            case SHOW_ANIM_PULSE:
                for (uint8_t ch = 0; ch < 3u; ch++) {
                    rgb[ch] = s_b[ch] * bed + s_a[ch] * s_kick;
                }
                break;
            case SHOW_ANIM_RIPPLE: {
                float dr = (float)row - 2.5f, dc = (float)col - 3.5f;
                float ring = 1.0f - fabsf(sqrtf(dr * dr + dc * dc) - radius) /
                                        (0.9f * s_look.tail);
                float lit = ring > 0.0f ? ring * s_ripple_strength *
                                              clamp01(1.0f - radius / 6.0f)
                                        : 0.0f;
                for (uint8_t ch = 0; ch < 3u; ch++) {
                    rgb[ch] = s_b[ch] * bed + s_a[ch] * lit;
                }
                break;
            }
            case SHOW_ANIM_SPARKLE:
                for (uint8_t ch = 0; ch < 3u; ch++) {
                    rgb[ch] = s_b[ch] * bed + s_a[ch] * s_kick * 0.4f +
                              s_look.c[ch] * s_spark[pad - 1u];
                }
                break;
            case SHOW_ANIM_RAIN:
                if (rain_at(row, col, rgb) <= 0.0f) {
                    for (uint8_t ch = 0; ch < 3u; ch++) {
                        rgb[ch] = s_b[ch] * bed;
                    }
                }
                break;
            case SHOW_ANIM_CHASE: {
                float lit = s_chase[col - TILES_GRID_MIN_COL];
                for (uint8_t ch = 0; ch < 3u; ch++) {
                    rgb[ch] = s_b[ch] * bed + s_a[ch] * lit;
                }
                break;
            }
            case SHOW_ANIM_SPECTRUM:
                meter_pad(row, col, true, rgb);
                break;
            default:
                meter_pad(row, col, false, rgb);
                break;
            }
            float k = s_look.brightness;
            tiles_lighting_set_show_pad_rgb(pad, clamp01(rgb[0]) * k,
                                            clamp01(rgb[1]) * k,
                                            clamp01(rgb[2]) * k);
        }
    }
    push_underglow();
}

void tiles_show_link_service(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    bool active = s_on && now_ms - s_last_frame_ms < SHOW_TIMEOUT_MS;
    if (active != s_active) {
        s_active = active;
        s_glow = s_kick = s_accent = 0.0f;
        s_drop_count = 0u;
        tiles_lighting_set_show_active(active);
        s_last_step_ms = now_ms;
    }
    if (!active || now_ms - s_last_step_ms < SHOW_STEP_MS) {
        return;
    }
    render(now_ms, (float)(now_ms - s_last_step_ms));
    s_last_step_ms = now_ms;
}
