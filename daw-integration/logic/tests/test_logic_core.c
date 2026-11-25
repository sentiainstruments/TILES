#include "../bridge/tiles_logic_core.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define MAX_EVENTS 128u

typedef struct {
    tiles_out_target_t target;
    uint8_t bytes[32];
    size_t len;
} recorded_msg_t;

typedef struct {
    recorded_msg_t items[MAX_EVENTS];
    size_t count;
} recorder_t;

static void record_emit(tiles_out_target_t target, const uint8_t *bytes, size_t len, void *user_data) {
    recorder_t *rec = (recorder_t *)user_data;
    assert(rec->count < MAX_EVENTS);
    recorded_msg_t *slot = &rec->items[rec->count++];
    slot->target = target;
    assert(len <= sizeof(slot->bytes));
    memcpy(slot->bytes, bytes, len);
    slot->len = len;
}

static bool has_note(const recorder_t *rec, tiles_out_target_t target, uint8_t ch, uint8_t note, uint8_t vel) {
    uint8_t st = (uint8_t)(0x90u | (ch & 0x0Fu));
    for (size_t i = 0; i < rec->count; i++) {
        if (rec->items[i].target == target && rec->items[i].len == 3u &&
            rec->items[i].bytes[0] == st && rec->items[i].bytes[1] == note && rec->items[i].bytes[2] == vel) {
            return true;
        }
    }
    return false;
}

static bool has_cc(const recorder_t *rec, tiles_out_target_t target, uint8_t ch, uint8_t cc, uint8_t val) {
    uint8_t st = (uint8_t)(0xB0u | (ch & 0x0Fu));
    for (size_t i = 0; i < rec->count; i++) {
        if (rec->items[i].target == target && rec->items[i].len == 3u &&
            rec->items[i].bytes[0] == st && rec->items[i].bytes[1] == cc && rec->items[i].bytes[2] == val) {
            return true;
        }
    }
    return false;
}

static bool has_sysex(const recorder_t *rec, tiles_out_target_t target, uint8_t msg_type, size_t min_len) {
    for (size_t i = 0; i < rec->count; i++) {
        if (rec->items[i].target == target && rec->items[i].len >= min_len &&
            rec->items[i].bytes[0] == 0xF0u &&
            rec->items[i].bytes[1] == TILES_SYSEX_MFR_ID &&
            rec->items[i].bytes[2] == TILES_SYSEX_SUB_ID &&
            rec->items[i].bytes[3] == msg_type) {
            return true;
        }
    }
    return false;
}

static void test_transport_and_performance_lockout(void) {
    tiles_logic_ctx_t ctx;
    recorder_t rec;
    tiles_logic_init(&ctx, 1u);

    /* Click Play while stopped -> MCU Play (0x5E). */
    memset(&rec, 0, sizeof(rec));
    uint8_t play_cc[3] = {0xB0u, TILES_CC_PLAY, 127u};
    tiles_logic_on_tiles_daw_midi(&ctx, play_cc, 3u, 100u, record_emit, &rec);
    assert(ctx.logic_playing == true);
    assert(has_note(&rec, TILES_OUT_LOGIC_MCU, 0u, 0x5Eu, 127u));

    /* Second Play click while already playing is ignored in performance mode. */
    memset(&rec, 0, sizeof(rec));
    tiles_logic_on_tiles_daw_midi(&ctx, play_cc, 3u, 200u, record_emit, &rec);
    assert(rec.count == 0u);

    /* Hold Record while already playing is ignored in performance mode. */
    uint8_t rec_cc[3] = {0xB0u, TILES_CC_RECORD, 127u};
    tiles_logic_on_tiles_daw_midi(&ctx, rec_cc, 3u, 300u, record_emit, &rec);
    assert(rec.count == 0u);

    /* Circle+Diamond Stop (CC 103) -> MCU Stop (0x5D). */
    uint8_t stop_cc[3] = {0xB0u, TILES_CC_STOP, 127u};
    tiles_logic_on_tiles_daw_midi(&ctx, stop_cc, 3u, 400u, record_emit, &rec);
    assert(ctx.logic_playing == false);
    assert(has_note(&rec, TILES_OUT_LOGIC_MCU, 0u, 0x5Du, 127u));

    /* Record while stopped -> arms/starts MCU Record (0x5F). */
    memset(&rec, 0, sizeof(rec));
    tiles_logic_on_tiles_daw_midi(&ctx, rec_cc, 3u, 500u, record_emit, &rec);
    assert(ctx.logic_recording == true);
    assert(ctx.logic_playing == true);
    assert(has_note(&rec, TILES_OUT_LOGIC_MCU, 0u, 0x5Fu, 127u));
}

