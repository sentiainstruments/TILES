#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "tiles_logic_core.h"

namespace sentia::logic {

/*
 * Real-time MPE & DAW MIDI Monitor window for debugging TILES <-> Logic Pro streams.
 * Ingestion via recordPacket() is lock-free and allocation-free for CoreMIDI callbacks;
 * drainAndRefreshUi() runs on the main/UI thread to update ring views, voice state, and Cocoa UI.
 */
class MIDIMonitor {
public:
    enum class Port : uint8_t {
        TilesDaw = 0,
        TilesMpe = 1,
        LogicMcu = 2,
        LogicGrid = 3,
        LogicDisplay = 4,
        LogicMpe = 5,
    };

    enum class Direction : uint8_t {
        RxFromHardware = 0,
        TxToHardware = 1,
        RxFromLogic = 2,
        TxToLogic = 3,
    };

    enum class EventKind : uint8_t {
        NoteOn = 0,
        NoteOff = 1,
        PitchBend = 2,
        ChannelPressure = 3,
        PolyAftertouch = 4,
        SlideCC74 = 5,
        MpeZoneRpn = 6,
        FocusExprCC = 7,
        VoiceExprCC = 8,
        ControlChange = 9,
        SysEx = 10,
        Realtime = 11,
        Other = 12,
    };

    enum class FilterMode : uint8_t {
        AllTraffic = 0,
        MpeOnly = 1,
        IncomingOnly = 2,
        OutgoingOnly = 3,
    };

    struct MonitorEvent {
        uint64_t sequence = 0;
        uint64_t timestamp_ms = 0;
        Port port = Port::TilesMpe;
        Direction direction = Direction::RxFromHardware;
        EventKind kind = EventKind::Other;
        bool incoming = true;
        bool is_mpe = false;
        uint8_t channel_1based = 0; /* 1..16, 0 for system/SysEx */
        uint8_t data1 = 0;
        uint8_t data2 = 0;
        int16_t pitch_bend_signed = 0; /* -8192..+8191 */
        uint8_t length = 0;
        std::array<uint8_t, 24> bytes{};
    };

    struct MpeChannelState {
        bool gate_active = false;
        uint8_t active_note = 0;
        uint8_t velocity = 0;
        uint8_t pressure = 0;
        uint8_t slide_cc74 = 64;
        int16_t pitch_bend = 0;
        uint64_t event_count = 0;
    };

    struct Counters {
        uint64_t total_recorded = 0;
        uint64_t incoming_count = 0;
        uint64_t outgoing_count = 0;
        uint64_t mpe_count = 0;
        uint64_t dropped_count = 0;
    };

    static constexpr size_t kRingCapacity = 1024;
    static constexpr size_t kHistoryCapacity = 400;

    MIDIMonitor();
    ~MIDIMonitor();

    MIDIMonitor(const MIDIMonitor&) = delete;
    MIDIMonitor& operator=(const MIDIMonitor&) = delete;

    /* Opens/closes the graphical Cocoa monitor window (returns true cleanly in headless non-Apple builds). */
    bool openWindow();
    void closeWindow();
    bool isWindowOpen() const noexcept { return window_open_; }

    /* Non-allocating enqueue safe from CoreMIDI real-time read callbacks. */
    void recordPacket(
        uint64_t timestamp_ms,
        Port port,
        Direction direction,
        const uint8_t* bytes,
        size_t len
    ) noexcept;

    /* Drains queued packets on UI/consumer thread, updates voice state & UI window. */
    size_t drainAndRefreshUi();

    void setPaused(bool paused) noexcept;
    bool isPaused() const noexcept { return paused_.load(std::memory_order_acquire); }

    void setFilterMode(FilterMode mode) noexcept;
    FilterMode filterMode() const noexcept { return filter_mode_.load(std::memory_order_acquire); }

    void clear();

    /* Thread-safe / consumer-thread inspection helpers. */
    Counters counters() const noexcept;
    MpeChannelState channelState(uint8_t channel_1based) const noexcept;
    const std::deque<MonitorEvent>& recentEvents() const noexcept { return history_; }

    static bool isIncomingDirection(Direction dir) noexcept;
    static bool isMpeMessage(Port port, const uint8_t* bytes, size_t len) noexcept;
    static EventKind classifyEvent(Port port, const uint8_t* bytes, size_t len) noexcept;
    static std::string formatEventLine(const MonitorEvent& ev);
    static std::string formatVoiceSummary(const std::array<MpeChannelState, 16>& channels);

private:
    void applyChannelTelemetry(const MonitorEvent& ev) noexcept;
    bool matchesFilter(const MonitorEvent& ev) const noexcept;
    void refreshNativeWindow(const std::vector<MonitorEvent>& batch);

    std::atomic<bool> accepting_{true};
    std::atomic<bool> paused_{false};
    std::atomic<FilterMode> filter_mode_{FilterMode::AllTraffic};

    std::atomic<size_t> write_idx_{0};
    std::atomic<size_t> read_idx_{0};
    std::atomic<uint64_t> sequence_gen_{0};

    std::atomic<uint64_t> total_recorded_{0};
    std::atomic<uint64_t> incoming_count_{0};
    std::atomic<uint64_t> outgoing_count_{0};
    std::atomic<uint64_t> mpe_count_{0};
    std::atomic<uint64_t> dropped_count_{0};

    std::array<MonitorEvent, kRingCapacity> ring_{};
    std::deque<MonitorEvent> history_;
    std::array<MpeChannelState, 16> channels_{};

    bool window_open_ = false;
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace sentia::logic
