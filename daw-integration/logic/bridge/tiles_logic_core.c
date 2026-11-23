#include "tiles_logic_core.h"

#include <string.h>

/* Sentia palette halved to 7-bit RGB for TILES SysEx (docs/style/manuals.md). */
#define RGB7_MAGENTA_R 127u
#define RGB7_MAGENTA_G 0u
#define RGB7_MAGENTA_B 127u

#define RGB7_PERIWINKLE_R 74u
#define RGB7_PERIWINKLE_G 85u
#define RGB7_PERIWINKLE_B 124u

#define RGB7_INK_R 12u
#define RGB7_INK_G 15u
#define RGB7_INK_B 29u

#define RGB7_AMBER_R 115u
#define RGB7_AMBER_G 70u
#define RGB7_AMBER_B 12u

#define RGB7_GOLD_R 120u
#define RGB7_GOLD_G 105u
#define RGB7_GOLD_B 15u

#define RGB7_CORAL_R 118u
#define RGB7_CORAL_G 24u
#define RGB7_CORAL_B 24u

#define RGB7_TEAL_R 18u
#define RGB7_TEAL_G 95u
#define RGB7_TEAL_B 95u

#define RGB7_OFF_R 0u
#define RGB7_OFF_G 0u
#define RGB7_OFF_B 0u

/* Mackie Control transport, bank, channel, and assignment note IDs. */
#define MCU_NOTE_REC_ARM_BASE 0x00u
#define MCU_NOTE_SOLO_BASE 0x08u
#define MCU_NOTE_MUTE_BASE 0x10u
#define MCU_NOTE_SELECT_BASE 0x18u
#define MCU_NOTE_VPOT_PRESS_BASE 0x20u
#define MCU_NOTE_ASSIGN_TRACK 0x28u
#define MCU_NOTE_ASSIGN_SEND 0x29u
#define MCU_NOTE_ASSIGN_PAN 0x2Au
#define MCU_NOTE_ASSIGN_PLUGIN 0x2Bu
#define MCU_NOTE_BANK_LEFT 0x2Eu
#define MCU_NOTE_BANK_RIGHT 0x2Fu
#define MCU_NOTE_CH_LEFT 0x30u
#define MCU_NOTE_CH_RIGHT 0x31u
#define MCU_NOTE_UNDO 0x51u
#define MCU_NOTE_CYCLE 0x56u
#define MCU_NOTE_STOP 0x5Du
#define MCU_NOTE_PLAY 0x5Eu
#define MCU_NOTE_RECORD 0x5Fu

#define PAGE_DOUBLE_TAP_MS 450u

static void emit_cc(
    tiles_midi_emit_fn emit,
    void *user_data,
    tiles_out_target_t target,
    uint8_t ch,
    uint8_t cc,
    uint8_t val
) {
    uint8_t msg[3] = {(uint8_t)(0xB0u | (ch & 0x0Fu)), (uint8_t)(cc & 0x7Fu), (uint8_t)(val & 0x7Fu)};
    emit(target, msg, sizeof(msg), user_data);
}

static void emit_note(
    tiles_midi_emit_fn emit,
    void *user_data,
    tiles_out_target_t target,
    uint8_t ch,
    uint8_t note,
    uint8_t vel
) {
    uint8_t msg[3] = {(uint8_t)(0x90u | (ch & 0x0Fu)), (uint8_t)(note & 0x7Fu), (uint8_t)(vel & 0x7Fu)};
    emit(target, msg, sizeof(msg), user_data);
}

static void emit_mcu_button(tiles_midi_emit_fn emit, void *user_data, uint8_t note_id) {
    emit_note(emit, user_data, TILES_OUT_LOGIC_MCU, 0u, note_id, 0x7Fu);
    emit_note(emit, user_data, TILES_OUT_LOGIC_MCU, 0u, note_id, 0x00u);
}

static void emit_sysex_clip(
    tiles_midi_emit_fn emit,
    void *user_data,
    uint8_t track,
    uint8_t scene,
    const tiles_cell_state_t *cell
) {
    uint8_t flags = 0u;
    if (cell->has_clip) flags |= TILES_FLAG_HAS_CLIP;
    if (cell->is_playing) flags |= TILES_FLAG_IS_PLAYING;
    if (cell->is_triggered) flags |= TILES_FLAG_IS_TRIGGERED;

    uint8_t msg[11] = {
        0xF0u,
        TILES_SYSEX_MFR_ID,
        TILES_SYSEX_SUB_ID,
        TILES_MSG_CLIP_STATE,
        (uint8_t)(track & 0x7Fu),
        (uint8_t)(scene & 0x7Fu),
        (uint8_t)(flags & 0x7Fu),
        (uint8_t)(cell->r7 & 0x7Fu),
        (uint8_t)(cell->g7 & 0x7Fu),
        (uint8_t)(cell->b7 & 0x7Fu),
        0xF7u,
    };
    emit(TILES_OUT_TILES_DAW, msg, sizeof(msg), user_data);
}

static void emit_sysex_scene(
    tiles_midi_emit_fn emit,
    void *user_data,
    uint8_t scene,
    const tiles_scene_state_t *row
) {
    uint8_t flags = row->is_triggered ? TILES_FLAG_IS_TRIGGERED : 0u;
    uint8_t msg[10] = {
        0xF0u,
        TILES_SYSEX_MFR_ID,
        TILES_SYSEX_SUB_ID,
        TILES_MSG_SCENE_STATE,
        (uint8_t)(scene & 0x7Fu),
        (uint8_t)(flags & 0x7Fu),
        (uint8_t)(row->r7 & 0x7Fu),
        (uint8_t)(row->g7 & 0x7Fu),
        (uint8_t)(row->b7 & 0x7Fu),
        0xF7u,
    };
    emit(TILES_OUT_TILES_DAW, msg, sizeof(msg), user_data);
}

static void emit_sysex_open_melodic(tiles_midi_emit_fn emit, void *user_data) {
    uint8_t msg[5] = {0xF0u, TILES_SYSEX_MFR_ID, TILES_SYSEX_SUB_ID, TILES_MSG_OPEN_MELODIC, 0xF7u};
    emit(TILES_OUT_TILES_DAW, msg, sizeof(msg), user_data);
}

