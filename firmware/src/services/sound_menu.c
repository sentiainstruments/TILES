#include "sound_menu.h"

#include "audio_engine.h"
#include "board_layout.h"
#include "buttons.h"
#include "hall.h"
#include "haptics.h"
#include "lighting.h"
#include "midi_channels.h"
#include "touch.h"
#include "usb_audio.h"

#include <string.h>

/* A press past half travel picks, as in the mode menu (op_mode.c); deeper
 * fine-tunes an effect up to the next level at full travel (~900). */
#define PICK_DEPTH 450.0f
#define FULL_DEPTH 900.0f
#define PREVIEW_MS 600u
#define FX_SEND_MS 20u
/* A sound that isn't the track's, a level an effect hasn't reached: first
 * guesses. */
#define DIM 0.18f
#define KEY_DIM 0.4f
#define ROWS 4u
#define COLS 6u

/* A family a row, top to bottom, by name (a reordered list keeps its
 * place); the preview note suits the family. */
static const char *const k_rows[ROWS][COLS] = {
    {"acid", "reese", "wobble", "sub", "solid"},
    {"pluck", "lead", "stab", "hoover", "tine"},
    {"haze", "air", "choir", "cloud", "dust", "scatter"},
    {"glass", "pure", "chime", "shimmer"},
};
static const float k_row_rgb[ROWS][3] = {
    {1.0f, 0.0f, 0.3f},   /* basses */
    {0.0f, 0.7f, 1.0f},   /* plucks and leads */
    {0.55f, 0.3f, 1.0f},  /* pads */
    {0.75f, 0.85f, 1.0f}, /* bells */
};
static const uint8_t k_row_preview[ROWS] = {36u, 60u, 57u, 72u};

/* An effect a column; its amount as a meter from the bottom row (off) up. */
static const uint8_t k_fx_param[COLS] = {TILES_FX_TRACK_REVERB, TILES_FX_TRACK_DELAY, TILES_FX_TRACK_CHORUS,
                                         TILES_FX_TRACK_DRIVE,  TILES_FX_TRACK_CRUSH, TILES_FX_TRACK_PUMP};
static const float k_fx_rgb[COLS][3] = {
    {0.2f, 0.4f, 1.0f}, /* reverb */
    {0.0f, 0.8f, 0.8f}, /* delay */
    {0.6f, 0.3f, 1.0f}, /* chorus */
    {1.0f, 0.35f, 0.0f}, /* drive */
    {0.7f, 1.0f, 0.0f}, /* crush */
    {1.0f, 0.0f, 0.6f}, /* pump */
};

/* Each track on the underglow in its sequencer lane's colour (op_mode.c);
 * a lane's channel plays its track (audio_engine.h). */
static const float k_track_rgb[4][3] = {
    {1.0f, 0.12f, 0.0f},
    {1.0f, 0.35f, 0.0f},
    {1.0f, 0.62f, 0.0f},
    {1.0f, 0.0f, 0.3f},
};
static const uint8_t k_track_channel[4] = {TILES_MIDI_CH_SEQ_LANE_0, TILES_MIDI_CH_SEQ_LANE_1,
                                           TILES_MIDI_CH_SEQ_LANE_2, TILES_MIDI_CH_SEQ_LANE_3};

static bool s_open, s_effects;
static uint8_t s_track;
static int16_t s_patch_at[ROWS][COLS]; /* -1: none */
static uint8_t s_patch;                /* the track's, as picked here */
static float s_fx[COLS];               /* the track's amounts, as set here */
static uint32_t s_fx_sent_ms[COLS];
static bool s_prev_touched[ROWS * COLS];
static bool s_picked[ROWS * COLS]; /* this touch has picked already */
static bool s_minus_was, s_plus_was;
static uint8_t s_preview_note;
static bool s_preview_on;
static uint32_t s_preview_until;

static void preview_off(void) {
    if (s_preview_on) {
        tiles_usb_audio_midi((uint8_t)(0x80u | k_track_channel[s_track]), s_preview_note, 0u);
        s_preview_on = false;
    }
}

static void load_track(void) {
    s_patch = tiles_audio_engine_patch(s_track);
    for (uint32_t c = 0; c < COLS; c++) {
        s_fx[c] = tiles_audio_engine_fx(s_track, k_fx_param[c]);
        s_fx_sent_ms[c] = 0u;
    }
}

void tiles_sound_menu_open(bool effects, uint8_t track) {
    preview_off();
    s_open = true;
    s_effects = effects;
    s_track = (uint8_t)(track % 4u);
    for (uint32_t r = 0; r < ROWS; r++) {
        for (uint32_t c = 0; c < COLS; c++) {
            s_patch_at[r][c] = -1;
            for (uint8_t p = 0; k_rows[r][c] != NULL && p < tiles_audio_engine_patch_count(); p++) {
                if (strcmp(k_rows[r][c], tiles_audio_engine_patch_name(p)) == 0) {
                    s_patch_at[r][c] = (int16_t)p;
                }
            }
        }
    }
    load_track();
    for (uint8_t i = 0; i < ROWS * COLS; i++) {
        s_prev_touched[i] = tiles_touch_is_touched((uint8_t)(i + 1u));
        s_picked[i] = s_prev_touched[i]; /* a finger already down picks nothing */
    }
    s_minus_was = tiles_button_is_pressed(1u);
    s_plus_was = tiles_button_is_pressed(2u);
}

