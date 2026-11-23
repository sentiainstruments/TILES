#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../bridge/MIDILogger.hpp"

int main() {
    using sentia::logic::MIDILogger;

    MIDILogger logger;
    std::vector<std::string> lines;
    logger.setSink([&lines](const std::string& line) {
        lines.push_back(line);
    });

    /* Disabled logger drops nothing into ring buffer. */
    const uint8_t mpe_press[2] = {0xD1u, 96u};
    logger.log(10, MIDILogger::Port::TilesMpe, MIDILogger::Direction::RxFromHardware, mpe_press, sizeof(mpe_press));
    assert(logger.flush() == 0);
    assert(lines.empty());

    /* Enable and verify MPE Tilt + Press + Focus CC decoding. */
    logger.setEnabled(true);
    const uint8_t mpe_tilt[3] = {0xE2u, 0x00u, 0x60u}; /* ch 3 bend */
    logger.log(25, MIDILogger::Port::TilesMpe, MIDILogger::Direction::RxFromHardware, mpe_tilt, sizeof(mpe_tilt));

    const uint8_t focus_cc[3] = {0xBFu, 22u, 96u}; /* Ch 16 CC 22 Tilt */
    logger.log(26, MIDILogger::Port::LogicGrid, MIDILogger::Direction::TxToLogic, focus_cc, sizeof(focus_cc));

    /* Verify TILES SysEx haptic CLIP_STATE decoding. */
    const uint8_t clip_sysex[10] = {0xF0u, 0x7Du, 0x01u, 0x10u, 1u, 0x7Fu, 0x00u, 0x7Fu, 0x03u, 0xF7u};
    logger.log(30, MIDILogger::Port::TilesDaw, MIDILogger::Direction::TxToHardware, clip_sysex, sizeof(clip_sysex));

    /* Verify StudioLink Haptic Intensity CC 115 decoding. */
    const uint8_t haptic_cc[3] = {0xB0u, 115u, 100u};
    logger.log(35, MIDILogger::Port::LogicGrid, MIDILogger::Direction::RxFromLogic, haptic_cc, sizeof(haptic_cc));

    /* Verify Scripter MIDILogger toggle CC 85 / filter CC 86 decoding. */
    const uint8_t log_enable[3] = {0xB0u, 85u, 127u};
    const uint8_t log_filter[3] = {0xB0u, 86u, 1u};
    logger.log(36, MIDILogger::Port::LogicGrid, MIDILogger::Direction::RxFromLogic, log_enable, sizeof(log_enable));
    logger.log(37, MIDILogger::Port::LogicGrid, MIDILogger::Direction::RxFromLogic, log_filter, sizeof(log_filter));

    const size_t flushed = logger.flush();
    assert(flushed == 6);
    assert(lines.size() == 6);
    assert(lines[0].find("MPE_Tilt ch=3") != std::string::npos);
    assert(lines[1].find("MPE_FOCUS Tilt=96") != std::string::npos);
    assert(lines[2].find("SysEx CLIP_STATE pad=1") != std::string::npos);
    assert(lines[2].find("HAS_CLIP") != std::string::npos);
    assert(lines[2].find("IS_PLAYING") != std::string::npos);
    assert(lines[3].find("HAPTIC_INTENSITY val=100") != std::string::npos);
    assert(lines[4].find("MIDI_LOGGER ON") != std::string::npos);
    assert(lines[5].find("MIDI_LOGGER_FILTER MPE") != std::string::npos);

    /* Category filter keeps only haptic traffic when CatHaptic is selected. */
    lines.clear();
    logger.setCategoryMask(MIDILogger::CatHaptic);
    logger.log(40, MIDILogger::Port::TilesMpe, MIDILogger::Direction::RxFromHardware, mpe_press, sizeof(mpe_press));
    logger.log(41, MIDILogger::Port::LogicGrid, MIDILogger::Direction::RxFromLogic, haptic_cc, sizeof(haptic_cc));
    assert(logger.flush() == 1);
    assert(lines[0].find("HAPTIC_INTENSITY val=100") != std::string::npos);

    /* Verify overflow drop counter behaves deterministically. */
    logger.setCategoryMask(MIDILogger::CatAll);
    for (size_t i = 0; i < MIDILogger::kRingCapacity + 10u; ++i) {
        logger.log(100 + i, MIDILogger::Port::TilesMpe, MIDILogger::Direction::RxFromHardware, mpe_press, sizeof(mpe_press));
    }
    assert(logger.droppedCount() > 0);
    assert(logger.flush() == MIDILogger::kRingCapacity - 1u);

    std::printf("All SENTIA TILES MIDILogger tests passed.\n");
    return 0;
}