static bool cell_equal(const tiles_cell_state_t *a, const tiles_cell_state_t *b) {
    return a->has_clip == b->has_clip &&
           a->is_playing == b->is_playing &&
           a->is_triggered == b->is_triggered &&
           a->r7 == b->r7 &&
           a->g7 == b->g7 &&
           a->b7 == b->b7;
}

static bool scene_equal(const tiles_scene_state_t *a, const tiles_scene_state_t *b) {
    return a->is_triggered == b->is_triggered &&
           a->r7 == b->r7 &&
           a->g7 == b->g7 &&
           a->b7 == b->b7;
}

static tiles_cell_state_t render_smart_controls_cell(
    const tiles_logic_ctx_t *ctx,
    uint8_t col_0_4,
    uint8_t row_0_3
) {
    tiles_cell_state_t out = {0};
    out.has_clip = true;

    if (col_0_4 < 4u) {
        uint8_t macro_idx = (row_0_3 < 2u)
                                ? (uint8_t)(row_0_3 * 4u + col_0_4)
                                : (uint8_t)((row_0_3 - 2u) * 4u + col_0_4);
        const tiles_macro_state_t *m = &ctx->macros[macro_idx];
        uint8_t level = m->value;
        if (row_0_3 < 2u) {
            /* Top 2 rows: macro increment / value bar in Periwinkle. */
            out.is_playing = (level >= 96u);
            uint8_t scale = (uint8_t)(25u + (level * 102u) / 127u);
            out.r7 = (uint8_t)((RGB7_PERIWINKLE_R * scale) / 127u);
            out.g7 = (uint8_t)((RGB7_PERIWINKLE_G * scale) / 127u);
            out.b7 = (uint8_t)((RGB7_PERIWINKLE_B * scale) / 127u);
        } else {
            /* Rows 3-4: macro decrement / reset in muted Ink/Teal. */
            out.r7 = RGB7_INK_R;
            out.g7 = (uint8_t)(RGB7_INK_G + (level / 4u));
            out.b7 = (uint8_t)(RGB7_INK_B + (level / 2u));
        }
        return out;
    }

    /* Col 5 (index 4): MCU assignment mode buttons. */
    bool active = false;
    if (row_0_3 == 0u) active = (ctx->mcu_assign == TILES_MCU_ASSIGN_PLUGIN);
    if (row_0_3 == 1u) active = (ctx->mcu_assign == TILES_MCU_ASSIGN_PAN);
    if (row_0_3 == 2u) active = (ctx->mcu_assign == TILES_MCU_ASSIGN_SEND);
    if (row_0_3 == 3u) active = (ctx->mcu_assign == TILES_MCU_ASSIGN_TRACK);

    out.is_playing = active;
    out.r7 = active ? RGB7_MAGENTA_R : RGB7_INK_R;
    out.g7 = active ? RGB7_MAGENTA_G : RGB7_INK_G;
    out.b7 = active ? RGB7_MAGENTA_B : RGB7_INK_B;
    return out;
}

static void get_fader_group_rgb7(uint8_t group_color_idx, uint8_t *r7, uint8_t *g7, uint8_t *b7) {
    static const uint8_t palette[6][3] = {
        {RGB7_PERIWINKLE_R, RGB7_PERIWINKLE_G, RGB7_PERIWINKLE_B},
        {RGB7_TEAL_R, RGB7_TEAL_G, RGB7_TEAL_B},
        {RGB7_AMBER_R, RGB7_AMBER_G, RGB7_AMBER_B},
        {RGB7_GOLD_R, RGB7_GOLD_G, RGB7_GOLD_B},
        {RGB7_CORAL_R, RGB7_CORAL_G, RGB7_CORAL_B},
        {RGB7_MAGENTA_R, RGB7_MAGENTA_G, RGB7_MAGENTA_B},
    };
    uint8_t idx = (uint8_t)(group_color_idx % 6u);
    *r7 = palette[idx][0];
    *g7 = palette[idx][1];
    *b7 = palette[idx][2];
}

static tiles_cell_state_t render_mixer_cell(
    const tiles_logic_ctx_t *ctx,
    uint8_t col_0_4,
    uint8_t row_0_3
) {
    tiles_cell_state_t out = {0};
    uint8_t track = (uint8_t)(ctx->track_offset + col_0_4);
    if (track >= TILES_MAX_TRACKS) return out;

    const tiles_strip_state_t *s = &ctx->strips[track];
    uint8_t fg_r = RGB7_PERIWINKLE_R;
    uint8_t fg_g = RGB7_PERIWINKLE_G;
    uint8_t fg_b = RGB7_PERIWINKLE_B;
    uint8_t effective_color = s->has_custom_color ? s->color_code : ctx->fader_group_color;
    get_fader_group_rgb7(effective_color, &fg_r, &fg_g, &fg_b);
    out.has_clip = true;

    if (row_0_3 == 0u) {
        out.is_playing = s->muted;
        out.r7 = s->muted ? RGB7_AMBER_R : (uint8_t)(fg_r / 4u);
        out.g7 = s->muted ? RGB7_AMBER_G : (uint8_t)(fg_g / 4u);
        out.b7 = s->muted ? RGB7_AMBER_B : (uint8_t)(fg_b / 4u);
    } else if (row_0_3 == 1u) {
        out.is_playing = s->soloed;
        out.r7 = s->soloed ? RGB7_GOLD_R : (uint8_t)(fg_r / 4u);
        out.g7 = s->soloed ? RGB7_GOLD_G : (uint8_t)(fg_g / 4u);
        out.b7 = s->soloed ? RGB7_GOLD_B : (uint8_t)(fg_b / 4u);
    } else if (row_0_3 == 2u) {
        out.is_playing = s->rec_armed;
        out.r7 = s->rec_armed ? RGB7_CORAL_R : (uint8_t)(fg_r / 4u);
        out.g7 = s->rec_armed ? RGB7_CORAL_G : (uint8_t)(fg_g / 4u);
        out.b7 = s->rec_armed ? RGB7_CORAL_B : (uint8_t)(fg_b / 4u);
    } else {
        out.is_playing = s->selected;
        out.r7 = s->selected ? RGB7_MAGENTA_R : fg_r;
        out.g7 = s->selected ? RGB7_MAGENTA_G : fg_g;
        out.b7 = s->selected ? RGB7_MAGENTA_B : fg_b;
    }
    return out;
}