static void test_live_loops_grid_and_sysex_haptics(void) {
    tiles_logic_ctx_t ctx;
    recorder_t rec;
    tiles_logic_init(&ctx, 2u);

    /* Host reports clip in Track 2, Scene 1 (playing) on TILES Logic Grid port. */
    memset(&rec, 0, sizeof(rec));
    uint8_t cell_sysex[11] = {
        0xF0u, TILES_SYSEX_MFR_ID, TILES_SYSEX_SUB_ID, TILES_MSG_CLIP_STATE,
        1u, 0u, TILES_FLAG_HAS_CLIP | TILES_FLAG_IS_PLAYING, 127u, 0u, 127u, 0xF7u,
    };
    tiles_logic_on_grid_midi(&ctx, cell_sysex, sizeof(cell_sysex), record_emit, &rec);
    assert(ctx.loop_cells[1][0].has_clip == true);
    assert(ctx.loop_cells[1][0].is_playing == true);
    assert(has_sysex(&rec, TILES_OUT_TILES_DAW, TILES_MSG_CLIP_STATE, 11u));

    /* Clicking Pad 2 on Live Loops fires Cell + Note feedback. */
    memset(&rec, 0, sizeof(rec));
    uint8_t grid_touch[3] = {0xB0u, TILES_CC_GRID_TOUCH, 2u};
    tiles_logic_on_tiles_daw_midi(&ctx, grid_touch, 3u, 1000u, record_emit, &rec);
    assert(has_cc(&rec, TILES_OUT_LOGIC_GRID, TILES_GRID_OUT_CHANNEL, TILES_GRID_CC_CELL_FIRE, 2u));
    assert(has_note(&rec, TILES_OUT_LOGIC_GRID, 0u, 37u, 127u));

    /* Clicking Col 6 (Pad 12 = Scene 2) launches Scene 2. */
    memset(&rec, 0, sizeof(rec));
    uint8_t scene_touch[3] = {0xB0u, TILES_CC_GRID_TOUCH, 12u};
    tiles_logic_on_tiles_daw_midi(&ctx, scene_touch, 3u, 1100u, record_emit, &rec);
    assert(has_cc(&rec, TILES_OUT_LOGIC_GRID, TILES_GRID_OUT_CHANNEL, TILES_GRID_CC_SCENE_FIRE, 2u));

    /* Circle+Empty Record (Pad 3) exclusive-arms Track 3 and opens Melodic Mode (0x12). */
    memset(&rec, 0, sizeof(rec));
    ctx.strips[0].rec_armed = true;
    uint8_t rec_touch[3] = {0xB0u, TILES_CC_RECORD_TOUCH, 3u};
    tiles_logic_on_tiles_daw_midi(&ctx, rec_touch, 3u, 1200u, record_emit, &rec);
    assert(ctx.strips[0].rec_armed == false);
    assert(ctx.strips[2].rec_armed == true);
    assert(ctx.capture_active == true);
    assert(has_note(&rec, TILES_OUT_LOGIC_MCU, 0u, 0x00u, 127u)); /* disarm track 0 */
    assert(has_note(&rec, TILES_OUT_LOGIC_MCU, 0u, 0x02u, 127u)); /* arm track 2 */
    assert(has_sysex(&rec, TILES_OUT_TILES_DAW, TILES_MSG_OPEN_MELODIC, 5u));

    /* End capture (CC 107) stops recording capture. */
    memset(&rec, 0, sizeof(rec));
    uint8_t end_cap[3] = {0xB0u, TILES_CC_END_CAPTURE, 127u};
    tiles_logic_on_tiles_daw_midi(&ctx, end_cap, 3u, 1400u, record_emit, &rec);
    assert(ctx.capture_active == false);
    assert(has_cc(&rec, TILES_OUT_LOGIC_GRID, TILES_GRID_OUT_CHANNEL, TILES_GRID_CC_END_CAPTURE, 3u));
}

