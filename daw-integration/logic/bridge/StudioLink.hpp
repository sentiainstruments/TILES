#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "tiles_logic_core.h"

namespace sentia::logic {

constexpr uint8_t kGridCcMixerBankStep = 114u;
constexpr uint8_t kGridCcHapticIntensity = 115u;
constexpr uint8_t kGridCcDetentSync = 116u;
constexpr uint8_t kGridCcHapticCue = 117u;
constexpr uint64_t kDefaultHapticPulseMs = 65u;

/* Coordinates bidirectional Controller Assignment state and synchronized hardware haptics. */
class StudioLink {
public:
    explicit StudioLink(uint8_t unit_number = 1u);
    void reset(uint8_t unit_number);

    tiles_logic_ctx_t& core() noexcept { return core_; }
    const tiles_logic_ctx_t& core() const noexcept { return core_; }

    void setQuantizedSync(bool enabled) noexcept { quantized_sync_enabled_ = enabled; }
    bool quantizedSync() const noexcept { return quantized_sync_enabled_; }

    void setHapticIntensity(uint8_t level_0_127) noexcept {
        haptic_intensity_ = (level_0_127 > 127u) ? 127u : level_0_127;
    }
    uint8_t hapticIntensity() const noexcept { return haptic_intensity_; }

    void setPerformanceTransport(bool ignored_while_playing) noexcept {
        core_.diamond_ignored_while_playing = ignored_while_playing;
    }

    void setExpressionSplit(bool enabled) noexcept {
        core_.expr_split_enabled = enabled;
    }

    void onHardwareDawMidi(
        const uint8_t* bytes,
        size_t len,
        uint64_t now_ms,
        tiles_midi_emit_fn emit,
        void* user_data
    );

    void onHardwareMpeMidi(
        const uint8_t* bytes,
        size_t len,
        tiles_midi_emit_fn emit,
        void* user_data
    );

    void onLogicMcuMidi(
        const uint8_t* bytes,
        size_t len,
        uint64_t now_ms,
        tiles_midi_emit_fn emit,
        void* user_data
    );

    void onLogicGridMidi(
        const uint8_t* bytes,
        size_t len,
        uint64_t now_ms,
        tiles_midi_emit_fn emit,
        void* user_data
    );

    void onLogicDisplayMidi(
        const uint8_t* bytes,
        size_t len,
        tiles_midi_emit_fn emit,
        void* user_data
    );

    void triggerPadHapticPulse(
        uint8_t pad_1_24,
        uint64_t now_ms,
        uint64_t duration_ms,
        tiles_midi_emit_fn emit,
        void* user_data
    );

    void tick(uint64_t now_ms, tiles_midi_emit_fn emit, void* user_data);

    void repaint(bool force_full, tiles_midi_emit_fn emit, void* user_data);

    uint64_t nextExpiryMs() const noexcept;

private:
    void emitPadPulseSysEx(uint8_t pad_1_24, tiles_midi_emit_fn emit, void* user_data);
    void pulseMacroDetent(uint8_t macro_idx, uint64_t now_ms, tiles_midi_emit_fn emit, void* user_data);

    tiles_logic_ctx_t core_{};
    std::array<uint64_t, TILES_NUM_PADS> haptic_pulse_until_ms_{};
    std::array<bool, TILES_NUM_MACROS> macro_detent_armed_{};
    uint32_t clock_ticks_ = 0;
    uint8_t haptic_intensity_ = 100u;
    bool quantized_sync_enabled_ = true;
};

} // namespace sentia::logic