static void render_target_state(
    const tiles_logic_ctx_t *ctx,
    tiles_cell_state_t out_cells[TILES_VISIBLE_TRACKS][TILES_NUM_ROWS],
    tiles_scene_state_t out_scenes[TILES_NUM_ROWS]
) {
    for (uint8_t col = 0u; col < TILES_VISIBLE_TRACKS; col++) {
        uint8_t track = (uint8_t)(ctx->track_offset + col);
        for (uint8_t row = 0u; row < TILES_NUM_ROWS; row++) {
            if (ctx->page == TILES_PAGE_LIVE_LOOPS) {
                out_cells[col][row] = (track < TILES_MAX_TRACKS)
                                          ? ctx->loop_cells[track][row]
                                          : (tiles_cell_state_t){0};
            } else if (ctx->page == TILES_PAGE_SMART_CONTROLS) {
                out_cells[col][row] = render_smart_controls_cell(ctx, col, row);
            } else {
                out_cells[col][row] = render_mixer_cell(ctx, col, row);
            }
        }
    }

    for (uint8_t row = 0u; row < TILES_NUM_ROWS; row++) {
        if (ctx->page == TILES_PAGE_LIVE_LOOPS) {
            out_scenes[row] = ctx->loop_scenes[row];
        } else {
            out_scenes[row] = (tiles_scene_state_t){
                .is_triggered = (row == 3u && ctx->page == TILES_PAGE_SMART_CONTROLS && ctx->expr_split_enabled) ||
                                (row == 3u && ctx->page == TILES_PAGE_MIXER && ctx->logic_cycle),
                .r7 = RGB7_MAGENTA_R,
                .g7 = RGB7_MAGENTA_G,
                .b7 = RGB7_MAGENTA_B,
            };
        }
    }
}

void tiles_logic_init(tiles_logic_ctx_t *ctx, uint8_t unit_number) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->unit_number = (unit_number >= 1u && unit_number <= 4u) ? unit_number : 1u;
    ctx->page = TILES_PAGE_LIVE_LOOPS;
    ctx->mcu_assign = TILES_MCU_ASSIGN_PLUGIN;
    ctx->expr_split_enabled = true;
    ctx->diamond_ignored_while_playing = true;
    for (uint8_t i = 0u; i < TILES_NUM_MACROS; i++) {
        ctx->macros[i].value = 64u;
        ctx->macros[i].ring_level = 6u;
    }
    for (uint8_t t = 0u; t < TILES_MAX_TRACKS; t++) {
        ctx->strips[t].fader_msb = 100u;
    }
    ctx->strips[0].selected = true;
}

void tiles_logic_repaint(
    tiles_logic_ctx_t *ctx,
    bool force_full,
    tiles_midi_emit_fn emit,
    void *user_data
) {
    if (!emit) return;

    tiles_cell_state_t target_cells[TILES_VISIBLE_TRACKS][TILES_NUM_ROWS];
    tiles_scene_state_t target_scenes[TILES_NUM_ROWS];
    render_target_state(ctx, target_cells, target_scenes);

    bool send_all = force_full || !ctx->pad_cache_valid;
    for (uint8_t col = 0u; col < TILES_VISIBLE_TRACKS; col++) {
        uint8_t track = (uint8_t)(ctx->track_offset + col);
        if (track >= TILES_MAX_TRACKS) continue;
        for (uint8_t row = 0u; row < TILES_NUM_ROWS; row++) {
            if (send_all || !cell_equal(&ctx->sent_pads[col][row], &target_cells[col][row])) {
                emit_sysex_clip(emit, user_data, track, row, &target_cells[col][row]);
                ctx->sent_pads[col][row] = target_cells[col][row];
            }
        }
    }
    for (uint8_t row = 0u; row < TILES_NUM_ROWS; row++) {
        if (send_all || !scene_equal(&ctx->sent_scenes[row], &target_scenes[row])) {
            emit_sysex_scene(emit, user_data, row, &target_scenes[row]);
            ctx->sent_scenes[row] = target_scenes[row];
        }
    }
    ctx->pad_cache_valid = true;
}

void tiles_logic_set_page(
    tiles_logic_ctx_t *ctx,
    tiles_logic_page_t page,
    tiles_midi_emit_fn emit,
    void *user_data
) {
    if (page >= TILES_PAGE_COUNT) return;
    if (ctx->page != page) {
        ctx->page = page;
        ctx->pad_cache_valid = false;
    }
    tiles_logic_repaint(ctx, true, emit, user_data);
}

static void sync_mcu_bank_to_index(
    tiles_logic_ctx_t *ctx,
    uint8_t target_bank,
    tiles_midi_emit_fn emit,
    void *user_data
) {
    while (ctx->mcu_bank_offset < target_bank) {
        emit_mcu_button(emit, user_data, MCU_NOTE_BANK_RIGHT);
        ctx->mcu_bank_offset++;
    }
    while (ctx->mcu_bank_offset > target_bank) {
        emit_mcu_button(emit, user_data, MCU_NOTE_BANK_LEFT);
        ctx->mcu_bank_offset--;
    }
}

static void sync_mcu_bank_to_track_offset(
    tiles_logic_ctx_t *ctx,
    uint8_t new_offset,
    tiles_midi_emit_fn emit,
    void *user_data
) {
    if (new_offset > TILES_MAX_TRACKS - TILES_VISIBLE_TRACKS) {
        new_offset = TILES_MAX_TRACKS - TILES_VISIBLE_TRACKS;
    }
    ctx->track_offset = new_offset;

    /* MCU operates in 8-channel banks; step banks to keep focus aligned. */
    sync_mcu_bank_to_index(ctx, (uint8_t)(new_offset / 8u), emit, user_data);

    ctx->pad_cache_valid = false;
    tiles_logic_repaint(ctx, true, emit, user_data);
}

