#include "MIDILogger.hpp"

#include "tiles_logic_core.h"

#include <cstdio>
#include <utility>

namespace sentia::logic {

namespace {

int32_t decodePitchBendSigned(uint8_t lsb, uint8_t msb) noexcept {
    const uint16_t raw = static_cast<uint16_t>((static_cast<uint16_t>(msb & 0x7Fu) << 7u) | (lsb & 0x7Fu));
    return static_cast<int32_t>(raw) - 8192;
}

std::string formatHexBytes(const uint8_t* bytes, uint8_t len) {
    std::string out;
    out.reserve(static_cast<size_t>(len) * 3u);
    char buf[8];
    for (uint8_t i = 0; i < len; ++i) {
        std::snprintf(buf, sizeof(buf), i == 0 ? "%02X" : " %02X", bytes[i]);
        out += buf;
    }
    return out;
}

std::string decodeSysEx(const uint8_t* bytes, uint8_t len) {
    char buf[160];
    if (len >= 4 && bytes[1] == TILES_SYSEX_MFR_ID && bytes[2] == TILES_SYSEX_SUB_ID) {
        const uint8_t cmd = bytes[3];
        if (cmd == TILES_MSG_CLIP_STATE && len >= 9) {
            const uint8_t pad = bytes[4];
            const uint8_t r = bytes[5];
            const uint8_t g = bytes[6];
            const uint8_t b = bytes[7];
            const uint8_t flags = bytes[8];
            const bool has_clip = (flags & TILES_FLAG_HAS_CLIP) != 0u;
            const bool playing = (flags & TILES_FLAG_IS_PLAYING) != 0u;
            const bool trig = (flags & TILES_FLAG_IS_TRIGGERED) != 0u;
            std::snprintf(
                buf,
                sizeof(buf),
                "SysEx CLIP_STATE pad=%u rgb7=(%u,%u,%u) flags=[%s%s%s]",
                static_cast<unsigned>(pad),
                static_cast<unsigned>(r),
                static_cast<unsigned>(g),
                static_cast<unsigned>(b),
                has_clip ? "HAS_CLIP " : "",
                playing ? "IS_PLAYING " : "",
                trig ? "TRIGGERED" : (has_clip || playing ? "" : "NONE")
            );
            return std::string(buf);
        }
        if (cmd == TILES_MSG_SCENE_STATE && len >= 9) {
            const uint8_t scene = bytes[4];
            const uint8_t flags = bytes[8];
            std::snprintf(
                buf,
                sizeof(buf),
                "SysEx SCENE_STATE scene=%u rgb7=(%u,%u,%u) flags=0x%02X",
                static_cast<unsigned>(scene),
                static_cast<unsigned>(bytes[5]),
                static_cast<unsigned>(bytes[6]),
                static_cast<unsigned>(bytes[7]),
                static_cast<unsigned>(flags)
            );
            return std::string(buf);
        }
        if (cmd == TILES_MSG_OPEN_MELODIC && len >= 5) {
            std::snprintf(buf, sizeof(buf), "SysEx OPEN_MELODIC track=%u", static_cast<unsigned>(bytes[4]));
            return std::string(buf);
        }
    }

    if (len >= 6 && bytes[1] == 0x00u && bytes[2] == 0x00u && bytes[3] == 0x66u && bytes[4] == 0x14u) {
        std::snprintf(buf, sizeof(buf), "SysEx MCU msg=0x%02X", static_cast<unsigned>(bytes[5]));
        return std::string(buf);
    }

    return "SysEx";
}

std::string decodeCC(MIDILogger::Port port, uint8_t ch1, uint8_t cc, uint8_t val) {
    char buf[160];

    if (port == MIDILogger::Port::LogicGrid || port == MIDILogger::Port::TilesMpe || port == MIDILogger::Port::LogicMpe) {
        if (ch1 == 1 && cc == TILES_LOGIC_CC_LOG_ENABLE) {
            std::snprintf(buf, sizeof(buf), "MIDI_LOGGER %s", val > 0 ? "ON" : "OFF");
            return std::string(buf);
        }
        if (ch1 == 1 && cc == TILES_LOGIC_CC_LOG_FILTER) {
            static const char* kFilterNames[5] = {"ALL", "MPE", "HAPTIC", "GRID", "MCU"};
            const char* label = (val < 5u) ? kFilterNames[val] : "ALL";
            std::snprintf(buf, sizeof(buf), "MIDI_LOGGER_FILTER %s", label);
            return std::string(buf);
        }
        if (ch1 == 1 && cc == 115u) {
            std::snprintf(buf, sizeof(buf), "HAPTIC_INTENSITY val=%u%s", static_cast<unsigned>(val), val == 0 ? " (MUTED)" : "");
            return std::string(buf);
        }
        if (ch1 == 1 && cc == 116u) {
            std::snprintf(buf, sizeof(buf), "HAPTIC_DETENT_SYNC macro=%u", static_cast<unsigned>(val));
            return std::string(buf);
        }
        if (ch1 == 1 && cc == 117u) {
            std::snprintf(buf, sizeof(buf), "HAPTIC_DIRECT_CUE target=%u", static_cast<unsigned>(val));
            return std::string(buf);
        }
        if (ch1 == 1 && cc == 118u) {
            std::snprintf(buf, sizeof(buf), "EXPR_SPLIT %s", val > 0 ? "ON" : "OFF");
            return std::string(buf);
        }
        if (ch1 == 1 && cc == 119u) {
            std::snprintf(buf, sizeof(buf), "PAGE_SELECT val=%u", static_cast<unsigned>(val));
            return std::string(buf);
        }
        if (ch1 == 1 && cc == 114u) {
            std::snprintf(buf, sizeof(buf), "MIXER_BANK_STEP %s", val < 64 ? "-5" : "+5");
            return std::string(buf);
        }
        if (ch1 == 16) {
            switch (cc) {
                case TILES_EXPR_CC_STRIKE:
                    std::snprintf(buf, sizeof(buf), "MPE_FOCUS Strike=%u", static_cast<unsigned>(val));
                    return std::string(buf);
                case TILES_EXPR_CC_PRESS:
                    std::snprintf(buf, sizeof(buf), "MPE_FOCUS Press=%u", static_cast<unsigned>(val));
                    return std::string(buf);
                case TILES_EXPR_CC_TILT:
                    std::snprintf(buf, sizeof(buf), "MPE_FOCUS Tilt=%u", static_cast<unsigned>(val));
                    return std::string(buf);
                case TILES_EXPR_CC_TOUCH:
                    std::snprintf(buf, sizeof(buf), "MPE_FOCUS Touch=%s", val > 0 ? "DOWN" : "UP");
                    return std::string(buf);
                case TILES_GRID_CC_CELL_FIRE:
                    std::snprintf(buf, sizeof(buf), "GRID_CELL_FIRE pad=%u", static_cast<unsigned>(val));
                    return std::string(buf);
                case TILES_GRID_CC_CELL_STOP:
                    std::snprintf(buf, sizeof(buf), "GRID_CELL_STOP pad=%u", static_cast<unsigned>(val));
                    return std::string(buf);
                case TILES_GRID_CC_CELL_RECORD:
                    std::snprintf(buf, sizeof(buf), "GRID_CELL_REC pad=%u", static_cast<unsigned>(val));
                    return std::string(buf);
                case TILES_GRID_CC_CELL_DELETE:
                    std::snprintf(buf, sizeof(buf), "GRID_CELL_DEL pad=%u", static_cast<unsigned>(val));
                    return std::string(buf);
                case TILES_GRID_CC_SCENE_FIRE:
                    std::snprintf(buf, sizeof(buf), "GRID_SCENE_FIRE scene=%u", static_cast<unsigned>(val));
                    return std::string(buf);
                default:
                    break;
            }
        }
        if (ch1 >= 2 && ch1 <= 9) {
            switch (cc) {
                case TILES_EXPR_VOICE_CC_STRIKE:
                    std::snprintf(buf, sizeof(buf), "MPE_VOICE(%u) Strike=%u", static_cast<unsigned>(ch1), static_cast<unsigned>(val));
                    return std::string(buf);
                case TILES_EXPR_VOICE_CC_PRESS:
                    std::snprintf(buf, sizeof(buf), "MPE_VOICE(%u) Press=%u", static_cast<unsigned>(ch1), static_cast<unsigned>(val));
                    return std::string(buf);
                case TILES_EXPR_VOICE_CC_TILT:
                    std::snprintf(buf, sizeof(buf), "MPE_VOICE(%u) Tilt=%u", static_cast<unsigned>(ch1), static_cast<unsigned>(val));
                    return std::string(buf);
                case TILES_EXPR_VOICE_CC_TOUCH:
                    std::snprintf(buf, sizeof(buf), "MPE_VOICE(%u) Touch=%s", static_cast<unsigned>(ch1), val > 0 ? "DOWN" : "UP");
                    return std::string(buf);
                case 74u:
                    std::snprintf(buf, sizeof(buf), "MPE_VOICE(%u) Slide(CC74)=%u", static_cast<unsigned>(ch1), static_cast<unsigned>(val));
                    return std::string(buf);
                default:
                    break;
            }
        }
    }

    if (port == MIDILogger::Port::TilesDaw && ch1 == 1) {
        if (cc >= TILES_CC_PLAY && cc <= TILES_CC_RECORD_TOUCH) {
            std::snprintf(buf, sizeof(buf), "DAW_CC %u val=%u", static_cast<unsigned>(cc), static_cast<unsigned>(val));
            return std::string(buf);
        }
    }

    std::snprintf(buf, sizeof(buf), "CC ch=%u cc=%u val=%u", static_cast<unsigned>(ch1), static_cast<unsigned>(cc), static_cast<unsigned>(val));
    return std::string(buf);
}

std::string decodeMessage(const MIDILogger::Entry& entry) {
    if (entry.length == 0) return "Empty";

    const uint8_t st = entry.bytes[0];
    if (st == 0xF8u) return "Clock (0xF8)";
    if (st == 0xFAu) return "Start (0xFA)";
    if (st == 0xFCu) return "Stop (0xFC)";
    if (st == 0xF0u) return decodeSysEx(entry.bytes.data(), entry.length);

    const uint8_t msg_type = st & 0xF0u;
    const uint8_t ch1 = static_cast<uint8_t>((st & 0x0Fu) + 1u);
    char buf[160];

    if (msg_type == 0x90u && entry.length >= 3) {
        if (entry.bytes[2] == 0) {
            std::snprintf(buf, sizeof(buf), "NoteOff ch=%u note=%u", static_cast<unsigned>(ch1), static_cast<unsigned>(entry.bytes[1]));
        } else {
            std::snprintf(buf, sizeof(buf), "NoteOn ch=%u note=%u vel=%u", static_cast<unsigned>(ch1), static_cast<unsigned>(entry.bytes[1]), static_cast<unsigned>(entry.bytes[2]));
        }
        return std::string(buf);
    }
    if (msg_type == 0x80u && entry.length >= 3) {
        std::snprintf(buf, sizeof(buf), "NoteOff ch=%u note=%u vel=%u", static_cast<unsigned>(ch1), static_cast<unsigned>(entry.bytes[1]), static_cast<unsigned>(entry.bytes[2]));
        return std::string(buf);
    }
    if (msg_type == 0xD0u && entry.length >= 2) {
        std::snprintf(buf, sizeof(buf), "MPE_Press ch=%u pressure=%u", static_cast<unsigned>(ch1), static_cast<unsigned>(entry.bytes[1]));
        return std::string(buf);
    }
    if (msg_type == 0xE0u && entry.length >= 3) {
        const int32_t bend = decodePitchBendSigned(entry.bytes[1], entry.bytes[2]);
        std::snprintf(buf, sizeof(buf), "MPE_Tilt ch=%u bend=%d", static_cast<unsigned>(ch1), static_cast<int>(bend));
        return std::string(buf);
    }
    if (msg_type == 0xB0u && entry.length >= 3) {
        return decodeCC(entry.port, ch1, entry.bytes[1], entry.bytes[2]);
    }
    if (msg_type == 0xC0u && entry.length >= 2) {
        std::snprintf(buf, sizeof(buf), "Program ch=%u pgm=%u", static_cast<unsigned>(ch1), static_cast<unsigned>(entry.bytes[1]));
        return std::string(buf);
    }

    std::snprintf(buf, sizeof(buf), "MIDI status=0x%02X len=%u", static_cast<unsigned>(st), static_cast<unsigned>(entry.length));
    return std::string(buf);
}

} // namespace

MIDILogger::MIDILogger() = default;

void MIDILogger::setEnabled(bool enabled) noexcept {
    enabled_.store(enabled, std::memory_order_release);
}

bool MIDILogger::isEnabled() const noexcept {
    return enabled_.load(std::memory_order_acquire);
}

void MIDILogger::setCategoryMask(uint32_t mask) noexcept {
    category_mask_.store(mask, std::memory_order_release);
}

uint32_t MIDILogger::categoryMask() const noexcept {
    return category_mask_.load(std::memory_order_acquire);
}

void MIDILogger::setSink(SinkCallback sink) {
    sink_ = std::move(sink);
}

uint32_t MIDILogger::classifyMessage(Port port, const uint8_t* bytes, size_t len) noexcept {
    if (!bytes || len == 0) return CatOther;

    const uint8_t st = bytes[0];
    if (st == 0xF0u) {
        if (len >= 4 && bytes[1] == TILES_SYSEX_MFR_ID && bytes[2] == TILES_SYSEX_SUB_ID &&
            (bytes[3] == TILES_MSG_CLIP_STATE || bytes[3] == TILES_MSG_SCENE_STATE || bytes[3] == TILES_MSG_OPEN_MELODIC)) {
            return CatHaptic | CatGrid;
        }
        return (port == Port::LogicMcu) ? CatMcu : CatOther;
    }

    const uint8_t msg_type = st & 0xF0u;
    const uint8_t ch1 = static_cast<uint8_t>((st & 0x0Fu) + 1u);

    if (port == Port::TilesMpe || port == Port::LogicMpe) {
        return CatMpe;
    }

    if (port == Port::LogicMcu) {
        return CatMcu;
    }

    if (msg_type == 0xD0u || msg_type == 0xE0u) {
        return CatMpe;
    }

    if (msg_type == 0xB0u && len >= 3) {
        const uint8_t cc = bytes[1];
        if (ch1 == 1 && (cc == 115u || cc == 116u || cc == 117u)) {
            return CatHaptic;
        }
        if (ch1 == 16 && cc >= TILES_EXPR_CC_STRIKE && cc <= TILES_EXPR_CC_TOUCH) {
            return CatMpe;
        }
        if (ch1 >= 2 && ch1 <= 9 && cc >= TILES_EXPR_VOICE_CC_STRIKE && cc <= TILES_EXPR_VOICE_CC_TOUCH) {
            return CatMpe;
        }
        return CatGrid;
    }

    return (port == Port::LogicGrid || port == Port::TilesDaw) ? CatGrid : CatOther;
}

void MIDILogger::log(
    uint64_t timestamp_ms,
    Port port,
    Direction direction,
    const uint8_t* bytes,
    size_t len
) noexcept {
    if (!enabled_.load(std::memory_order_acquire) || !bytes || len == 0) {
        return;
    }

    const uint32_t cat = classifyMessage(port, bytes, len);
    if ((cat & category_mask_.load(std::memory_order_acquire)) == 0u) {
        return;
    }

    const size_t w = write_idx_.load(std::memory_order_relaxed);
    const size_t next_w = (w + 1u) % kRingCapacity;
    if (next_w == read_idx_.load(std::memory_order_acquire)) {
        dropped_.fetch_add(1u, std::memory_order_relaxed);
        return;
    }

    Entry& slot = ring_[w];
    slot.timestamp_ms = timestamp_ms;
    slot.port = port;
    slot.direction = direction;
    slot.categories = cat;
    const uint8_t copy_len = static_cast<uint8_t>(len > slot.bytes.size() ? slot.bytes.size() : len);
    slot.length = copy_len;
    for (uint8_t i = 0; i < copy_len; ++i) {
        slot.bytes[i] = bytes[i];
    }

    write_idx_.store(next_w, std::memory_order_release);
}

size_t MIDILogger::flush() {
    size_t count = 0;
    size_t r = read_idx_.load(std::memory_order_relaxed);
    const size_t w = write_idx_.load(std::memory_order_acquire);

    while (r != w) {
        const std::string line = formatEntry(ring_[r]);
        if (sink_) {
            sink_(line);
        } else {
            std::fprintf(stderr, "%s\n", line.c_str());
        }
        r = (r + 1u) % kRingCapacity;
        read_idx_.store(r, std::memory_order_release);
        ++count;
    }

    return count;
}

uint64_t MIDILogger::droppedCount() const noexcept {
    return dropped_.load(std::memory_order_acquire);
}

const char* MIDILogger::portLabel(Port port) noexcept {
    switch (port) {
        case Port::TilesDaw:
            return "TILES-DAW";
        case Port::TilesMpe:
            return "TILES-MPE";
        case Port::LogicMcu:
            return "LOGIC-MCU";
        case Port::LogicGrid:
            return "LOGIC-GRID";
        case Port::LogicDisplay:
            return "LOGIC-DISP";
        case Port::LogicMpe:
            return "LOGIC-MPE";
    }
    return "MIDI";
}

const char* MIDILogger::directionLabel(Direction direction) noexcept {
    switch (direction) {
        case Direction::RxFromHardware:
            return "HW->BR";
        case Direction::TxToHardware:
            return "BR->HW";
        case Direction::RxFromLogic:
            return "LG->BR";
        case Direction::TxToLogic:
            return "BR->LG";
    }
    return "--";
}

std::string MIDILogger::formatEntry(const Entry& entry) {
    char prefix[64];
    std::snprintf(
        prefix,
        sizeof(prefix),
        "[%08llu ms] %-6s %-10s | ",
        static_cast<unsigned long long>(entry.timestamp_ms),
        directionLabel(entry.direction),
        portLabel(entry.port)
    );

    const std::string decoded = decodeMessage(entry);
    const std::string hex = formatHexBytes(entry.bytes.data(), entry.length);
    return std::string(prefix) + decoded + " [" + hex + "]";
}

} // namespace sentia::logic
