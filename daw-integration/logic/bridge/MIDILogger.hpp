#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace sentia::logic {

class MIDILogger {
public:
    enum class Port : uint8_t {
        TilesDaw = 0,
        TilesMpe = 1,
        LogicMcu = 2,
        LogicGrid = 3,
        LogicDisplay = 4,
        LogicMpe = 5
    };

    enum class Direction : uint8_t {
        RxFromHardware = 0,
        TxToHardware = 1,
        RxFromLogic = 2,
        TxToLogic = 3
    };

    enum Category : uint32_t {
        CatMpe = 1u << 0,
        CatHaptic = 1u << 1,
        CatGrid = 1u << 2,
        CatMcu = 1u << 3,
        CatOther = 1u << 4,
        CatAll = 0xFFFFFFFFu
    };

    struct Entry {
        uint64_t timestamp_ms = 0;
        Port port = Port::TilesDaw;
        Direction direction = Direction::RxFromHardware;
        uint32_t categories = 0;
        uint8_t length = 0;
        std::array<uint8_t, 24> bytes{};
    };

    using SinkCallback = std::function<void(const std::string& formatted_line)>;

    static constexpr size_t kRingCapacity = 1024;

    MIDILogger();
    ~MIDILogger() = default;

    MIDILogger(const MIDILogger&) = delete;
    MIDILogger& operator=(const MIDILogger&) = delete;

    void setEnabled(bool enabled) noexcept;
    bool isEnabled() const noexcept;

    void setCategoryMask(uint32_t mask) noexcept;
    uint32_t categoryMask() const noexcept;

    void setSink(SinkCallback sink);

    /* Non-allocating enqueue safe to invoke from CoreMIDI real-time callbacks. */
    void log(
        uint64_t timestamp_ms,
        Port port,
        Direction direction,
        const uint8_t* bytes,
        size_t len
    ) noexcept;

    /* Drains queued entries and writes formatted lines to the configured sink (or stderr). */
    size_t flush();

    uint64_t droppedCount() const noexcept;

    static uint32_t classifyMessage(Port port, const uint8_t* bytes, size_t len) noexcept;
    static std::string formatEntry(const Entry& entry);
    static const char* portLabel(Port port) noexcept;
    static const char* directionLabel(Direction direction) noexcept;

private:
    std::atomic<bool> enabled_{false};
    std::atomic<uint32_t> category_mask_{CatAll};
    std::atomic<size_t> write_idx_{0};
    std::atomic<size_t> read_idx_{0};
    std::atomic<uint64_t> dropped_{0};
    std::array<Entry, kRingCapacity> ring_{};
    SinkCallback sink_;
};

} // namespace sentia::logic