static void handle_pad_smart_controls(
    tiles_logic_ctx_t *ctx,
    uint8_t col_1_6,
    uint8_t row_0_3,
    tiles_midi_emit_fn emit,
    void *user_data
) {
    if (col_1_6 == 6u) {
        if (row_0_3 == 0u) {
            tiles_logic_set_page(ctx, TILES_PAGE_MIXER, emit, user_data);
        } else if (row_0_3 == 1u) {
            if (ctx->macro_bank > 0u) ctx->macro_bank--;
            emit_mcu_button(emit, user_data, MCU_NOTE_CH_LEFT);
        } else if (row_0_3 == 2u) {
            ctx->macro_bank++;
            emit_mcu_button(emit, user_data, MCU_NOTE_CH_RIGHT);
        } else if (row_0_3 == 3u) {
            ctx->expr_split_enabled = !ctx->expr_split_enabled;
            tiles_logic_repaint(ctx, false, emit, user_data);
        }
        return;
    }

    if (col_1_6 == 5u) {
        static const uint8_t assign_notes[4] = {
            MCU_NOTE_ASSIGN_PLUGIN,
            MCU_NOTE_ASSIGN_PAN,
            MCU_NOTE_ASSIGN_SEND,
            MCU_NOTE_ASSIGN_TRACK,
        };
        static const tiles_mcu_assign_t assign_modes[4] = {
            TILES_MCU_ASSIGN_PLUGIN,
            TILES_MCU_ASSIGN_PAN,
            TILES_MCU_ASSIGN_SEND,
            TILES_MCU_ASSIGN_TRACK,
        };
        ctx->mcu_assign = assign_modes[row_0_3];
        emit_mcu_button(emit, user_data, assign_notes[row_0_3]);
        tiles_logic_repaint(ctx, false, emit, user_data);
        return;
    }

    /* Cols 1..4: top 2 rows step MCU V-Pot +6, bottom 2 rows step -6. */
    uint8_t col0 = (uint8_t)(col_1_6 - 1u);
    uint8_t macro_idx = (row_0_3 < 2u)
                            ? (uint8_t)(row_0_3 * 4u + col0)
                            : (uint8_t)((row_0_3 - 2u) * 4u + col0);
    bool inc = (row_0_3 < 2u);
    uint8_t vpot_delta = inc ? 0x06u : 0x46u;
    emit_cc(emit, user_data, TILES_OUT_LOGIC_MCU, 0u, (uint8_t)(0x10u + macro_idx), vpot_delta);

    int16_t next = (int16_t)ctx->macros[macro_idx].value + (inc ? 16 : -16);
    if (next < 0) next = 0;
    if (next > 127) next = 127;
    ctx->macros[macro_idx].value = (uint8_t)next;
    ctx->macros[macro_idx].ring_level = (uint8_t)((next * 11) / 127);

    /* Also emit absolute assignable CC on Logic Grid for Cmd+L Smart Control Learn. */
    emit_cc(emit, user_data, TILES_OUT_LOGIC_GRID, TILES_GRID_OUT_CHANNEL, (uint8_t)(102u + macro_idx),
            ctx->macros[macro_idx].value);
    tiles_logic_repaint(ctx, false, emit, user_data);
}

static void handle_pad_mixer(
    tiles_logic_ctx_t *ctx,
    uint8_t col_1_6,
    uint8_t row_0_3,
    tiles_midi_emit_fn emit,
    void *user_data
) {
    if (col_1_6 == 6u) {
        if (row_0_3 == 0u) {
            tiles_logic_set_page(ctx, TILES_PAGE_LIVE_LOOPS, emit, user_data);
        } else if (row_0_3 == 1u) {
            emit_mcu_button(emit, user_data, MCU_NOTE_CH_LEFT);
            if (ctx->track_offset > 0u) {
                sync_mcu_bank_to_track_offset(ctx, (uint8_t)(ctx->track_offset - 1u), emit, user_data);
            }
        } else if (row_0_3 == 2u) {
            emit_mcu_button(emit, user_data, MCU_NOTE_CH_RIGHT);
            if (ctx->track_offset < TILES_MAX_TRACKS - TILES_VISIBLE_TRACKS) {
                sync_mcu_bank_to_track_offset(ctx, (uint8_t)(ctx->track_offset + 1u), emit, user_data);
            }
        } else if (row_0_3 == 3u) {
            ctx->logic_cycle = !ctx->logic_cycle;
            emit_mcu_button(emit, user_data, MCU_NOTE_CYCLE);
            tiles_logic_repaint(ctx, false, emit, user_data);
        }
        return;
    }

    uint8_t col0 = (uint8_t)(col_1_6 - 1u);
    uint8_t abs_track = (uint8_t)(ctx->track_offset + col0);
    if (abs_track >= TILES_MAX_TRACKS) return;

    sync_mcu_bank_to_index(ctx, (uint8_t)(abs_track / 8u), emit, user_data);
    uint8_t mcu_ch = (uint8_t)(abs_track % 8u);
    tiles_strip_state_t *strip = &ctx->strips[abs_track];

    if (row_0_3 == 0u) {
        strip->muted = !strip->muted;
        emit_mcu_button(emit, user_data, (uint8_t)(MCU_NOTE_MUTE_BASE + mcu_ch));
    } else if (row_0_3 == 1u) {
        strip->soloed = !strip->soloed;
        emit_mcu_button(emit, user_data, (uint8_t)(MCU_NOTE_SOLO_BASE + mcu_ch));
    } else if (row_0_3 == 2u) {
        strip->rec_armed = !strip->rec_armed;
        emit_mcu_button(emit, user_data, (uint8_t)(MCU_NOTE_REC_ARM_BASE + mcu_ch));
    } else {
        for (uint8_t t = 0u; t < TILES_MAX_TRACKS; t++) {
            ctx->strips[t].selected = (t == abs_track);
        }
        emit_mcu_button(emit, user_data, (uint8_t)(MCU_NOTE_SELECT_BASE + mcu_ch));
    }
    tiles_logic_repaint(ctx, false, emit, user_data);
}