static void test_pages_smart_controls_and_mixer_banking(void) {
    tiles_logic_ctx_t ctx;
    recorder_t rec;
    tiles_logic_init(&ctx, 1u);

    /* Single Master Stop emits CC 29; rapid second tap cycles to Smart Controls. */
    memset(&rec, 0, sizeof(rec));
    uint8_t mstop[3] = {0xB0u, TILES_CC_MASTER_STOP, 127u};
    tiles_logic_on_tiles_daw_midi(&ctx, mstop, 3u, 1000u, record_emit, &rec);
    assert(ctx.page == TILES_PAGE_LIVE_LOOPS);
    assert(has_cc(&rec, TILES_OUT_LOGIC_GRID, TILES_GRID_OUT_CHANNEL, TILES_GRID_CC_MASTER_STOP, 127u));

    memset(&rec, 0, sizeof(rec));
    tiles_logic_on_tiles_daw_midi(&ctx, mstop, 3u, 1250u, record_emit, &rec);
    assert(ctx.page == TILES_PAGE_SMART_CONTROLS);

    /* Tap Pad 1 on Smart Controls -> MCU V-Pot 0 increment (+6) + Grid CC 102. */
    memset(&rec, 0, sizeof(rec));
    uint8_t pad1[3] = {0xB0u, TILES_CC_GRID_TOUCH, 1u};
    tiles_logic_on_tiles_daw_midi(&ctx, pad1, 3u, 1300u, record_emit, &rec);
    assert(has_cc(&rec, TILES_OUT_LOGIC_MCU, 0u, 0x10u, 0x06u));
    assert(has_cc(&rec, TILES_OUT_LOGIC_GRID, TILES_GRID_OUT_CHANNEL, 102u, 80u));

    /* Tap Pad 6 -> advances from Smart Controls to Mixer page. */
    memset(&rec, 0, sizeof(rec));
    uint8_t pad6[3] = {0xB0u, TILES_CC_GRID_TOUCH, 6u};
    tiles_logic_on_tiles_daw_midi(&ctx, pad6, 3u, 1400u, record_emit, &rec);
    assert(ctx.page == TILES_PAGE_MIXER);

    /* Shift track window to offset 10 (Track 11 = MCU bank 1, ch 2), mute col 1 -> Track 10 (bank 1, ch 2). */
    memset(&rec, 0, sizeof(rec));
    uint8_t off10[3] = {0xB0u, TILES_CC_TRACK_OFFSET, 10u};
    tiles_logic_on_tiles_daw_midi(&ctx, off10, 3u, 1500u, record_emit, &rec);
    assert(ctx.mcu_bank_offset == 1u);
    assert(has_note(&rec, TILES_OUT_LOGIC_MCU, 0u, 0x2Fu, 127u));

    /* Tap Pad 1 (row 0 Mute, col 1 -> abs_track 10 = bank 1 ch 2). */
    memset(&rec, 0, sizeof(rec));
    tiles_logic_on_tiles_daw_midi(&ctx, pad1, 3u, 1600u, record_emit, &rec);
    assert(ctx.strips[10].muted == true);
    assert(has_note(&rec, TILES_OUT_LOGIC_MCU, 0u, (uint8_t)(0x10u + 2u), 127u));

    /* Grid CC 113 = 3 switches to Mixer Group 4 (track_offset 15, fader_group_color 3) and repaints SysEx LEDs. */
    memset(&rec, 0, sizeof(rec));
    uint8_t grid_bank[3] = {0xB0u, TILES_LOGIC_CC_MIXER_BANK, 3u};
    tiles_logic_on_grid_midi(&ctx, grid_bank, 3u, record_emit, &rec);
    assert(ctx.track_offset == 15u);
    assert(ctx.fader_group_color == 3u);
    assert(has_sysex(&rec, TILES_OUT_TILES_DAW, TILES_MSG_CLIP_STATE, 11u));

    /* Grid CC 112 = 5 sets fader group LED color to Magenta (5) and repaints immediately. */
    memset(&rec, 0, sizeof(rec));
    uint8_t grid_color[3] = {0xB0u, TILES_LOGIC_CC_FADER_COLOR, 5u};
    tiles_logic_on_grid_midi(&ctx, grid_color, 3u, record_emit, &rec);
    assert(ctx.fader_group_color == 5u);
    assert(has_sysex(&rec, TILES_OUT_TILES_DAW, TILES_MSG_CLIP_STATE, 11u));

    /* Per-fader track color hints on Ch 16 (CC 32..36) update visible track LEDs independently. */
    memset(&rec, 0, sizeof(rec));
    uint8_t ch16_col0_amber[3] = {(uint8_t)(0xB0u | TILES_GRID_OUT_CHANNEL), (uint8_t)(TILES_GRID_CC_FADER_COLOR_BASE + 0u), 63u};
    uint8_t ch16_col1_teal[3] = {(uint8_t)(0xB0u | TILES_GRID_OUT_CHANNEL), (uint8_t)(TILES_GRID_CC_FADER_COLOR_BASE + 1u), 42u};
    tiles_logic_on_grid_midi(&ctx, ch16_col0_amber, 3u, record_emit, &rec);
    tiles_logic_on_grid_midi(&ctx, ch16_col1_teal, 3u, record_emit, &rec);
    assert(ctx.strips[15].has_custom_color == true && ctx.strips[15].color_code == 2u);
    assert(ctx.strips[16].has_custom_color == true && ctx.strips[16].color_code == 1u);
    assert(has_sysex(&rec, TILES_OUT_TILES_DAW, TILES_MSG_CLIP_STATE, 11u));

    /* Grid CC 114 steps mixer bank (+5 for >= 64, -5 for < 64) and aligns MCU bank. */
    memset(&rec, 0, sizeof(rec));
    uint8_t bank_step_up[3] = {0xB0u, TILES_LOGIC_CC_BANK_STEP, 127u};
    tiles_logic_on_grid_midi(&ctx, bank_step_up, 3u, record_emit, &rec);
    assert(ctx.track_offset == 20u);
    assert(ctx.fader_group_color == 4u);

    memset(&rec, 0, sizeof(rec));
    uint8_t bank_step_dn[3] = {0xB0u, TILES_LOGIC_CC_BANK_STEP, 0u};
    tiles_logic_on_grid_midi(&ctx, bank_step_dn, 3u, record_emit, &rec);
    assert(ctx.track_offset == 15u);
    assert(ctx.fader_group_color == 3u);
}

