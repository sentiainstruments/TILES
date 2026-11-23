#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Matches firmware/src/midi/product_identity.h and board_pins.h. */
#define TILES_USB_VID 0x1209u
#define TILES_USB_PID 0x0001u
#define TILES_SYSEX_MFR_ID 0x7Du
#define TILES_SYSEX_SUB_ID 0x01u
#define TILES_NUM_PADS 24u
#define TILES_NUM_COLS 6u
#define TILES_NUM_ROWS 4u
#define TILES_VISIBLE_TRACKS 5u
#define TILES_MAX_TRACKS 64u
#define TILES_NUM_MACROS 8u

/* Matches firmware/src/midi/midi_out.h (0 = MIDI channel 1). */
#define TILES_MASTER_CHANNEL 0u
#define TILES_MPE_FIRST_MEMBER_CH 1u
#define TILES_MPE_LAST_MEMBER_CH 8u

/* Matches OP_TRANSPORT_*_CC and OP_SCENE_CC_* in firmware/src/services/op_mode.c. */
#define TILES_CC_PLAY 102u
#define TILES_CC_STOP 103u
#define TILES_CC_RECORD 104u
#define TILES_CC_MASTER_STOP 105u
#define TILES_CC_TRACK_OFFSET 106u
#define TILES_CC_END_CAPTURE 107u
#define TILES_CC_GRID_TOUCH 108u
#define TILES_CC_STOP_TOUCH 109u
#define TILES_CC_DELETE_TOUCH 110u
#define TILES_CC_RECORD_TOUCH 111u

/* Bridge control CCs on TILES Logic Grid (channel 1) for Logic key commands & bank sync. */
#define TILES_LOGIC_CC_LOG_ENABLE 85u
#define TILES_LOGIC_CC_LOG_FILTER 86u
#define TILES_LOGIC_CC_FADER_COLOR 112u
#define TILES_LOGIC_CC_MIXER_BANK 113u
#define TILES_LOGIC_CC_BANK_STEP 114u
#define TILES_LOGIC_CC_EXPR_SPLIT 118u
#define TILES_LOGIC_CC_PAGE_SELECT 119u

/* Matches OP_SCENE_MSG_* and OP_SCENE_FLAG_* in op_mode.c. */
#define TILES_MSG_CLIP_STATE 0x10u
#define TILES_MSG_SCENE_STATE 0x11u
#define TILES_MSG_OPEN_MELODIC 0x12u

#define TILES_FLAG_HAS_CLIP 0x01u
#define TILES_FLAG_IS_PLAYING 0x02u
#define TILES_FLAG_IS_TRIGGERED 0x04u

/* Auto-Assign focused-pad expression CCs on channel 16 (0x0F). */
#define TILES_EXPR_FOCUS_CHANNEL 15u
#define TILES_EXPR_CC_STRIKE 20u
#define TILES_EXPR_CC_PRESS 21u
#define TILES_EXPR_CC_TILT 22u
#define TILES_EXPR_CC_TOUCH 23u

/* Per-voice independent expression CCs on member channels 2..9 (1..8). */
#define TILES_EXPR_VOICE_CC_STRIKE 14u
#define TILES_EXPR_VOICE_CC_PRESS 15u
#define TILES_EXPR_VOICE_CC_TILT 16u
#define TILES_EXPR_VOICE_CC_TOUCH 17u

/* Live Loops Grid port CCs (channel 16, 0-based 15) for Logic Controller Assignments. */
#define TILES_GRID_OUT_CHANNEL 15u
#define TILES_GRID_CC_CELL_FIRE 24u
#define TILES_GRID_CC_CELL_STOP 25u
#define TILES_GRID_CC_CELL_RECORD 26u
#define TILES_GRID_CC_CELL_DELETE 27u
#define TILES_GRID_CC_SCENE_FIRE 28u
#define TILES_GRID_CC_MASTER_STOP 29u
#define TILES_GRID_CC_END_CAPTURE 30u
#define TILES_GRID_CC_FADER_COLOR_BASE 32u

typedef enum {
    TILES_PAGE_LIVE_LOOPS = 0,
    TILES_PAGE_SMART_CONTROLS = 1,
    TILES_PAGE_MIXER = 2,
    TILES_PAGE_COUNT = 3,
} tiles_logic_page_t;

typedef enum {
    TILES_MCU_ASSIGN_TRACK = 0,
    TILES_MCU_ASSIGN_SEND = 1,
    TILES_MCU_ASSIGN_PAN = 2,
    TILES_MCU_ASSIGN_PLUGIN = 3,
} tiles_mcu_assign_t;

typedef enum {
    TILES_OUT_TILES_DAW = 0,
    TILES_OUT_LOGIC_MCU = 1,
    TILES_OUT_LOGIC_GRID = 2,
} tiles_out_target_t;