static void handle_pad_live_loops(
    tiles_logic_ctx_t *ctx,
    uint8_t cc,
    uint8_t pad_1_24,
    tiles_midi_emit_fn emit,
    void *user_data
) {
    uint8_t col = (uint8_t)(((pad_1_24 - 1u) % TILES_NUM_COLS) + 1u);
    uint8_t row = (uint8_t)((pad_1_24 - 1u) / TILES_NUM_COLS);
    uint8_t track = (uint8_t)(ctx->track_offset + (col - 1u));

    if (col == 6u) {
        if (cc == TILES_CC_GRID_TOUCH) {
            emit_cc(emit, user_data, TILES_OUT_LOGIC_GRID, TILES_GRID_OUT_CHANNEL,
                    TILES_GRID_CC_SCENE_FIRE, (uint8_t)(row + 1u));
            ctx->loop_scenes[row].is_triggered = true;
            tiles_logic_repaint(ctx, false, emit, user_data);
        }
        return;
    }

    if (track >= TILES_MAX_TRACKS) return;

    if (cc == TILES_CC_GRID_TOUCH) {
        emit_cc(emit, user_data, TILES_OUT_LOGIC_GRID, TILES_GRID_OUT_CHANNEL,
                TILES_GRID_CC_CELL_FIRE, pad_1_24);
        /* Also emit discrete note per visible cell for Logic Live Loops Learn. */
        emit_note(emit, user_data, TILES_OUT_LOGIC_GRID, 0u, (uint8_t)(36u + pad_1_24 - 1u), 127u);
        emit_note(emit, user_data, TILES_OUT_LOGIC_GRID, 0u, (uint8_t)(36u + pad_1_24 - 1u), 0u);
    } else if (cc == TILES_CC_STOP_TOUCH) {
        emit_cc(emit, user_data, TILES_OUT_LOGIC_GRID, TILES_GRID_OUT_CHANNEL,
                TILES_GRID_CC_CELL_STOP, pad_1_24);
        ctx->loop_cells[track][row].is_playing = false;
        ctx->loop_cells[track][row].is_triggered = false;
        tiles_logic_repaint(ctx, false, emit, user_data);
    } else if (cc == TILES_CC_RECORD_TOUCH) {
        /* Exclusive-arm target track via MCU, fire record trigger, and open melodic mode. */
        for (uint8_t t = 0u; t < TILES_MAX_TRACKS; t++) {
            if (t != track && ctx->strips[t].rec_armed) {
                ctx->strips[t].rec_armed = false;
                sync_mcu_bank_to_index(ctx, (uint8_t)(t / 8u), emit, user_data);
                emit_mcu_button(emit, user_data, (uint8_t)(MCU_NOTE_REC_ARM_BASE + (t % 8u)));
            }
        }
        sync_mcu_bank_to_index(ctx, (uint8_t)(track / 8u), emit, user_data);
        if (!ctx->strips[track].rec_armed) {
            ctx->strips[track].rec_armed = true;
            emit_mcu_button(emit, user_data, (uint8_t)(MCU_NOTE_REC_ARM_BASE + (track % 8u)));
        }
        emit_cc(emit, user_data, TILES_OUT_LOGIC_GRID, TILES_GRID_OUT_CHANNEL,
                TILES_GRID_CC_CELL_RECORD, pad_1_24);
        ctx->capture_active = true;
        ctx->capture_pad = pad_1_24;
        emit_sysex_open_melodic(emit, user_data);
    } else if (cc == TILES_CC_DELETE_TOUCH) {
        emit_cc(emit, user_data, TILES_OUT_LOGIC_GRID, TILES_GRID_OUT_CHANNEL,
                TILES_GRID_CC_CELL_DELETE, pad_1_24);
        ctx->loop_cells[track][row] = (tiles_cell_state_t){0};
        tiles_logic_repaint(ctx, false, emit, user_data);
    }
}

void tiles_logic_on_tiles_daw_midi(
    tiles_logic_ctx_t *ctx,
    const uint8_t *bytes,
    size_t len,
    uint64_t now_ms,
    tiles_midi_emit_fn emit,
    void *user_data
) {
    if (!ctx || !bytes || len < 3u || !emit) return;

    uint8_t status = bytes[0];
    if ((status & 0xF0u) != 0xB0u || (status & 0x0Fu) != TILES_MASTER_CHANNEL) return;

    uint8_t cc = (uint8_t)(bytes[1] & 0x7Fu);
    uint8_t val = (uint8_t)(bytes[2] & 0x7Fu);

    if (cc == TILES_CC_PLAY && val > 0u) {
        if (!ctx->logic_playing) {
            emit_mcu_button(emit, user_data, MCU_NOTE_PLAY);
            ctx->logic_playing = true;
        }
        return;
    }

    if (cc == TILES_CC_STOP && val > 0u) {
        if (ctx->logic_recording) {
            emit_mcu_button(emit, user_data, MCU_NOTE_RECORD);
            ctx->logic_recording = false;
        }
        emit_mcu_button(emit, user_data, MCU_NOTE_STOP);
        ctx->logic_playing = false;
        return;
    }

    if (cc == TILES_CC_RECORD && val > 0u) {
        if (ctx->diamond_ignored_while_playing && ctx->logic_playing) return;
        emit_mcu_button(emit, user_data, MCU_NOTE_RECORD);
        ctx->logic_recording = true;
        ctx->logic_playing = true;
        return;
    }

    if (cc == TILES_CC_TRACK_OFFSET) {
        sync_mcu_bank_to_track_offset(ctx, val, emit, user_data);
        return;
    }

    if (cc == TILES_CC_MASTER_STOP && val > 0u) {
        if (ctx->last_master_stop_ms > 0u && (now_ms - ctx->last_master_stop_ms) <= PAGE_DOUBLE_TAP_MS) {
            ctx->last_master_stop_ms = 0u;
            tiles_logic_page_t next = (tiles_logic_page_t)((ctx->page + 1u) % TILES_PAGE_COUNT);
            tiles_logic_set_page(ctx, next, emit, user_data);
            return;
        }
        ctx->last_master_stop_ms = now_ms;
        emit_cc(emit, user_data, TILES_OUT_LOGIC_GRID, TILES_GRID_OUT_CHANNEL, TILES_GRID_CC_MASTER_STOP, 127u);
        emit_cc(emit, user_data, TILES_OUT_LOGIC_GRID, TILES_GRID_OUT_CHANNEL, TILES_GRID_CC_MASTER_STOP, 0u);
        return;
    }

    if (cc == TILES_CC_END_CAPTURE && val > 0u) {
        if (ctx->capture_active) {
            emit_cc(emit, user_data, TILES_OUT_LOGIC_GRID, TILES_GRID_OUT_CHANNEL,
                    TILES_GRID_CC_END_CAPTURE, ctx->capture_pad);
            ctx->capture_active = false;
            ctx->capture_pad = 0u;
        }
        return;
    }

    if (cc >= TILES_CC_GRID_TOUCH && cc <= TILES_CC_RECORD_TOUCH) {
        if (val < 1u || val > TILES_NUM_PADS) return;

        if (ctx->page == TILES_PAGE_LIVE_LOOPS) {
            handle_pad_live_loops(ctx, cc, val, emit, user_data);
            return;
        }

        if (cc == TILES_CC_GRID_TOUCH || cc == TILES_CC_STOP_TOUCH) {
            uint8_t col = (uint8_t)(((val - 1u) % TILES_NUM_COLS) + 1u);
            uint8_t row = (uint8_t)((val - 1u) / TILES_NUM_COLS);
            if (ctx->page == TILES_PAGE_SMART_CONTROLS) {
                handle_pad_smart_controls(ctx, col, row, emit, user_data);
            } else {
                handle_pad_mixer(ctx, col, row, emit, user_data);
            }
        }
    }
}