static void test_mcu_handshake_and_feedback(void) {
    tiles_logic_ctx_t ctx;
    recorder_t rec;
    tiles_logic_init(&ctx, 3u);

    /* Universal Device Inquiry -> responds with Mackie Control ID reply. */
    memset(&rec, 0, sizeof(rec));
    uint8_t inquiry[6] = {0xF0u, 0x7Eu, 0x7Fu, 0x06u, 0x01u, 0xF7u};
    tiles_logic_on_mcu_midi(&ctx, inquiry, sizeof(inquiry), record_emit, &rec);
    assert(rec.count == 1u);
    assert(rec.items[0].target == TILES_OUT_LOGIC_MCU);
    assert(rec.items[0].len == 17u);
    assert(rec.items[0].bytes[8] == 0x14u);

    /* Host Connection Query -> returns serial ending with unit 3 ('3'). */
    memset(&rec, 0, sizeof(rec));
    uint8_t query[7] = {0xF0u, 0x00u, 0x00u, 0x66u, 0x14u, 0x00u, 0xF7u};
    tiles_logic_on_mcu_midi(&ctx, query, sizeof(query), record_emit, &rec);
    assert(rec.count == 1u);
    assert(rec.items[0].len == 18u);
    assert(rec.items[0].bytes[5] == 0x01u);
    assert(rec.items[0].bytes[12] == '3');

    /* MCU V-Pot LED ring update (0xB0 0x32 0x0B) updates macro 2 ring_level=11. */
    memset(&rec, 0, sizeof(rec));
    uint8_t ring[3] = {0xB0u, 0x32u, 0x0Bu};
    tiles_logic_on_mcu_midi(&ctx, ring, 3u, record_emit, &rec);
    assert(ctx.macros[2].ring_level == 11u);
    assert(ctx.macros[2].value == 127u);
}

