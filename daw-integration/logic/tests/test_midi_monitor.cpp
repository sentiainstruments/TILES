#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>

#include "../bridge/MIDIMonitor.hpp"

int main() {
    using sentia::logic::MIDIMonitor;

    MIDIMonitor monitor;
    assert(!monitor.isWindowOpen());
    assert(monitor.openWindow());
    assert(monitor.isWindowOpen());

    /* Incoming MPE NoteOn + Pressure + PitchBend on Member Ch 2 (0x91 / 0xD1 / 0xE1). */
    const uint8_t note_on[3] = {0x91u, 60u, 108u};
    const uint8_t press[2] = {0xD1u, 88u};
    const uint8_t tilt[3] = {0xE1u, 0x00u, 0x60u}; /* +4096 */
    monitor.recordPacket(100, MIDIMonitor::Port::TilesMpe, MIDIMonitor::Direction::RxFromHardware, note_on, sizeof(note_on));
    monitor.recordPacket(102, MIDIMonitor::Port::TilesMpe, MIDIMonitor::Direction::RxFromHardware, press, sizeof(press));
    monitor.recordPacket(104, MIDIMonitor::Port::TilesMpe, MIDIMonitor::Direction::RxFromHardware, tilt, sizeof(tilt));

    /* Outgoing MPE forward to Logic Pro and Auto-Assign focus CC on Ch 16. */
    monitor.recordPacket(105, MIDIMonitor::Port::LogicMpe, MIDIMonitor::Direction::TxToLogic, tilt, sizeof(tilt));
    const uint8_t focus_press[3] = {0xBFu, TILES_EXPR_CC_PRESS, 88u};
    monitor.recordPacket(106, MIDIMonitor::Port::LogicGrid, MIDIMonitor::Direction::TxToLogic, focus_press, sizeof(focus_press));

    /* Outgoing non-MPE SysEx LED feedback to hardware DAW port. */
    const uint8_t clip_sysex[10] = {0xF0u, 0x7Du, 0x01u, 0x10u, 2u, 0x7Fu, 0x00u, 0x7Fu, 0x01u, 0xF7u};
    monitor.recordPacket(110, MIDIMonitor::Port::TilesDaw, MIDIMonitor::Direction::TxToHardware, clip_sysex, sizeof(clip_sysex));

    const size_t drained = monitor.drainAndRefreshUi();
    assert(drained == 6);

    const MIDIMonitor::Counters c = monitor.counters();
    assert(c.total_recorded == 6);
    assert(c.incoming_count == 3);
    assert(c.outgoing_count == 3);
    assert(c.mpe_count == 5);
    assert(c.dropped_count == 0);

    const MIDIMonitor::MpeChannelState ch2 = monitor.channelState(2);
    assert(ch2.gate_active);
    assert(ch2.active_note == 60);
    assert(ch2.velocity == 108);
    assert(ch2.pressure == 88);
    assert(ch2.pitch_bend == 4096);

    const auto& recent = monitor.recentEvents();
    assert(recent.size() == 6);
    const std::string line_note = MIDIMonitor::formatEventLine(recent[0]);
    const std::string line_tilt = MIDIMonitor::formatEventLine(recent[2]);
    const std::string line_focus = MIDIMonitor::formatEventLine(recent[4]);
    const std::string line_sysex = MIDIMonitor::formatEventLine(recent[5]);
    assert(line_note.find("IN  HW->BR") != std::string::npos);
    assert(line_note.find("NoteOn") != std::string::npos);
    assert(line_tilt.find("TiltBend bend= +4096") != std::string::npos);
    assert(line_focus.find("FocusPress") != std::string::npos);
    assert(line_sysex.find("CLIP_STATE pad=2") != std::string::npos);

    /* Incoming-only filter excludes outgoing events from history while still updating voice telemetry. */
    monitor.clear();
    assert(monitor.recentEvents().empty());
    assert(monitor.counters().total_recorded == 0);

    monitor.setFilterMode(MIDIMonitor::FilterMode::IncomingOnly);
    const uint8_t note_off[3] = {0x81u, 60u, 0u};
    const uint8_t slide_cc74[3] = {0xB2u, 74u, 99u};
    monitor.recordPacket(200, MIDIMonitor::Port::TilesMpe, MIDIMonitor::Direction::RxFromHardware, note_off, sizeof(note_off));
    monitor.recordPacket(201, MIDIMonitor::Port::LogicMpe, MIDIMonitor::Direction::TxToLogic, slide_cc74, sizeof(slide_cc74));
    assert(monitor.drainAndRefreshUi() == 2);
    assert(monitor.recentEvents().size() == 1);
    assert(monitor.recentEvents()[0].kind == MIDIMonitor::EventKind::NoteOff);
    assert(!monitor.channelState(2).gate_active);
    assert(monitor.channelState(3).slide_cc74 == 99);

    /* Pausing freezes recentEvents while still advancing incoming Note/MPE telemetry. */
    monitor.setFilterMode(MIDIMonitor::FilterMode::MpeOnly);
    monitor.setPaused(true);
    monitor.recordPacket(210, MIDIMonitor::Port::TilesMpe, MIDIMonitor::Direction::RxFromHardware, note_on, sizeof(note_on));
    assert(monitor.drainAndRefreshUi() == 1);
    assert(monitor.recentEvents().size() == 1);
    assert(monitor.channelState(2).gate_active);

    monitor.closeWindow();
    assert(!monitor.isWindowOpen());

    std::printf("All SENTIA TILES MIDIMonitor tests passed.\n");
    return 0;
}