void tiles_logic_on_tiles_mpe_midi(
    tiles_logic_ctx_t *ctx,
    const uint8_t *bytes,
    size_t len,
    tiles_midi_emit_fn emit,
    void *user_data
) {
    if (!ctx || !ctx->expr_split_enabled || !bytes || len < 2u || !emit) return;

    uint8_t status = bytes[0];
    uint8_t type = (uint8_t)(status & 0xF0u);
    uint8_t ch = (uint8_t)(status & 0x0Fu);
    if (ch < TILES_MPE_FIRST_MEMBER_CH || ch > TILES_MPE_LAST_MEMBER_CH) return;

    tiles_voice_expr_t *v = &ctx->voices[ch];

    if (type == 0x90u && len >= 3u) {
        uint8_t vel = (uint8_t)(bytes[2] & 0x7Fu);
        if (vel > 0u) {
            v->active = true;
            v->note = (uint8_t)(bytes[1] & 0x7Fu);
            v->strike_vel = vel;
            ctx->focused_voice_ch = ch;
            emit_cc(emit, user_data, TILES_OUT_LOGIC_GRID, ch, TILES_EXPR_VOICE_CC_TOUCH, 127u);
            emit_cc(emit, user_data, TILES_OUT_LOGIC_GRID, ch, TILES_EXPR_VOICE_CC_STRIKE, vel);
            emit_cc(emit, user_data, TILES_OUT_LOGIC_GRID, TILES_EXPR_FOCUS_CHANNEL, TILES_EXPR_CC_TOUCH, 127u);
            emit_cc(emit, user_data, TILES_OUT_LOGIC_GRID, TILES_EXPR_FOCUS_CHANNEL, TILES_EXPR_CC_STRIKE, vel);
        } else {
            v->active = false;
            emit_cc(emit, user_data, TILES_OUT_LOGIC_GRID, ch, TILES_EXPR_VOICE_CC_TOUCH, 0u);
            if (ctx->focused_voice_ch == ch) {
                emit_cc(emit, user_data, TILES_OUT_LOGIC_GRID, TILES_EXPR_FOCUS_CHANNEL, TILES_EXPR_CC_TOUCH, 0u);
            }
        }
    } else if (type == 0x80u && len >= 3u) {
        v->active = false;
        emit_cc(emit, user_data, TILES_OUT_LOGIC_GRID, ch, TILES_EXPR_VOICE_CC_TOUCH, 0u);
        if (ctx->focused_voice_ch == ch) {
            emit_cc(emit, user_data, TILES_OUT_LOGIC_GRID, TILES_EXPR_FOCUS_CHANNEL, TILES_EXPR_CC_TOUCH, 0u);
        }
    } else if (type == 0xD0u && len >= 2u) {
        uint8_t press = (uint8_t)(bytes[1] & 0x7Fu);
        v->press = press;
        emit_cc(emit, user_data, TILES_OUT_LOGIC_GRID, ch, TILES_EXPR_VOICE_CC_PRESS, press);
        if (ctx->focused_voice_ch == ch) {
            emit_cc(emit, user_data, TILES_OUT_LOGIC_GRID, TILES_EXPR_FOCUS_CHANNEL, TILES_EXPR_CC_PRESS, press);
        }
    } else if (type == 0xE0u && len >= 3u) {
        /* Use 14-bit pitch bend MSB directly for zero-overhead 0..127 tilt CC. */
        uint8_t tilt = (uint8_t)(bytes[2] & 0x7Fu);
        v->tilt_cc = tilt;
        emit_cc(emit, user_data, TILES_OUT_LOGIC_GRID, ch, TILES_EXPR_VOICE_CC_TILT, tilt);
        if (ctx->focused_voice_ch == ch) {
            emit_cc(emit, user_data, TILES_OUT_LOGIC_GRID, TILES_EXPR_FOCUS_CHANNEL, TILES_EXPR_CC_TILT, tilt);
        }
    }
}

static void handle_mcu_sysex(
    tiles_logic_ctx_t *ctx,
    const uint8_t *bytes,
    size_t len,
    tiles_midi_emit_fn emit,
    void *user_data
) {
    /* Universal Device Inquiry: F0 7E <id> 06 01 F7 -> Identify as Mackie Control. */
    if (len >= 6u && bytes[1] == 0x7Eu && bytes[3] == 0x06u && bytes[4] == 0x01u) {
        static const uint8_t id_reply[17] = {
            0xF0u, 0x7Eu, 0x00u, 0x06u, 0x02u,
            0x00u, 0x00u, 0x66u, 0x14u, 0x00u,
            0x01u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u, 0xF7u,
        };
        emit(TILES_OUT_LOGIC_MCU, id_reply, sizeof(id_reply), user_data);
        return;
    }

    /* Mackie Control Host Connection Query / Challenge (SysEx 00 00 66 14 ...). */
    if (len >= 7u && bytes[1] == 0x00u && bytes[2] == 0x00u && bytes[3] == 0x66u && bytes[4] == 0x14u) {
        uint8_t cmd = bytes[5];
        uint8_t serial[7] = {'T', 'I', 'L', 'E', 'S', '0', (uint8_t)('0' + ctx->unit_number)};
        if (cmd == 0x00u) {
            /* Reply Host Connection Query with Model 0x14 + 7-byte serial + 4-byte challenge. */
            uint8_t reply[18] = {
                0xF0u, 0x00u, 0x00u, 0x66u, 0x14u, 0x01u,
                serial[0], serial[1], serial[2], serial[3], serial[4], serial[5], serial[6],
                0x11u, 0x22u, 0x33u, 0x44u, 0xF7u,
            };
            emit(TILES_OUT_LOGIC_MCU, reply, sizeof(reply), user_data);
        } else if (cmd == 0x02u && len >= 17u) {
            /* Standard Mackie challenge response confirmation (0x03). */
            const uint8_t *c = &bytes[13];
            uint8_t resp[4] = {
                (uint8_t)(0x7Fu & (c[0] + (c[1] ^ 0x0Au) - c[3])),
                (uint8_t)(0x7Fu & ((c[2] >> 4) ^ (c[0] + c[3]))),
                (uint8_t)(0x7Fu & ((c[3] - (c[2] << 2)) ^ (c[0] | c[1]))),
                (uint8_t)(0x7Fu & (c[1] - c[2] + (0xF0u ^ (c[3] << 4)))),
            };
            uint8_t confirm[18] = {
                0xF0u, 0x00u, 0x00u, 0x66u, 0x14u, 0x03u,
                serial[0], serial[1], serial[2], serial[3], serial[4], serial[5], serial[6],
                resp[0], resp[1], resp[2], resp[3], 0xF7u,
            };
            emit(TILES_OUT_LOGIC_MCU, confirm, sizeof(confirm), user_data);
        }
    }
}