static void test_mpe_independent_expression_split_and_display(void) {
    tiles_logic_ctx_t ctx;
    recorder_t rec;
    tiles_logic_init(&ctx, 1u);

    /* Pad A on MPE member ch 2 (idx 1) strike, press, tilt. */
    memset(&rec, 0, sizeof(rec));
    uint8_t note_a[3] = {0x91u, 60u, 110u};
    uint8_t press_a[2] = {0xD1u, 85u};
    uint8_t tilt_a[3] = {0xE1u, 0x00u, 96u};
    tiles_logic_on_tiles_mpe_midi(&ctx, note_a, 3u, record_emit, &rec);
    tiles_logic_on_tiles_mpe_midi(&ctx, press_a, 2u, record_emit, &rec);
    tiles_logic_on_tiles_mpe_midi(&ctx, tilt_a, 3u, record_emit, &rec);

    assert(has_cc(&rec, TILES_OUT_LOGIC_GRID, 1u, TILES_EXPR_VOICE_CC_TOUCH, 127u));
    assert(has_cc(&rec, TILES_OUT_LOGIC_GRID, 1u, TILES_EXPR_VOICE_CC_STRIKE, 110u));
    assert(has_cc(&rec, TILES_OUT_LOGIC_GRID, 1u, TILES_EXPR_VOICE_CC_PRESS, 85u));
    assert(has_cc(&rec, TILES_OUT_LOGIC_GRID, 1u, TILES_EXPR_VOICE_CC_TILT, 96u));
    assert(has_cc(&rec, TILES_OUT_LOGIC_GRID, TILES_EXPR_FOCUS_CHANNEL, TILES_EXPR_CC_TOUCH, 127u));
    assert(has_cc(&rec, TILES_OUT_LOGIC_GRID, TILES_EXPR_FOCUS_CHANNEL, TILES_EXPR_CC_STRIKE, 110u));
    assert(has_cc(&rec, TILES_OUT_LOGIC_GRID, TILES_EXPR_FOCUS_CHANNEL, TILES_EXPR_CC_PRESS, 85u));
    assert(has_cc(&rec, TILES_OUT_LOGIC_GRID, TILES_EXPR_FOCUS_CHANNEL, TILES_EXPR_CC_TILT, 96u));

    /* Simultaneous Pad B on MPE member ch 3 (idx 2) stays independent of Pad A voice. */
    memset(&rec, 0, sizeof(rec));
    uint8_t press_b[2] = {0xD2u, 42u};
    tiles_logic_on_tiles_mpe_midi(&ctx, press_b, 2u, record_emit, &rec);
    assert(ctx.voices[1].press == 85u);
    assert(ctx.voices[2].press == 42u);
    assert(has_cc(&rec, TILES_OUT_LOGIC_GRID, 2u, TILES_EXPR_VOICE_CC_PRESS, 42u));

    /* Display MIDI echoes primary (Ch 1) and secondary (Ch 2) straight to TILES DAW out. */
    memset(&rec, 0, sizeof(rec));
    uint8_t disp_primary[3] = {0x90u, 60u, 100u};
    uint8_t disp_secondary[3] = {0x91u, 64u, 90u};
    tiles_logic_on_display_midi(&ctx, disp_primary, 3u, record_emit, &rec);
    tiles_logic_on_display_midi(&ctx, disp_secondary, 3u, record_emit, &rec);
    assert(has_note(&rec, TILES_OUT_TILES_DAW, 0u, 60u, 100u));
    assert(has_note(&rec, TILES_OUT_TILES_DAW, 1u, 64u, 90u));
}

int main(void) {
    test_transport_and_performance_lockout();
    test_live_loops_grid_and_sysex_haptics();
    test_pages_smart_controls_and_mixer_banking();
    test_mcu_handshake_and_feedback();
    test_mpe_independent_expression_split_and_display();
    printf("All SENTIA TILES Logic Pro integration core tests passed.\n");
    return 0;
}
