#include "StudioLink.hpp"

#include <algorithm>

namespace sentia::logic {

namespace {

constexpr uint8_t kMcuNotePlay = 0x5Eu;
constexpr uint8_t kDetentMin = 62u;
constexpr uint8_t kDetentMax = 66u;

uint8_t macroToPad1(uint8_t macro_idx) {
    /* Macros 0..3 sit on row 0 cols 1..4 (pads 1..4); macros 4..7 sit on row 1 cols 1..4 (pads 7..10). */
    const uint8_t row = (macro_idx < 4u) ? 0u : 1u;
    const uint8_t col = static_cast<uint8_t>(macro_idx % 4u);
    return static_cast<uint8_t>(row * TILES_NUM_COLS + col + 1u);
}

bool isMidpointDetent(uint8_t value) {
    return value >= kDetentMin && value <= kDetentMax;
}

} // namespace

StudioLink::StudioLink(uint8_t unit_number) {
    reset(unit_number);
}

void StudioLink::reset(uint8_t unit_number) {
    tiles_logic_init(&core_, unit_number);
    haptic_pulse_until_ms_.fill(0u);
    macro_detent_armed_.fill(true);
    clock_ticks_ = 0u;
    haptic_intensity_ = 100u;
    quantized_sync_enabled_ = true;
}

void StudioLink::onHardwareDawMidi(
    const uint8_t* bytes,
    size_t len,
    uint64_t now_ms,
    tiles_midi_emit_fn emit,
    void* user_data
) {
    if (!bytes || len == 0 || !emit) return;

    const bool prev_playing = core_.logic_playing;
    tiles_logic_on_tiles_daw_midi(&core_, bytes, len, now_ms, emit, user_data);

    /* Emit brief synchronized haptic confirmation when transport starts cleanly. */
    if (!prev_playing && core_.logic_playing && core_.page == TILES_PAGE_LIVE_LOOPS) {
        triggerPadHapticPulse(6u, now_ms, kDefaultHapticPulseMs, emit, user_data);
    }
}

void StudioLink::onHardwareMpeMidi(
    const uint8_t* bytes,
    size_t len,
    tiles_midi_emit_fn emit,
    void* user_data
) {
    tiles_logic_on_tiles_mpe_midi(&core_, bytes, len, emit, user_data);
}

void StudioLink::onLogicMcuMidi(
    const uint8_t* bytes,
    size_t len,
    uint64_t now_ms,
    tiles_midi_emit_fn emit,
    void* user_data
) {
    if (!bytes || len == 0 || !emit) return;

    const bool prev_playing = core_.logic_playing;
    tiles_logic_on_mcu_midi(&core_, bytes, len, emit, user_data);

    if (len >= 3u) {
        const uint8_t type = static_cast<uint8_t>(bytes[0] & 0xF0u);
        const uint8_t d1 = static_cast<uint8_t>(bytes[1] & 0x7Fu);
        const uint8_t d2 = static_cast<uint8_t>(bytes[2] & 0x7Fu);

        if (type == 0x90u && d1 == kMcuNotePlay && !prev_playing && core_.logic_playing) {
            triggerPadHapticPulse(6u, now_ms, kDefaultHapticPulseMs, emit, user_data);
        } else if (type == 0xB0u && d1 >= 0x30u && d1 <= 0x37u) {
            const uint8_t idx = static_cast<uint8_t>(d1 - 0x30u);
            const uint8_t ring = static_cast<uint8_t>(d2 & 0x0Fu);
            if (ring == 5u || ring == 6u) {
                if (macro_detent_armed_[idx]) {
                    macro_detent_armed_[idx] = false;
                    pulseMacroDetent(idx, now_ms, emit, user_data);
                }
            } else {
                macro_detent_armed_[idx] = true;
            }
        }
    }
}

void StudioLink::onLogicGridMidi(
    const uint8_t* bytes,
    size_t len,
    uint64_t now_ms,
    tiles_midi_emit_fn emit,
    void* user_data
) {
    if (!bytes || len == 0 || !emit) return;

    /* Realtime MIDI clock tick drives beat-aligned haptic sync when enabled. */
    if (bytes[0] == 0xF8u) {
        clock_ticks_++;
        if (quantized_sync_enabled_ && (clock_ticks_ % 24u) == 0u && core_.page == TILES_PAGE_LIVE_LOOPS) {
            for (uint8_t row = 0; row < TILES_NUM_ROWS; ++row) {
                if (core_.loop_scenes[row].is_triggered) {
                    const uint8_t scene_pad = static_cast<uint8_t>(row * TILES_NUM_COLS + TILES_NUM_COLS);
                    triggerPadHapticPulse(scene_pad, now_ms, 45u, emit, user_data);
                }
            }
        }
        return;
    }

    if (len >= 3u && (bytes[0] & 0xF0u) == 0xB0u) {
        const uint8_t cc = static_cast<uint8_t>(bytes[1] & 0x7Fu);
        const uint8_t val = static_cast<uint8_t>(bytes[2] & 0x7Fu);

        if (cc == kGridCcMixerBankStep) {
            /* Step visible 5-track window & 8-ch MCU bank (-5 left when < 64, +5 right when >= 64). */
            int16_t next_off = static_cast<int16_t>(core_.track_offset) + (val >= 64u ? 5 : -5);
            if (next_off < 0) next_off = 0;
            if (next_off > static_cast<int16_t>(TILES_MAX_TRACKS - TILES_VISIBLE_TRACKS)) {
                next_off = static_cast<int16_t>(TILES_MAX_TRACKS - TILES_VISIBLE_TRACKS);
            }
            const uint8_t sync_cc[3] = {0xB0u, TILES_CC_TRACK_OFFSET, static_cast<uint8_t>(next_off)};
            tiles_logic_on_tiles_daw_midi(&core_, sync_cc, sizeof(sync_cc), now_ms, emit, user_data);
            return;
        }

        if (cc == kGridCcHapticIntensity) {
            setHapticIntensity(val);
            if (val == 0u) {
                haptic_pulse_until_ms_.fill(0u);
                repaint(true, emit, user_data);
            }
            return;
        }

        if (cc == kGridCcHapticCue) {
            if (val >= 1u && val <= TILES_NUM_PADS) {
                triggerPadHapticPulse(val, now_ms, kDefaultHapticPulseMs, emit, user_data);
            } else if (val == 127u) {
                for (uint8_t p = 1u; p <= TILES_NUM_PADS; ++p) {
                    triggerPadHapticPulse(p, now_ms, kDefaultHapticPulseMs, emit, user_data);
                }
            } else if (val == 0u) {
                haptic_pulse_until_ms_.fill(0u);
                repaint(true, emit, user_data);
            }
            return;
        }

        if (cc == kGridCcDetentSync) {
            if (val >= 1u && val <= TILES_NUM_MACROS) {
                pulseMacroDetent(static_cast<uint8_t>(val - 1u), now_ms, emit, user_data);
            }
            return;
        }

        /* Synchronize Smart Control feedback (CC 102..109) + midpoint detent haptic bump. */
        if (cc >= 102u && cc <= 109u) {
            const uint8_t idx = static_cast<uint8_t>(cc - 102u);
            core_.macros[idx].value = val;
            core_.macros[idx].ring_level = static_cast<uint8_t>((val * 11u) / 127u);
            core_.macros[idx].dirty = false;

            if (isMidpointDetent(val)) {
                if (macro_detent_armed_[idx]) {
                    macro_detent_armed_[idx] = false;
                    pulseMacroDetent(idx, now_ms, emit, user_data);
                }
            } else {
                macro_detent_armed_[idx] = true;
            }
            if (core_.page == TILES_PAGE_SMART_CONTROLS) {
                repaint(false, emit, user_data);
            }
            return;
        }
    }

    tiles_logic_on_grid_midi(&core_, bytes, len, emit, user_data);
}

void StudioLink::onLogicDisplayMidi(
    const uint8_t* bytes,
    size_t len,
    tiles_midi_emit_fn emit,
    void* user_data
) {
    tiles_logic_on_display_midi(&core_, bytes, len, emit, user_data);
}

void StudioLink::emitPadPulseSysEx(uint8_t pad_1_24, tiles_midi_emit_fn emit, void* user_data) {
    if (!emit || pad_1_24 < 1u || pad_1_24 > TILES_NUM_PADS) return;

    const uint8_t col0 = static_cast<uint8_t>((pad_1_24 - 1u) % TILES_NUM_COLS);
    const uint8_t row0 = static_cast<uint8_t>((pad_1_24 - 1u) / TILES_NUM_COLS);

    if (col0 >= TILES_VISIBLE_TRACKS) {
        const uint8_t scene_msg[10] = {
            0xF0u,
            TILES_SYSEX_MFR_ID,
            TILES_SYSEX_SUB_ID,
            TILES_MSG_SCENE_STATE,
            row0,
            TILES_FLAG_IS_TRIGGERED,
            127u,
            0u,
            127u,
            0xF7u,
        };
        emit(TILES_OUT_TILES_DAW, scene_msg, sizeof(scene_msg), user_data);
        return;
    }

    const uint8_t track = static_cast<uint8_t>(core_.track_offset + col0);
    if (track >= TILES_MAX_TRACKS) return;

    const uint8_t clip_msg[11] = {
        0xF0u,
        TILES_SYSEX_MFR_ID,
        TILES_SYSEX_SUB_ID,
        TILES_MSG_CLIP_STATE,
        track,
        row0,
        static_cast<uint8_t>(TILES_FLAG_HAS_CLIP | TILES_FLAG_IS_PLAYING),
        127u,
        0u,
        127u,
        0xF7u,
    };
    emit(TILES_OUT_TILES_DAW, clip_msg, sizeof(clip_msg), user_data);
}

void StudioLink::pulseMacroDetent(uint8_t macro_idx, uint64_t now_ms, tiles_midi_emit_fn emit, void* user_data) {
    if (macro_idx >= TILES_NUM_MACROS) return;
    if (core_.page == TILES_PAGE_SMART_CONTROLS) {
        triggerPadHapticPulse(macroToPad1(macro_idx), now_ms, kDefaultHapticPulseMs, emit, user_data);
    }
}

void StudioLink::triggerPadHapticPulse(
    uint8_t pad_1_24,
    uint64_t now_ms,
    uint64_t duration_ms,
    tiles_midi_emit_fn emit,
    void* user_data
) {
    if (pad_1_24 < 1u || pad_1_24 > TILES_NUM_PADS || !emit || haptic_intensity_ == 0u) return;

    const uint64_t scaled_ms = (duration_ms * static_cast<uint64_t>(haptic_intensity_)) / 100u;
    const uint64_t clamped_ms = std::max<uint64_t>(20u, std::min<uint64_t>(scaled_ms, 500u));
    haptic_pulse_until_ms_[pad_1_24 - 1u] = now_ms + clamped_ms;
    emitPadPulseSysEx(pad_1_24, emit, user_data);
}

void StudioLink::tick(uint64_t now_ms, tiles_midi_emit_fn emit, void* user_data) {
    if (!emit) return;

    bool any_expired = false;
    for (uint8_t i = 0; i < TILES_NUM_PADS; ++i) {
        if (haptic_pulse_until_ms_[i] > 0u && now_ms >= haptic_pulse_until_ms_[i]) {
            haptic_pulse_until_ms_[i] = 0u;
            any_expired = true;
        }
    }

    if (any_expired) {
        core_.pad_cache_valid = false;
        tiles_logic_repaint(&core_, true, emit, user_data);
    }
}

void StudioLink::repaint(bool force_full, tiles_midi_emit_fn emit, void* user_data) {
    tiles_logic_repaint(&core_, force_full, emit, user_data);
}

uint64_t StudioLink::nextExpiryMs() const noexcept {
    uint64_t earliest = 0u;
    for (uint64_t t : haptic_pulse_until_ms_) {
        if (t > 0u && (earliest == 0u || t < earliest)) {
            earliest = t;
        }
    }
    return earliest;
}

} // namespace sentia::logic