void tiles_logic_on_mcu_midi(
    tiles_logic_ctx_t *ctx,
    const uint8_t *bytes,
    size_t len,
    tiles_midi_emit_fn emit,
    void *user_data
) {
    if (!ctx || !bytes || len == 0u || !emit) return;

    if (bytes[0] == 0xF0u) {
        handle_mcu_sysex(ctx, bytes, len, emit, user_data);
        return;
    }
    if (len < 3u) return;

    uint8_t status = bytes[0];
    uint8_t type = (uint8_t)(status & 0xF0u);
    uint8_t d1 = (uint8_t)(bytes[1] & 0x7Fu);
    uint8_t d2 = (uint8_t)(bytes[2] & 0x7Fu);

    if (type == 0x90u) {
        bool lit = (d2 > 0u);
        if (d1 == MCU_NOTE_PLAY) ctx->logic_playing = lit;
        else if (d1 == MCU_NOTE_RECORD) ctx->logic_recording = lit;
        else if (d1 == MCU_NOTE_CYCLE) ctx->logic_cycle = lit;
        else if (d1 == MCU_NOTE_ASSIGN_TRACK && lit) ctx->mcu_assign = TILES_MCU_ASSIGN_TRACK;
        else if (d1 == MCU_NOTE_ASSIGN_SEND && lit) ctx->mcu_assign = TILES_MCU_ASSIGN_SEND;
        else if (d1 == MCU_NOTE_ASSIGN_PAN && lit) ctx->mcu_assign = TILES_MCU_ASSIGN_PAN;
        else if (d1 == MCU_NOTE_ASSIGN_PLUGIN && lit) ctx->mcu_assign = TILES_MCU_ASSIGN_PLUGIN;
        else if (d1 < 32u) {
            uint8_t bank_start = (uint8_t)(ctx->mcu_bank_offset * 8u);
            uint8_t abs_track = (uint8_t)(bank_start + (d1 & 0x07u));
            if (abs_track < TILES_MAX_TRACKS) {
                uint8_t group = (uint8_t)(d1 >> 3);
                if (group == 0u) ctx->strips[abs_track].rec_armed = lit;
                else if (group == 1u) ctx->strips[abs_track].soloed = lit;
                else if (group == 2u) ctx->strips[abs_track].muted = lit;
                else if (group == 3u) ctx->strips[abs_track].selected = lit;
            }
        }
        tiles_logic_repaint(ctx, false, emit, user_data);
    } else if (type == 0xB0u && d1 >= 0x30u && d1 <= 0x37u) {
        /* MCU V-Pot LED ring update (0x30..0x37): bits 0..3 hold ring value 0..11. */
        uint8_t idx = (uint8_t)(d1 - 0x30u);
        uint8_t ring = (uint8_t)(d2 & 0x0Fu);
        if (ring > 11u) ring = 11u;
        ctx->macros[idx].ring_level = ring;
        ctx->macros[idx].value = (uint8_t)((ring * 127u) / 11u);
        if (ctx->page == TILES_PAGE_SMART_CONTROLS) {
            tiles_logic_repaint(ctx, false, emit, user_data);
        }
    }
}

