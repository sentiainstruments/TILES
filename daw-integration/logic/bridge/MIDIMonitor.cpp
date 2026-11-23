#include "MIDIMonitor.hpp"

#include <cstdio>
#include <cstring>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>
#endif

namespace sentia::logic {

namespace {

int16_t decodePitchBend14(uint8_t lsb, uint8_t msb) noexcept {
    const int32_t raw = static_cast<int32_t>(
        (static_cast<uint16_t>(msb & 0x7Fu) << 7u) | static_cast<uint16_t>(lsb & 0x7Fu)
    );
    return static_cast<int16_t>(raw - 8192);
}

const char* portShortLabel(MIDIMonitor::Port port) noexcept {
    switch (port) {
        case MIDIMonitor::Port::TilesDaw:
            return "TILES-DAW ";
        case MIDIMonitor::Port::TilesMpe:
            return "TILES-MPE ";
        case MIDIMonitor::Port::LogicMcu:
            return "LOGIC-MCU ";
        case MIDIMonitor::Port::LogicGrid:
            return "LOGIC-GRID";
        case MIDIMonitor::Port::LogicDisplay:
            return "LOGIC-DISP";
        case MIDIMonitor::Port::LogicMpe:
            return "LOGIC-MPE ";
    }
    return "MIDI      ";
}

const char* directionTag(MIDIMonitor::Direction dir) noexcept {
    switch (dir) {
        case MIDIMonitor::Direction::RxFromHardware:
            return "IN  HW->BR";
        case MIDIMonitor::Direction::TxToHardware:
            return "OUT BR->HW";
        case MIDIMonitor::Direction::RxFromLogic:
            return "IN  LG->BR";
        case MIDIMonitor::Direction::TxToLogic:
            return "OUT BR->LG";
    }
    return "MIDI      ";
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

const char* mpeRoleTag(uint8_t ch1) noexcept {
    if (ch1 == 1u) return "Mstr";
    if (ch1 >= 2u && ch1 <= 9u) return "MPE ";
    if (ch1 == 16u) return "Foc ";
    if (ch1 >= 10u && ch1 <= 15u) return "Ch  ";
    return "Sys ";
}

std::string decodeEventPayload(const MIDIMonitor::MonitorEvent& ev) {
    char buf[160];
    switch (ev.kind) {
        case MIDIMonitor::EventKind::NoteOn:
            std::snprintf(
                buf, sizeof(buf),
                "NoteOn   note=%-3u vel=%-3u",
                static_cast<unsigned>(ev.data1),
                static_cast<unsigned>(ev.data2)
            );
            return std::string(buf);
        case MIDIMonitor::EventKind::NoteOff:
            std::snprintf(
                buf, sizeof(buf),
                "NoteOff  note=%-3u vel=%-3u",
                static_cast<unsigned>(ev.data1),
                static_cast<unsigned>(ev.data2)
            );
            return std::string(buf);
        case MIDIMonitor::EventKind::PitchBend:
            std::snprintf(
                buf, sizeof(buf),
                "TiltBend bend=%+6d",
                static_cast<int>(ev.pitch_bend_signed)
            );
            return std::string(buf);
        case MIDIMonitor::EventKind::ChannelPressure:
            std::snprintf(
                buf, sizeof(buf),
                "Press    pressure=%-3u",
                static_cast<unsigned>(ev.data1)
            );
            return std::string(buf);
        case MIDIMonitor::EventKind::PolyAftertouch:
            std::snprintf(
                buf, sizeof(buf),
                "PolyAT   note=%-3u val=%-3u",
                static_cast<unsigned>(ev.data1),
                static_cast<unsigned>(ev.data2)
            );
            return std::string(buf);
        case MIDIMonitor::EventKind::SlideCC74:
            std::snprintf(
                buf, sizeof(buf),
                "Slide    CC74=%-3u",
                static_cast<unsigned>(ev.data2)
            );
            return std::string(buf);
        case MIDIMonitor::EventKind::MpeZoneRpn:
            std::snprintf(
                buf, sizeof(buf),
                "MPE_Zone CC=%-3u val=%-3u",
                static_cast<unsigned>(ev.data1),
                static_cast<unsigned>(ev.data2)
            );
            return std::string(buf);
        case MIDIMonitor::EventKind::FocusExprCC: {
            const char* axis = (ev.data1 == TILES_EXPR_CC_STRIKE) ? "Strike"
                             : (ev.data1 == TILES_EXPR_CC_PRESS) ? "Press "
                             : (ev.data1 == TILES_EXPR_CC_TILT) ? "Tilt  "
                             : "Touch ";
            std::snprintf(
                buf, sizeof(buf),
                "Focus%s CC%u=%-3u",
                axis,
                static_cast<unsigned>(ev.data1),
                static_cast<unsigned>(ev.data2)
            );
            return std::string(buf);
        }
        case MIDIMonitor::EventKind::VoiceExprCC: {
            const char* axis = (ev.data1 == TILES_EXPR_VOICE_CC_STRIKE) ? "Strike"
                             : (ev.data1 == TILES_EXPR_VOICE_CC_PRESS) ? "Press "
                             : (ev.data1 == TILES_EXPR_VOICE_CC_TILT) ? "Tilt  "
                             : "Touch ";
            std::snprintf(
                buf, sizeof(buf),
                "Voice%s CC%u=%-3u",
                axis,
                static_cast<unsigned>(ev.data1),
                static_cast<unsigned>(ev.data2)
            );
            return std::string(buf);
        }
        case MIDIMonitor::EventKind::ControlChange:
            std::snprintf(
                buf, sizeof(buf),
                "CC       cc=%-3u val=%-3u",
                static_cast<unsigned>(ev.data1),
                static_cast<unsigned>(ev.data2)
            );
            return std::string(buf);
        case MIDIMonitor::EventKind::SysEx:
            if (ev.length >= 9 && ev.bytes[1] == TILES_SYSEX_MFR_ID && ev.bytes[2] == TILES_SYSEX_SUB_ID) {
                if (ev.bytes[3] == TILES_MSG_CLIP_STATE) {
                    std::snprintf(
                        buf, sizeof(buf),
                        "SysEx    CLIP_STATE pad=%u flags=0x%02X",
                        static_cast<unsigned>(ev.bytes[4]),
                        static_cast<unsigned>(ev.bytes[8])
                    );
                    return std::string(buf);
                }
                if (ev.bytes[3] == TILES_MSG_SCENE_STATE) {
                    std::snprintf(
                        buf, sizeof(buf),
                        "SysEx    SCENE_STATE scene=%u flags=0x%02X",
                        static_cast<unsigned>(ev.bytes[4]),
                        static_cast<unsigned>(ev.bytes[8])
                    );
                    return std::string(buf);
                }
            }
            return "SysEx    frame";
        case MIDIMonitor::EventKind::Realtime:
            std::snprintf(buf, sizeof(buf), "Realtime byte=0x%02X", static_cast<unsigned>(ev.bytes[0]));
            return std::string(buf);
        case MIDIMonitor::EventKind::Other:
            std::snprintf(buf, sizeof(buf), "MIDI     status=0x%02X", static_cast<unsigned>(ev.bytes[0]));
            return std::string(buf);
    }
    return "MIDI";
}

} // namespace

struct MIDIMonitor::Impl {
#ifdef __APPLE__
    id window = nullptr;
    id content_view = nullptr;
    id stats_label = nullptr;
    id voice_strip = nullptr;
    id filter_popup = nullptr;
    id pause_button = nullptr;
    id clear_button = nullptr;
    id scroll_view = nullptr;
    id text_view = nullptr;
    id delegate_target = nullptr;
    size_t displayed_lines = 0;
#else
    int unused = 0;
#endif
};

#ifdef __APPLE__
namespace {

MIDIMonitor* g_active_monitor = nullptr;

template <typename Ret, typename... Args>
Ret callObjc(id obj, const char* sel_name, Args... args) {
    using Fn = Ret (*)(id, SEL, Args...);
    Fn fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(objc_msgSend));
    return fn(obj, sel_registerName(sel_name), args...);
}

id nsString(const char* utf8) {
    const id cls = reinterpret_cast<id>(objc_getClass("NSString"));
    return callObjc<id, const char*>(cls, "stringWithUTF8String:", utf8 ? utf8 : "");
}

id makeColor(double r, double g, double b, double a = 1.0) {
    const id cls = reinterpret_cast<id>(objc_getClass("NSColor"));
    return callObjc<id, double, double, double, double>(
        cls,
        "colorWithSRGBRed:green:blue:alpha:",
        r, g, b, a
    );
}

id makeMonoFont(double size) {
    const id font_cls = reinterpret_cast<id>(objc_getClass("NSFont"));
    id font = callObjc<id, double>(font_cls, "userFixedPitchFontOfSize:", size);
    if (!font) {
        font = callObjc<id, id, double>(font_cls, "fontWithName:size:", nsString("Menlo"), size);
    }
    return font;
}

id makeDict2(id k1, id v1, id k2, id v2) {
    const id cls = reinterpret_cast<id>(objc_getClass("NSDictionary"));
    id objects[2] = {v1, v2};
    id keys[2] = {k1, k2};
    return callObjc<id, const id*, const id*, unsigned long>(
        cls,
        "dictionaryWithObjects:forKeys:count:",
        objects,
        keys,
        2UL
    );
}

id makeLabel(CGRect frame, const char* text, double font_size, bool mono, id color) {
    const id cls = reinterpret_cast<id>(objc_getClass("NSTextField"));
    id field = callObjc<id, CGRect>(callObjc<id>(cls, "alloc"), "initWithFrame:", frame);
    callObjc<void, id>(field, "setStringValue:", nsString(text));
    callObjc<void, bool>(field, "setEditable:", false);
    callObjc<void, bool>(field, "setSelectable:", true);
    callObjc<void, bool>(field, "setBezeled:", false);
    callObjc<void, bool>(field, "setDrawsBackground:", false);
    if (color) {
        callObjc<void, id>(field, "setTextColor:", color);
    }
    if (mono) {
        id f = makeMonoFont(font_size);
        if (f) callObjc<void, id>(field, "setFont:", f);
    } else {
        id f = callObjc<id, double>(reinterpret_cast<id>(objc_getClass("NSFont")), "systemFontOfSize:", font_size);
        if (f) callObjc<void, id>(field, "setFont:", f);
    }
    return field;
}

void objcPauseAction(id self, SEL cmd, id sender) {
    (void)self;
    (void)cmd;
    (void)sender;
    if (g_active_monitor) {
        g_active_monitor->setPaused(!g_active_monitor->isPaused());
    }
}

void objcClearAction(id self, SEL cmd, id sender) {
    (void)self;
    (void)cmd;
    (void)sender;
    if (g_active_monitor) {
        g_active_monitor->clear();
    }
}

void objcFilterChangedAction(id self, SEL cmd, id sender) {
    (void)self;
    (void)cmd;
    if (!g_active_monitor || !sender) return;
    const long idx = callObjc<long>(sender, "indexOfSelectedItem");
    MIDIMonitor::FilterMode mode = MIDIMonitor::FilterMode::AllTraffic;
    if (idx == 1) mode = MIDIMonitor::FilterMode::MpeOnly;
    else if (idx == 2) mode = MIDIMonitor::FilterMode::IncomingOnly;
    else if (idx == 3) mode = MIDIMonitor::FilterMode::OutgoingOnly;
    g_active_monitor->setFilterMode(mode);
}

void objcWindowWillClose(id self, SEL cmd, id notification) {
    (void)self;
    (void)cmd;
    (void)notification;
    if (g_active_monitor) {
        g_active_monitor->closeWindow();
    }
}

Class ensureMonitorTargetClass() {
    Class existing = objc_getClass("TilesMidiMonitorDelegate");
    if (existing) return existing;

    Class cls = objc_allocateClassPair(objc_getClass("NSObject"), "TilesMidiMonitorDelegate", 0);
    if (!cls) return nullptr;

    class_addMethod(cls, sel_registerName("onTogglePause:"), reinterpret_cast<IMP>(objcPauseAction), "v@:@");
    class_addMethod(cls, sel_registerName("onClearMonitor:"), reinterpret_cast<IMP>(objcClearAction), "v@:@");
    class_addMethod(cls, sel_registerName("onFilterChanged:"), reinterpret_cast<IMP>(objcFilterChangedAction), "v@:@");
    class_addMethod(cls, sel_registerName("windowWillClose:"), reinterpret_cast<IMP>(objcWindowWillClose), "v@:@");
    objc_registerClassPair(cls);
    return cls;
}

struct NSRangeStruct {
    unsigned long location;
    unsigned long length;
};

} // namespace
#endif

MIDIMonitor::MIDIMonitor() : impl_(new Impl()) {}

MIDIMonitor::~MIDIMonitor() {
    closeWindow();
    delete impl_;
    impl_ = nullptr;
}

bool MIDIMonitor::isIncomingDirection(Direction dir) noexcept {
    return dir == Direction::RxFromHardware || dir == Direction::RxFromLogic;
}

bool MIDIMonitor::isMpeMessage(Port port, const uint8_t* bytes, size_t len) noexcept {
    if (!bytes || len == 0) return false;
    if (port == Port::TilesMpe || port == Port::LogicMpe) return true;

    const uint8_t st = bytes[0];
    if (st >= 0xF0u) return false;
    const uint8_t type = st & 0xF0u;
    const uint8_t ch1 = static_cast<uint8_t>((st & 0x0Fu) + 1u);

    if (type == 0xD0u || type == 0xE0u) return true;
    if (type == 0xB0u && len >= 3) {
        const uint8_t cc = static_cast<uint8_t>(bytes[1] & 0x7Fu);
        if (cc == 74u) return true;
        if (ch1 == 1u && (cc == 100u || cc == 101u || cc == 6u)) return true;
        if (ch1 == 16u && cc >= TILES_EXPR_CC_STRIKE && cc <= TILES_EXPR_CC_TOUCH) return true;
        if (ch1 >= 2u && ch1 <= 9u && cc >= TILES_EXPR_VOICE_CC_STRIKE && cc <= TILES_EXPR_VOICE_CC_TOUCH) {
            return true;
        }
    }
    return false;
}

MIDIMonitor::EventKind MIDIMonitor::classifyEvent(Port port, const uint8_t* bytes, size_t len) noexcept {
    (void)port;
    if (!bytes || len == 0) return EventKind::Other;

    const uint8_t st = bytes[0];
    if (st == 0xF0u) return EventKind::SysEx;
    if (st >= 0xF8u) return EventKind::Realtime;
    if (st >= 0xF0u) return EventKind::Other;

    const uint8_t type = st & 0xF0u;
    const uint8_t ch1 = static_cast<uint8_t>((st & 0x0Fu) + 1u);

    if (type == 0x90u && len >= 3) {
        return (bytes[2] & 0x7Fu) == 0u ? EventKind::NoteOff : EventKind::NoteOn;
    }
    if (type == 0x80u && len >= 3) return EventKind::NoteOff;
    if (type == 0xE0u && len >= 3) return EventKind::PitchBend;
    if (type == 0xD0u && len >= 2) return EventKind::ChannelPressure;
    if (type == 0xA0u && len >= 3) return EventKind::PolyAftertouch;
    if (type == 0xB0u && len >= 3) {
        const uint8_t cc = static_cast<uint8_t>(bytes[1] & 0x7Fu);
        if (cc == 74u) return EventKind::SlideCC74;
        if (ch1 == 1u && (cc == 100u || cc == 101u || cc == 6u)) return EventKind::MpeZoneRpn;
        if (ch1 == 16u && cc >= TILES_EXPR_CC_STRIKE && cc <= TILES_EXPR_CC_TOUCH) {
            return EventKind::FocusExprCC;
        }
        if (ch1 >= 2u && ch1 <= 9u && cc >= TILES_EXPR_VOICE_CC_STRIKE && cc <= TILES_EXPR_VOICE_CC_TOUCH) {
            return EventKind::VoiceExprCC;
        }
        return EventKind::ControlChange;
    }
    return EventKind::Other;
}

void MIDIMonitor::recordPacket(
    uint64_t timestamp_ms,
    Port port,
    Direction direction,
    const uint8_t* bytes,
    size_t len
) noexcept {
    if (!accepting_.load(std::memory_order_acquire) || !bytes || len == 0) {
        return;
    }

    const size_t w = write_idx_.load(std::memory_order_relaxed);
    const size_t next_w = (w + 1u) % kRingCapacity;
    if (next_w == read_idx_.load(std::memory_order_acquire)) {
        dropped_count_.fetch_add(1u, std::memory_order_relaxed);
        return;
    }

    MonitorEvent& slot = ring_[w];
    slot.sequence = sequence_gen_.fetch_add(1u, std::memory_order_relaxed) + 1u;
    slot.timestamp_ms = timestamp_ms;
    slot.port = port;
    slot.direction = direction;
    slot.kind = classifyEvent(port, bytes, len);
    slot.incoming = isIncomingDirection(direction);
    slot.is_mpe = isMpeMessage(port, bytes, len);

    const uint8_t st = bytes[0];
    slot.channel_1based = (st >= 0x80u && st < 0xF0u) ? static_cast<uint8_t>((st & 0x0Fu) + 1u) : 0u;
    slot.data1 = (len >= 2) ? static_cast<uint8_t>(bytes[1] & 0x7Fu) : 0u;
    slot.data2 = (len >= 3) ? static_cast<uint8_t>(bytes[2] & 0x7Fu) : 0u;
    slot.pitch_bend_signed = (slot.kind == EventKind::PitchBend && len >= 3)
                             ? decodePitchBend14(bytes[1], bytes[2])
                             : 0;

    const uint8_t copy_len = static_cast<uint8_t>(len > slot.bytes.size() ? slot.bytes.size() : len);
    slot.length = copy_len;
    for (uint8_t i = 0; i < copy_len; ++i) {
        slot.bytes[i] = bytes[i];
    }

    write_idx_.store(next_w, std::memory_order_release);

    total_recorded_.fetch_add(1u, std::memory_order_relaxed);
    if (slot.incoming) {
        incoming_count_.fetch_add(1u, std::memory_order_relaxed);
    } else {
        outgoing_count_.fetch_add(1u, std::memory_order_relaxed);
    }
    if (slot.is_mpe) {
        mpe_count_.fetch_add(1u, std::memory_order_relaxed);
    }
}

void MIDIMonitor::applyChannelTelemetry(const MonitorEvent& ev) noexcept {
    if (ev.channel_1based < 1u || ev.channel_1based > 16u) return;
    MpeChannelState& ch = channels_[ev.channel_1based - 1u];
    ++ch.event_count;

    switch (ev.kind) {
        case EventKind::NoteOn:
            ch.gate_active = true;
            ch.active_note = ev.data1;
            ch.velocity = ev.data2;
            break;
        case EventKind::NoteOff:
            if (ch.active_note == ev.data1 || ch.active_note == 0u) {
                ch.gate_active = false;
                ch.pressure = 0u;
            }
            break;
        case EventKind::PitchBend:
            ch.pitch_bend = ev.pitch_bend_signed;
            break;
        case EventKind::ChannelPressure:
            ch.pressure = ev.data1;
            break;
        case EventKind::SlideCC74:
            ch.slide_cc74 = ev.data2;
            break;
        case EventKind::VoiceExprCC:
            if (ev.data1 == TILES_EXPR_VOICE_CC_PRESS) ch.pressure = ev.data2;
            else if (ev.data1 == TILES_EXPR_VOICE_CC_STRIKE) ch.velocity = ev.data2;
            break;
        default:
            break;
    }
}

bool MIDIMonitor::matchesFilter(const MonitorEvent& ev) const noexcept {
    switch (filter_mode_.load(std::memory_order_acquire)) {
        case FilterMode::AllTraffic:
            return true;
        case FilterMode::MpeOnly:
            return ev.is_mpe;
        case FilterMode::IncomingOnly:
            return ev.incoming;
        case FilterMode::OutgoingOnly:
            return !ev.incoming;
    }
    return true;
}

size_t MIDIMonitor::drainAndRefreshUi() {
    size_t drained = 0;
    size_t r = read_idx_.load(std::memory_order_relaxed);
    const size_t w = write_idx_.load(std::memory_order_acquire);
    const bool is_paused = paused_.load(std::memory_order_acquire);

    std::vector<MonitorEvent> ui_batch;
    while (r != w) {
        const MonitorEvent& ev = ring_[r];
        applyChannelTelemetry(ev);

        if (!is_paused && matchesFilter(ev)) {
            if (history_.size() >= kHistoryCapacity) {
                history_.pop_front();
            }
            history_.push_back(ev);
            ui_batch.push_back(ev);
        }

        r = (r + 1u) % kRingCapacity;
        read_idx_.store(r, std::memory_order_release);
        ++drained;
    }

    if (window_open_) {
        refreshNativeWindow(ui_batch);
    }
    return drained;
}

void MIDIMonitor::setPaused(bool paused) noexcept {
    paused_.store(paused, std::memory_order_release);
#ifdef __APPLE__
    if (window_open_ && impl_->pause_button) {
        callObjc<void, id>(impl_->pause_button, "setTitle:", nsString(paused ? "Resume" : "Pause"));
    }
#endif
}

void MIDIMonitor::setFilterMode(FilterMode mode) noexcept {
    filter_mode_.store(mode, std::memory_order_release);
}

void MIDIMonitor::clear() {
    const size_t w = write_idx_.load(std::memory_order_acquire);
    read_idx_.store(w, std::memory_order_release);
    history_.clear();
    channels_.fill(MpeChannelState{});
    total_recorded_.store(0u, std::memory_order_release);
    incoming_count_.store(0u, std::memory_order_release);
    outgoing_count_.store(0u, std::memory_order_release);
    mpe_count_.store(0u, std::memory_order_release);
    dropped_count_.store(0u, std::memory_order_release);

#ifdef __APPLE__
    if (window_open_ && impl_->text_view) {
        callObjc<void, id>(impl_->text_view, "setString:", nsString(""));
        impl_->displayed_lines = 0;
        refreshNativeWindow({});
    }
#endif
}

MIDIMonitor::Counters MIDIMonitor::counters() const noexcept {
    Counters out;
    out.total_recorded = total_recorded_.load(std::memory_order_acquire);
    out.incoming_count = incoming_count_.load(std::memory_order_acquire);
    out.outgoing_count = outgoing_count_.load(std::memory_order_acquire);
    out.mpe_count = mpe_count_.load(std::memory_order_acquire);
    out.dropped_count = dropped_count_.load(std::memory_order_acquire);
    return out;
}

MIDIMonitor::MpeChannelState MIDIMonitor::channelState(uint8_t channel_1based) const noexcept {
    if (channel_1based < 1u || channel_1based > 16u) return MpeChannelState{};
    return channels_[channel_1based - 1u];
}

std::string MIDIMonitor::formatEventLine(const MonitorEvent& ev) {
    char ch_buf[16];
    if (ev.channel_1based > 0u) {
        std::snprintf(ch_buf, sizeof(ch_buf), "Ch%-2u[%s]", static_cast<unsigned>(ev.channel_1based), mpeRoleTag(ev.channel_1based));
    } else {
        std::snprintf(ch_buf, sizeof(ch_buf), "Sys     ");
    }

    char head[96];
    std::snprintf(
        head,
        sizeof(head),
        "%08llu | %-10s | %-10s | %-9s | ",
        static_cast<unsigned long long>(ev.timestamp_ms),
        directionTag(ev.direction),
        portShortLabel(ev.port),
        ch_buf
    );

    const std::string payload = decodeEventPayload(ev);
    const std::string hex = formatHexBytes(ev.bytes.data(), ev.length);
    return std::string(head) + payload + " | " + hex;
}

std::string MIDIMonitor::formatVoiceSummary(const std::array<MpeChannelState, 16>& channels) {
    std::string out = "MPE Voices  ";
    char cell[48];
    for (uint8_t ch1 = 2u; ch1 <= 9u; ++ch1) {
        const MpeChannelState& s = channels[ch1 - 1u];
        if (s.gate_active) {
            std::snprintf(
                cell, sizeof(cell),
                "[C%u n%-3u p%-3u b%+d] ",
                static_cast<unsigned>(ch1),
                static_cast<unsigned>(s.active_note),
                static_cast<unsigned>(s.pressure),
                static_cast<int>(s.pitch_bend)
            );
        } else {
            std::snprintf(cell, sizeof(cell), "[C%u idle] ", static_cast<unsigned>(ch1));
        }
        out += cell;
    }
    return out;
}

bool MIDIMonitor::openWindow() {
    if (window_open_) {
#ifdef __APPLE__
        if (impl_->window) {
            callObjc<void, id>(impl_->window, "makeKeyAndOrderFront:", static_cast<id>(nullptr));
        }
#endif
        return true;
    }

#ifdef __APPLE__
    const id nsapp_cls = reinterpret_cast<id>(objc_getClass("NSApplication"));
    const id win_cls = reinterpret_cast<id>(objc_getClass("NSWindow"));
    const id btn_cls = reinterpret_cast<id>(objc_getClass("NSButton"));
    const id popup_cls = reinterpret_cast<id>(objc_getClass("NSPopUpButton"));
    const id scroll_cls = reinterpret_cast<id>(objc_getClass("NSScrollView"));
    const id textview_cls = reinterpret_cast<id>(objc_getClass("NSTextView"));

    if (!nsapp_cls || !win_cls || !btn_cls || !popup_cls || !scroll_cls || !textview_cls) {
        window_open_ = true;
        return false;
    }

    const id app = callObjc<id>(nsapp_cls, "sharedApplication");
    if (!app) return false;
    callObjc<bool, long>(app, "setActivationPolicy:", 1L);
    callObjc<void>(app, "finishLaunching");

    /* Style mask: Titled (1) | Closable (2) | Miniaturizable (4) | Resizable (8) = 15 */
    const CGRect win_rect = {{120.0, 140.0}, {920.0, 580.0}};
    impl_->window = callObjc<id, CGRect, unsigned long, unsigned long, bool>(
        callObjc<id>(win_cls, "alloc"),
        "initWithContentRect:styleMask:backing:defer:",
        win_rect,
        15UL,
        2UL, /* NSBackingStoreBuffered */
        false
    );
    if (!impl_->window) return false;

    callObjc<void, id>(impl_->window, "setTitle:", nsString("SENTIA TILES \u2014 Real-Time MPE MIDI Monitor"));
    callObjc<void, bool>(impl_->window, "setReleasedWhenClosed:", false);
    callObjc<void, id>(impl_->window, "setBackgroundColor:", makeColor(0.055, 0.055, 0.078));

    const id appearance_cls = reinterpret_cast<id>(objc_getClass("NSAppearance"));
    if (appearance_cls) {
        const id dark = callObjc<id, id>(appearance_cls, "appearanceNamed:", nsString("NSAppearanceNameDarkAqua"));
        if (dark) callObjc<void, id>(impl_->window, "setAppearance:", dark);
    }

    Class target_cls = ensureMonitorTargetClass();
    if (target_cls) {
        impl_->delegate_target = callObjc<id>(callObjc<id>(reinterpret_cast<id>(target_cls), "alloc"), "init");
        callObjc<void, id>(impl_->window, "setDelegate:", impl_->delegate_target);
    }

    impl_->content_view = callObjc<id>(impl_->window, "contentView");

    impl_->stats_label = makeLabel(
        {{16.0, 538.0}, {460.0, 24.0}},
        "IN: 0  |  OUT: 0  |  MPE: 0  |  Zone: Lower (Ch 1 + Ch 2..9)",
        12.0,
        true,
        makeColor(0.93, 0.94, 0.97)
    );
    callObjc<void, unsigned long>(impl_->stats_label, "setAutoresizingMask:", 8UL | 2UL); /* MinY | Width */
    callObjc<void, id>(impl_->content_view, "addSubview:", impl_->stats_label);

    impl_->filter_popup = callObjc<id, CGRect, bool>(
        callObjc<id>(popup_cls, "alloc"),
        "initWithFrame:pullsDown:",
        CGRect{{490.0, 536.0}, {170.0, 26.0}},
        false
    );
    callObjc<void, id>(impl_->filter_popup, "addItemWithTitle:", nsString("All Traffic"));
    callObjc<void, id>(impl_->filter_popup, "addItemWithTitle:", nsString("MPE Stream Only"));
    callObjc<void, id>(impl_->filter_popup, "addItemWithTitle:", nsString("Incoming Only"));
    callObjc<void, id>(impl_->filter_popup, "addItemWithTitle:", nsString("Outgoing Only"));
    if (impl_->delegate_target) {
        callObjc<void, id>(impl_->filter_popup, "setTarget:", impl_->delegate_target);
        callObjc<void, SEL>(impl_->filter_popup, "setAction:", sel_registerName("onFilterChanged:"));
    }
    callObjc<void, unsigned long>(impl_->filter_popup, "setAutoresizingMask:", 8UL | 1UL); /* MinY | MinX */
    callObjc<void, id>(impl_->content_view, "addSubview:", impl_->filter_popup);

    impl_->pause_button = callObjc<id, CGRect>(callObjc<id>(btn_cls, "alloc"), "initWithFrame:", CGRect{{674.0, 535.0}, {108.0, 28.0}});
    callObjc<void, id>(impl_->pause_button, "setTitle:", nsString(isPaused() ? "Resume" : "Pause"));
    callObjc<void, unsigned long>(impl_->pause_button, "setBezelStyle:", 1UL);
    if (impl_->delegate_target) {
        callObjc<void, id>(impl_->pause_button, "setTarget:", impl_->delegate_target);
        callObjc<void, SEL>(impl_->pause_button, "setAction:", sel_registerName("onTogglePause:"));
    }
    callObjc<void, unsigned long>(impl_->pause_button, "setAutoresizingMask:", 8UL | 1UL);
    callObjc<void, id>(impl_->content_view, "addSubview:", impl_->pause_button);

    impl_->clear_button = callObjc<id, CGRect>(callObjc<id>(btn_cls, "alloc"), "initWithFrame:", CGRect{{792.0, 535.0}, {112.0, 28.0}});
    callObjc<void, id>(impl_->clear_button, "setTitle:", nsString("Clear"));
    callObjc<void, unsigned long>(impl_->clear_button, "setBezelStyle:", 1UL);
    if (impl_->delegate_target) {
        callObjc<void, id>(impl_->clear_button, "setTarget:", impl_->delegate_target);
        callObjc<void, SEL>(impl_->clear_button, "setAction:", sel_registerName("onClearMonitor:"));
    }
    callObjc<void, unsigned long>(impl_->clear_button, "setAutoresizingMask:", 8UL | 1UL);
    callObjc<void, id>(impl_->content_view, "addSubview:", impl_->clear_button);

    impl_->voice_strip = makeLabel(
        {{16.0, 488.0}, {888.0, 38.0}},
        formatVoiceSummary(channels_).c_str(),
        10.5,
        true,
        makeColor(0.67, 0.72, 0.82)
    );
    callObjc<void, unsigned long>(impl_->voice_strip, "setAutoresizingMask:", 8UL | 2UL);
    callObjc<void, id>(impl_->content_view, "addSubview:", impl_->voice_strip);

    impl_->scroll_view = callObjc<id, CGRect>(
        callObjc<id>(scroll_cls, "alloc"),
        "initWithFrame:",
        CGRect{{16.0, 14.0}, {888.0, 464.0}}
    );
    callObjc<void, bool>(impl_->scroll_view, "setHasVerticalScroller:", true);
    callObjc<void, bool>(impl_->scroll_view, "setHasHorizontalScroller:", true);
    callObjc<void, unsigned long>(impl_->scroll_view, "setBorderType:", 2UL); /* NSBezelBorder */
    callObjc<void, unsigned long>(impl_->scroll_view, "setAutoresizingMask:", 2UL | 16UL); /* Width | Height */

    impl_->text_view = callObjc<id, CGRect>(
        callObjc<id>(textview_cls, "alloc"),
        "initWithFrame:",
        CGRect{{0.0, 0.0}, {888.0, 464.0}}
    );
    callObjc<void, bool>(impl_->text_view, "setEditable:", false);
    callObjc<void, bool>(impl_->text_view, "setSelectable:", true);
    callObjc<void, bool>(impl_->text_view, "setRichText:", true);
    callObjc<void, id>(impl_->text_view, "setBackgroundColor:", makeColor(0.04, 0.042, 0.062));
    id mono_font = makeMonoFont(11.5);
    if (mono_font) {
        callObjc<void, id>(impl_->text_view, "setFont:", mono_font);
    }
    callObjc<void, bool>(impl_->text_view, "setVerticallyResizable:", true);
    callObjc<void, bool>(impl_->text_view, "setHorizontallyResizable:", true);
    callObjc<void, id>(impl_->scroll_view, "setDocumentView:", impl_->text_view);
    callObjc<void, id>(impl_->content_view, "addSubview:", impl_->scroll_view);

    g_active_monitor = this;
    window_open_ = true;

    callObjc<void, id>(impl_->window, "makeKeyAndOrderFront:", static_cast<id>(nullptr));
    refreshNativeWindow({});
    return true;
#else
    window_open_ = true;
    return true;
#endif
}

void MIDIMonitor::closeWindow() {
    if (!window_open_) return;
    window_open_ = false;

#ifdef __APPLE__
    if (g_active_monitor == this) {
        g_active_monitor = nullptr;
    }
    if (impl_->window) {
        callObjc<void, id>(impl_->window, "setDelegate:", static_cast<id>(nullptr));
        callObjc<void, id>(impl_->window, "orderOut:", static_cast<id>(nullptr));
        callObjc<void>(impl_->window, "release");
        impl_->window = nullptr;
    }
    if (impl_->stats_label) {
        callObjc<void>(impl_->stats_label, "release");
        impl_->stats_label = nullptr;
    }
    if (impl_->voice_strip) {
        callObjc<void>(impl_->voice_strip, "release");
        impl_->voice_strip = nullptr;
    }
    if (impl_->filter_popup) {
        callObjc<void>(impl_->filter_popup, "release");
        impl_->filter_popup = nullptr;
    }
    if (impl_->pause_button) {
        callObjc<void>(impl_->pause_button, "release");
        impl_->pause_button = nullptr;
    }
    if (impl_->clear_button) {
        callObjc<void>(impl_->clear_button, "release");
        impl_->clear_button = nullptr;
    }
    if (impl_->text_view) {
        callObjc<void>(impl_->text_view, "release");
        impl_->text_view = nullptr;
    }
    if (impl_->scroll_view) {
        callObjc<void>(impl_->scroll_view, "release");
        impl_->scroll_view = nullptr;
    }
    if (impl_->delegate_target) {
        callObjc<void>(impl_->delegate_target, "release");
        impl_->delegate_target = nullptr;
    }
    impl_->content_view = nullptr;
    impl_->displayed_lines = 0;
#endif
}

void MIDIMonitor::refreshNativeWindow(const std::vector<MonitorEvent>& batch) {
#ifdef __APPLE__
    if (!window_open_ || !impl_->window) return;

    const Counters stats = counters();
    char header[160];
    std::snprintf(
        header,
        sizeof(header),
        "IN: %llu  |  OUT: %llu  |  MPE: %llu  |  Drop: %llu  |  Zone: Lower (Ch 1 + Ch 2..9)",
        static_cast<unsigned long long>(stats.incoming_count),
        static_cast<unsigned long long>(stats.outgoing_count),
        static_cast<unsigned long long>(stats.mpe_count),
        static_cast<unsigned long long>(stats.dropped_count)
    );
    if (impl_->stats_label) {
        callObjc<void, id>(impl_->stats_label, "setStringValue:", nsString(header));
    }
    if (impl_->voice_strip) {
        callObjc<void, id>(impl_->voice_strip, "setStringValue:", nsString(formatVoiceSummary(channels_).c_str()));
    }

    if (batch.empty() || !impl_->text_view) return;

    const id storage = callObjc<id>(impl_->text_view, "textStorage");
    if (!storage) return;

    const id attr_cls = reinterpret_cast<id>(objc_getClass("NSAttributedString"));
    const id font = makeMonoFont(11.5);
    const id key_color = nsString("NSColor");
    const id key_font = nsString("NSFont");

    callObjc<void>(storage, "beginEditing");
    for (const MonitorEvent& ev : batch) {
        id row_color = nullptr;
        if (ev.incoming && ev.is_mpe) {
            row_color = makeColor(0.18, 0.83, 0.75); /* Teal IN MPE */
        } else if (!ev.incoming && ev.is_mpe) {
            row_color = makeColor(0.85, 0.27, 0.94); /* Sentia Magenta OUT MPE */
        } else if (ev.incoming) {
            row_color = makeColor(0.80, 0.84, 0.88); /* Slate IN DAW/Grid */
        } else {
            row_color = makeColor(0.96, 0.62, 0.04); /* Amber OUT Hardware/DAW */
        }

        std::string line = formatEventLine(ev);
        line += "\n";
        id attrs = makeDict2(key_color, row_color, key_font, font);
        id attr_str = callObjc<id, id, id>(
            callObjc<id>(attr_cls, "alloc"),
            "initWithString:attributes:",
            nsString(line.c_str()),
            attrs
        );
        callObjc<void, id>(storage, "appendAttributedString:", attr_str);
        callObjc<void>(attr_str, "release");
        ++impl_->displayed_lines;
    }

    if (impl_->displayed_lines > kHistoryCapacity) {
        const size_t excess_lines = impl_->displayed_lines - kHistoryCapacity;
        const id plain = callObjc<id>(storage, "string");
        const char* utf8 = callObjc<const char*>(plain, "UTF8String");
        if (utf8) {
            size_t cut_pos = 0;
            size_t seen = 0;
            while (utf8[cut_pos] != '\0' && seen < excess_lines) {
                if (utf8[cut_pos] == '\n') ++seen;
                ++cut_pos;
            }
            if (cut_pos > 0) {
                callObjc<void, NSRangeStruct>(storage, "deleteCharactersInRange:", NSRangeStruct{0UL, cut_pos});
                impl_->displayed_lines -= seen;
            }
        }
    }
    callObjc<void>(storage, "endEditing");

    const id plain_after = callObjc<id>(storage, "string");
    const unsigned long total_len = callObjc<unsigned long>(plain_after, "length");
    callObjc<void, NSRangeStruct>(impl_->text_view, "scrollRangeToVisible:", NSRangeStruct{total_len, 0UL});
#else
    (void)batch;
#endif
}

} // namespace sentia::logic