void tiles_sound_menu_close(void) {
    preview_off();
    s_open = false;
}

bool tiles_sound_menu_is_open(void) {
    return s_open;
}

bool tiles_sound_menu_is_effects(void) {
    return s_open && s_effects;
}

static void pick_sound(uint32_t row, uint32_t col, uint32_t now_ms) {
    int16_t p = s_patch_at[row][col];
    if (p < 0) {
        return;
    }
    preview_off();
    if ((uint8_t)p != s_patch && tiles_usb_audio_set_patch(s_track, (uint8_t)p)) {
        s_patch = (uint8_t)p;
    }
    s_preview_note = k_row_preview[row];
    tiles_usb_audio_midi((uint8_t)(0x90u | k_track_channel[s_track]), s_preview_note, 100u);
    s_preview_on = true;
    s_preview_until = now_ms + PREVIEW_MS;
}

/* Row 1 (top) is full, row 4 off; pressing deeper adds up to a third more. */
static void set_effect(uint32_t row, uint32_t col, float depth, uint32_t now_ms) {
    float level = (float)(ROWS - 1u - row) / (float)(ROWS - 1u);
    float extra = (depth - PICK_DEPTH) / (FULL_DEPTH - PICK_DEPTH);
    extra = extra < 0.0f ? 0.0f : extra > 1.0f ? 1.0f : extra;
    float v = level + extra / (float)(ROWS - 1u);
    v = v > 1.0f ? 1.0f : v;
    float d = v - s_fx[col];
    if ((d > 0.005f || d < -0.005f) && now_ms - s_fx_sent_ms[col] >= FX_SEND_MS &&
        tiles_usb_audio_set_fx(s_track, k_fx_param[col], v)) {
        s_fx[col] = v;
        s_fx_sent_ms[col] = now_ms;
    }
}

static void handle_input(uint32_t now_ms) {
    bool minus = tiles_button_is_pressed(1u), plus = tiles_button_is_pressed(2u);
    if ((minus && !s_minus_was) || (plus && !s_plus_was)) {
        preview_off();
        s_track = (uint8_t)((s_track + (plus && !s_plus_was ? 1u : 3u)) % 4u);
        load_track();
    }
    s_minus_was = minus;
    s_plus_was = plus;

    for (uint32_t row = 0; row < ROWS; row++) {
        for (uint32_t col = 0; col < COLS; col++) {
            uint8_t pad = board_pad_for_row_col((uint8_t)(row + 1u), (uint8_t)(col + 1u));
            uint32_t i = pad - 1u;
            bool touched = tiles_touch_is_touched(pad);
            if (touched && !s_prev_touched[i]) {
                tiles_haptics_trigger_touch_pulse(pad);
            }
            if (!touched) {
                s_picked[i] = false;
            }
            s_prev_touched[i] = touched;
            float depth = touched ? (float)tiles_hall_get_depth(pad) : 0.0f;
            if (!touched || depth < PICK_DEPTH) {
                continue;
            }
            if (s_effects) {
                set_effect(row, col, depth, now_ms);
            } else if (!s_picked[i]) {
                s_picked[i] = true;
                pick_sound(row, col, now_ms);
            }
        }
    }
    if (s_preview_on && (int32_t)(now_ms - s_preview_until) >= 0) {
        preview_off();
    }
}

static void render(void) {
    float frame[ROWS * COLS][3];
    memset(frame, 0, sizeof(frame));
    for (uint32_t row = 0; row < ROWS; row++) {
        for (uint32_t col = 0; col < COLS; col++) {
            float *px = frame[board_pad_for_row_col((uint8_t)(row + 1u), (uint8_t)(col + 1u)) - 1u];
            const float *rgb;
            float level;
            if (s_effects) {
                /* The bottom row always shows the column; the rows above
                 * light as the amount reaches them. */
                rgb = k_fx_rgb[col];
                uint32_t step = ROWS - 1u - row; /* 0 = the off row .. 3 = full */
                level = DIM;
                if (step > 0u) {
                    float reach = s_fx[col] * (float)(ROWS - 1u) - (float)(step - 1u);
                    level = reach < 0.0f ? 0.0f : reach > 1.0f ? 1.0f : reach;
                }
            } else {
                int16_t p = s_patch_at[row][col];
                rgb = k_row_rgb[row];
                level = p < 0 ? 0.0f : (uint8_t)p == s_patch ? 1.0f : DIM;
            }
            px[0] = rgb[0] * level;
            px[1] = rgb[1] * level;
            px[2] = rgb[2] * level;
        }
    }
    tiles_lighting_set_standby_frame(frame, 6u);
    const float *t = k_track_rgb[s_track];
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        tiles_lighting_set_standby_underglow_rgb(i, t[0], t[1], t[2]);
    }
    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        float level = 0.0f;
        if (col == 1u || col == 2u) {
            level = KEY_DIM; /* "-"/"+": the track */
        } else if (col == TILES_DIAMOND_BUTTON_COL || (s_effects && col == TILES_CIRCLE_BUTTON_COL)) {
            level = 1.0f; /* the menu's own keys */
        }
        tiles_buttons_set_standby_led(board_button_for_col(col), level);
    }
}

void tiles_sound_menu_scan(uint32_t now_ms) {
    if (!s_open) {
        return;
    }
    handle_input(now_ms);
    render();
}