void tiles_logic_on_grid_midi(
    tiles_logic_ctx_t *ctx,
    const uint8_t *bytes,
    size_t len,
    tiles_midi_emit_fn emit,
    void *user_data
) {
    if (!ctx || !bytes || len < 3u || !emit) return;

    /* Forward native TILES SysEx directly when supplied by external tools/scripts. */
    if (bytes[0] == 0xF0u && len >= 5u &&
        bytes[1] == TILES_SYSEX_MFR_ID && bytes[2] == TILES_SYSEX_SUB_ID) {
        if (bytes[3] == TILES_MSG_CLIP_STATE && len >= 11u) {
            uint8_t track = bytes[4];
            uint8_t scene = bytes[5];
            if (track < TILES_MAX_TRACKS && scene < TILES_NUM_ROWS) {
                uint8_t flags = bytes[6];
                ctx->loop_cells[track][scene] = (tiles_cell_state_t){
                    .has_clip = (flags & TILES_FLAG_HAS_CLIP) != 0u,
                    .is_playing = (flags & TILES_FLAG_IS_PLAYING) != 0u,
                    .is_triggered = (flags & TILES_FLAG_IS_TRIGGERED) != 0u,
                    .r7 = (uint8_t)(bytes[7] & 0x7Fu),
                    .g7 = (uint8_t)(bytes[8] & 0x7Fu),
                    .b7 = (uint8_t)(bytes[9] & 0x7Fu),
                };
                tiles_logic_repaint(ctx, false, emit, user_data);
            }
        } else if (bytes[3] == TILES_MSG_SCENE_STATE && len >= 10u) {
            uint8_t scene = bytes[4];
            if (scene < TILES_NUM_ROWS) {
                uint8_t flags = bytes[5];
                ctx->loop_scenes[scene] = (tiles_scene_state_t){
                    .is_triggered = (flags & TILES_FLAG_IS_TRIGGERED) != 0u,
                    .r7 = (uint8_t)(bytes[6] & 0x7Fu),
                    .g7 = (uint8_t)(bytes[7] & 0x7Fu),
                    .b7 = (uint8_t)(bytes[8] & 0x7Fu),
                };
                tiles_logic_repaint(ctx, false, emit, user_data);
            }
        } else if (bytes[3] == TILES_MSG_OPEN_MELODIC) {
            emit_sysex_open_melodic(emit, user_data);
        }
        return;
    }

    uint8_t status = bytes[0];
    if ((status & 0xF0u) == 0xB0u) {
        uint8_t ch = (uint8_t)(status & 0x0Fu);
        uint8_t cc = (uint8_t)(bytes[1] & 0x7Fu);
        uint8_t val = (uint8_t)(bytes[2] & 0x7Fu);
        if (ch == TILES_GRID_OUT_CHANNEL &&
            cc >= TILES_GRID_CC_FADER_COLOR_BASE &&
            cc < (TILES_GRID_CC_FADER_COLOR_BASE + TILES_VISIBLE_TRACKS)) {
            uint8_t col = (uint8_t)(cc - TILES_GRID_CC_FADER_COLOR_BASE);
            uint8_t track = (uint8_t)(ctx->track_offset + col);
            if (track < TILES_MAX_TRACKS) {
                uint8_t code = (val <= 31u) ? 0u
                             : (val <= 52u) ? 1u
                             : (val <= 73u) ? 2u
                             : (val <= 95u) ? 3u
                             : (val <= 116u) ? 4u
                             : 5u;
                ctx->strips[track].has_custom_color = true;
                ctx->strips[track].color_code = code;
                tiles_logic_repaint(ctx, false, emit, user_data);
            }
            return;
        }
        if (cc == TILES_LOGIC_CC_PAGE_SELECT) {
            tiles_logic_page_t target = (val == 127u)
                                            ? (tiles_logic_page_t)((ctx->page + 1u) % TILES_PAGE_COUNT)
                                            : (tiles_logic_page_t)(val % TILES_PAGE_COUNT);
            tiles_logic_set_page(ctx, target, emit, user_data);
        } else if (cc == TILES_LOGIC_CC_EXPR_SPLIT) {
            ctx->expr_split_enabled = (val > 0u);
            tiles_logic_repaint(ctx, false, emit, user_data);
        } else if (cc == TILES_LOGIC_CC_FADER_COLOR) {
            ctx->fader_group_color = (uint8_t)(val % 6u);
            ctx->pad_cache_valid = false;
            tiles_logic_repaint(ctx, true, emit, user_data);
        } else if (cc == TILES_LOGIC_CC_MIXER_BANK) {
            uint16_t raw_off = (uint16_t)val * TILES_VISIBLE_TRACKS;
            uint8_t clamped_off = (raw_off > TILES_MAX_TRACKS - TILES_VISIBLE_TRACKS)
                                      ? (uint8_t)(TILES_MAX_TRACKS - TILES_VISIBLE_TRACKS)
                                      : (uint8_t)raw_off;
            ctx->fader_group_color = (uint8_t)(val % 6u);
            sync_mcu_bank_to_track_offset(ctx, clamped_off, emit, user_data);
        } else if (cc == TILES_LOGIC_CC_BANK_STEP) {
            int16_t step = (val < 64u) ? -(int16_t)TILES_VISIBLE_TRACKS : (int16_t)TILES_VISIBLE_TRACKS;
            int16_t next_off = (int16_t)ctx->track_offset + step;
            if (next_off < 0) next_off = 0;
            if (next_off > (int16_t)(TILES_MAX_TRACKS - TILES_VISIBLE_TRACKS)) {
                next_off = (int16_t)(TILES_MAX_TRACKS - TILES_VISIBLE_TRACKS);
            }
            ctx->fader_group_color = (uint8_t)(((uint8_t)next_off / TILES_VISIBLE_TRACKS) % 6u);
            sync_mcu_bank_to_track_offset(ctx, (uint8_t)next_off, emit, user_data);
        } else if (cc == TILES_CC_TRACK_OFFSET) {
            ctx->fader_group_color = (uint8_t)((val / TILES_VISIBLE_TRACKS) % 6u);
            sync_mcu_bank_to_track_offset(ctx, val, emit, user_data);
        }
    } else if ((status & 0xF0u) == 0x90u && ctx->page == TILES_PAGE_LIVE_LOOPS) {
        /* Note feedback on Grid port updates visible cell LED + haptic state. */
        uint8_t note = (uint8_t)(bytes[1] & 0x7Fu);
        uint8_t vel = (uint8_t)(bytes[2] & 0x7Fu);
        if (note >= 36u && note < 36u + 20u) {
            uint8_t pad0 = (uint8_t)(note - 36u);
            uint8_t col = (uint8_t)(pad0 % TILES_VISIBLE_TRACKS);
            uint8_t row = (uint8_t)(pad0 / TILES_VISIBLE_TRACKS);
            uint8_t track = (uint8_t)(ctx->track_offset + col);
            if (track < TILES_MAX_TRACKS) {
                tiles_cell_state_t *c = &ctx->loop_cells[track][row];
                c->has_clip = (vel > 0u);
                c->is_playing = (vel >= 64u);
                c->is_triggered = (vel > 0u && vel < 64u);
                if (vel > 0u && c->r7 == 0u && c->g7 == 0u && c->b7 == 0u) {
                    c->r7 = RGB7_TEAL_R;
                    c->g7 = RGB7_TEAL_G;
                    c->b7 = RGB7_TEAL_B;
                }
                tiles_logic_repaint(ctx, false, emit, user_data);
            }
        }
    }
}

void tiles_logic_on_display_midi(
    tiles_logic_ctx_t *ctx,
    const uint8_t *bytes,
    size_t len,
    tiles_midi_emit_fn emit,
    void *user_data
) {
    (void)ctx;
    if (!bytes || len < 3u || !emit) return;

    uint8_t status = bytes[0];
    uint8_t type = (uint8_t)(status & 0xF0u);
    uint8_t ch = (uint8_t)(status & 0x0Fu);

    /* Firmware lights DAW Ch 1 in green and Ch 2 in soft red; collapse higher channels to Ch 1. */
    uint8_t out_ch = (ch == 1u) ? 1u : 0u;

    if (type == 0x90u || type == 0x80u) {
        uint8_t out[3] = {
            (uint8_t)(type | out_ch),
            (uint8_t)(bytes[1] & 0x7Fu),
            (uint8_t)(bytes[2] & 0x7Fu),
        };
        emit(TILES_OUT_TILES_DAW, out, sizeof(out), user_data);
    } else if (type == 0xB0u && (bytes[1] == 120u || bytes[1] == 123u)) {
        emit_cc(emit, user_data, TILES_OUT_TILES_DAW, out_ch, bytes[1], 0u);
    }
}
