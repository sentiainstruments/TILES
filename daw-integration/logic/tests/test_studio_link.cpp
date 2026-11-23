#include "../bridge/StudioLink.hpp"

#include <cassert>
#include <cstdio>
#include <vector>

namespace {

struct RecordedMsg {
    tiles_out_target_t target;
    std::vector<uint8_t> bytes;
};

void collectEmit(tiles_out_target_t target, const uint8_t* bytes, size_t len, void* user_data) {
    auto* out = static_cast<std::vector<RecordedMsg>*>(user_data);
    out->push_back(RecordedMsg{target, std::vector<uint8_t>(bytes, bytes + len)});
}

bool hasSysExClipFlag(const std::vector<RecordedMsg>& msgs, uint8_t flag_mask) {
    for (const auto& m : msgs) {
        if (m.target == TILES_OUT_TILES_DAW && m.bytes.size() >= 11u &&
            m.bytes[0] == 0xF0u && m.bytes[3] == TILES_MSG_CLIP_STATE &&
            (m.bytes[6] & flag_mask) == flag_mask) {
            return true;
        }
    }
    return false;
}

bool hasSysExSceneTriggered(const std::vector<RecordedMsg>& msgs, uint8_t scene_row) {
    for (const auto& m : msgs) {
        if (m.target == TILES_OUT_TILES_DAW && m.bytes.size() >= 10u &&
            m.bytes[0] == 0xF0u && m.bytes[3] == TILES_MSG_SCENE_STATE &&
            m.bytes[4] == scene_row && (m.bytes[5] & TILES_FLAG_IS_TRIGGERED) != 0u) {
            return true;
        }
    }
    return false;
}

void testHapticCueAndExpiry() {
    sentia::logic::StudioLink link(1u);
    std::vector<RecordedMsg> msgs;

    /* Logic Controller Assignment sends Haptic Cue CC 117 for Pad 3 -> SysEx clip pulse. */
    const uint8_t cue[3] = {0xB0u, sentia::logic::kGridCcHapticCue, 3u};
    link.onLogicGridMidi(cue, sizeof(cue), 1000u, collectEmit, &msgs);
    assert(hasSysExClipFlag(msgs, TILES_FLAG_HAS_CLIP | TILES_FLAG_IS_PLAYING));
    assert(link.nextExpiryMs() == 1000u + sentia::logic::kDefaultHapticPulseMs);

    /* Before expiry: tick does nothing. */
    msgs.clear();
    link.tick(1050u, collectEmit, &msgs);
    assert(msgs.empty());
    assert(link.nextExpiryMs() == 1065u);

    /* At expiry: tick clears pulse and repaints baseline state. */
    msgs.clear();
    link.tick(1065u, collectEmit, &msgs);
    assert(link.nextExpiryMs() == 0u);
    assert(!msgs.empty());
}

void testSmartControlDetentSync() {
    sentia::logic::StudioLink link(1u);
    std::vector<RecordedMsg> msgs;

    const uint8_t smart_page[3] = {0xB0u, TILES_LOGIC_CC_PAGE_SELECT, 1u};
    link.onLogicGridMidi(smart_page, sizeof(smart_page), 100u, collectEmit, &msgs);
    assert(link.core().page == TILES_PAGE_SMART_CONTROLS);

    /* Sweeping Smart Control 1 (CC 102) onto midpoint 64 emits detent haptic pulse on Pad 1. */
    msgs.clear();
    const uint8_t detent[3] = {0xBFu, 102u, 64u};
    link.onLogicGridMidi(detent, sizeof(detent), 200u, collectEmit, &msgs);
    assert(link.core().macros[0].value == 64u);
    assert(hasSysExClipFlag(msgs, TILES_FLAG_HAS_CLIP | TILES_FLAG_IS_PLAYING));
    assert(link.nextExpiryMs() == 200u + sentia::logic::kDefaultHapticPulseMs);
}

void testBeatQuantizedSceneHapticSync() {
    sentia::logic::StudioLink link(1u);
    std::vector<RecordedMsg> msgs;

    /* Trigger Scene Row 1 via Live Loops grid CC. */
    const uint8_t scene_msg[10] = {
        0xF0u, TILES_SYSEX_MFR_ID, TILES_SYSEX_SUB_ID, TILES_MSG_SCENE_STATE,
        1u, TILES_FLAG_IS_TRIGGERED, 127u, 0u, 127u, 0xF7u,
    };
    link.onLogicGridMidi(scene_msg, sizeof(scene_msg), 100u, collectEmit, &msgs);

    /* Feed 24 MIDI clock ticks (1 quarter note) -> emits synchronized scene haptic cue. */
    msgs.clear();
    const uint8_t clk = 0xF8u;
    for (int i = 0; i < 24; ++i) {
        link.onLogicGridMidi(&clk, 1u, static_cast<uint64_t>(200 + i), collectEmit, &msgs);
    }
    assert(hasSysExSceneTriggered(msgs, 1u));
    assert(link.nextExpiryMs() > 0u);
}

void testMixerBankStepAndHapticIntensity() {
    sentia::logic::StudioLink link(1u);
    std::vector<RecordedMsg> msgs;

    /* CC 114 >= 64 advances track window +5; second step reaches track 10 (MCU bank 1). */
    const uint8_t bank_next[3] = {0xB0u, sentia::logic::kGridCcMixerBankStep, 127u};
    link.onLogicGridMidi(bank_next, sizeof(bank_next), 100u, collectEmit, &msgs);
    assert(link.core().track_offset == 5u);
    link.onLogicGridMidi(bank_next, sizeof(bank_next), 120u, collectEmit, &msgs);
    assert(link.core().track_offset == 10u);
    assert(link.core().mcu_bank_offset == 1u);

    /* CC 115 == 0 mutes synchronized haptic pulses. */
    const uint8_t h_mute[3] = {0xB0u, sentia::logic::kGridCcHapticIntensity, 0u};
    link.onLogicGridMidi(h_mute, sizeof(h_mute), 150u, collectEmit, &msgs);
    assert(link.hapticIntensity() == 0u);
    msgs.clear();
    const uint8_t cue[3] = {0xB0u, sentia::logic::kGridCcHapticCue, 2u};
    link.onLogicGridMidi(cue, sizeof(cue), 200u, collectEmit, &msgs);
    assert(msgs.empty());
    assert(link.nextExpiryMs() == 0u);
}

} // namespace

int main() {
    testHapticCueAndExpiry();
    testSmartControlDetentSync();
    testBeatQuantizedSceneHapticSync();
    testMixerBankStepAndHapticIntensity();
    std::printf("All SENTIA TILES StudioLink tests passed.\n");
    return 0;
}