typedef struct {
    bool has_clip;
    bool is_playing;
    bool is_triggered;
    uint8_t r7;
    uint8_t g7;
    uint8_t b7;
} tiles_cell_state_t;

typedef struct {
    bool is_triggered;
    uint8_t r7;
    uint8_t g7;
    uint8_t b7;
} tiles_scene_state_t;

typedef struct {
    bool muted;
    bool soloed;
    bool rec_armed;
    bool selected;
    bool has_custom_color;
    uint8_t color_code;
    uint8_t fader_msb;
    uint8_t vpot_ring; /* 0..11 */
} tiles_strip_state_t;

typedef struct {
    uint8_t value;       /* 0..127 */
    uint8_t ring_level;  /* 0..11 */
    bool dirty;
} tiles_macro_state_t;

typedef struct {
    bool active;
    uint8_t note;
    uint8_t strike_vel;
    uint8_t press;
    uint8_t tilt_cc;
} tiles_voice_expr_t;

typedef struct {
    uint8_t unit_number; /* 1..4 */
    tiles_logic_page_t page;
    tiles_mcu_assign_t mcu_assign;
    bool expr_split_enabled;
    bool diamond_ignored_while_playing;

    /* Mirrors Logic Pro MCU transport LEDs. */
    bool logic_playing;
    bool logic_recording;
    bool logic_cycle;

    /* Track window synced with CC 106 (0..59) and active fader group palette (0..5). */
    uint8_t track_offset;
    uint8_t mcu_bank_offset;
    uint8_t macro_bank;
    uint8_t fader_group_color;

    /* Rapid Circle+Diamond double-tap detection for page cycling. */
    uint64_t last_master_stop_ms;
    bool capture_active;
    uint8_t capture_pad;

    /* Live Loops state (64 tracks x 4 scenes) and visible pad SysEx cache. */
    tiles_cell_state_t loop_cells[TILES_MAX_TRACKS][TILES_NUM_ROWS];
    tiles_scene_state_t loop_scenes[TILES_NUM_ROWS];
    tiles_cell_state_t sent_pads[TILES_VISIBLE_TRACKS][TILES_NUM_ROWS];
    tiles_scene_state_t sent_scenes[TILES_NUM_ROWS];
    bool pad_cache_valid;

    /* Mixer & Smart Control mirrors (from Logic MCU feedback). */
    tiles_strip_state_t strips[TILES_MAX_TRACKS];
    tiles_macro_state_t macros[TILES_NUM_MACROS];

    /* Independent per-voice MPE expression tracking for Auto-Assign split. */
    tiles_voice_expr_t voices[16];
    uint8_t focused_voice_ch;
} tiles_logic_ctx_t;

typedef void (*tiles_midi_emit_fn)(tiles_out_target_t target, const uint8_t *bytes, size_t len, void *user_data);

void tiles_logic_init(tiles_logic_ctx_t *ctx, uint8_t unit_number);

/* Processes a MIDI message arriving from SENTIA TILES DAW port. */
void tiles_logic_on_tiles_daw_midi(
    tiles_logic_ctx_t *ctx,
    const uint8_t *bytes,
    size_t len,
    uint64_t now_ms,
    tiles_midi_emit_fn emit,
    void *user_data
);

/* Processes a passive tap of SENTIA TILES MIDI (MPE) port for Auto-Assign split. */
void tiles_logic_on_tiles_mpe_midi(
    tiles_logic_ctx_t *ctx,
    const uint8_t *bytes,
    size_t len,
    tiles_midi_emit_fn emit,
    void *user_data
);

/* Processes feedback arriving from Logic Pro on TILES Logic MCU port. */
void tiles_logic_on_mcu_midi(
    tiles_logic_ctx_t *ctx,
    const uint8_t *bytes,
    size_t len,
    tiles_midi_emit_fn emit,
    void *user_data
);

/* Processes control/feedback arriving from Logic Pro on TILES Logic Grid port. */
void tiles_logic_on_grid_midi(
    tiles_logic_ctx_t *ctx,
    const uint8_t *bytes,
    size_t len,
    tiles_midi_emit_fn emit,
    void *user_data
);

/* Processes note display echoes arriving on TILES Logic Display port. */
void tiles_logic_on_display_midi(
    tiles_logic_ctx_t *ctx,
    const uint8_t *bytes,
    size_t len,
    tiles_midi_emit_fn emit,
    void *user_data
);

void tiles_logic_set_page(
    tiles_logic_ctx_t *ctx,
    tiles_logic_page_t page,
    tiles_midi_emit_fn emit,
    void *user_data
);

void tiles_logic_repaint(
    tiles_logic_ctx_t *ctx,
    bool force_full,
    tiles_midi_emit_fn emit,
    void *user_data
);

#ifdef __cplusplus
}
#endif
