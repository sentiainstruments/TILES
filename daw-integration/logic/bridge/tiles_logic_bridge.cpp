#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <signal.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "LogicManager.hpp"
#include "MIDILogger.hpp"
#include "MIDIMonitor.hpp"
#include "StudioLink.hpp"

namespace {

constexpr size_t kParserBufCap = 512;

class PrioMutex {
public:
    PrioMutex() {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
        pthread_mutex_init(&mutex_, &attr);
        pthread_mutexattr_destroy(&attr);
    }

    ~PrioMutex() {
        pthread_mutex_destroy(&mutex_);
    }

    PrioMutex(const PrioMutex&) = delete;
    PrioMutex& operator=(const PrioMutex&) = delete;

    void lock() { pthread_mutex_lock(&mutex_); }
    void unlock() { pthread_mutex_unlock(&mutex_); }

private:
    pthread_mutex_t mutex_{};
};

class ScopedLock {
public:
    explicit ScopedLock(PrioMutex& m) : mutex_(m) { mutex_.lock(); }
    ~ScopedLock() { mutex_.unlock(); }

    ScopedLock(const ScopedLock&) = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;

private:
    PrioMutex& mutex_;
};

struct MidiStreamParser {
    std::array<uint8_t, kParserBufCap> sysex{};
    size_t sysex_len = 0;
    bool in_sysex = false;
    std::array<uint8_t, 3> msg{};
    uint8_t msg_len = 0;
    uint8_t expected_len = 0;
    uint8_t running_status = 0;

    void reset() {
        sysex_len = 0;
        in_sysex = false;
        msg_len = 0;
        expected_len = 0;
        running_status = 0;
    }

    static uint8_t expectedChannelLen(uint8_t status) {
        switch (status & 0xF0u) {
            case 0xC0u:
            case 0xD0u:
                return 2;
            case 0x80u:
            case 0x90u:
            case 0xA0u:
            case 0xB0u:
            case 0xE0u:
                return 3;
            default:
                return 0;
        }
    }

    template <typename Callback>
    void feed(const uint8_t* data, size_t len, bool forward_clock, Callback&& on_msg) {
        for (size_t i = 0; i < len; ++i) {
            const uint8_t b = data[i];

            if (b >= 0xF8u) {
                /* Realtime bytes never interrupt SysEx or channel voice framing. */
                if (forward_clock && b == 0xF8u) {
                    on_msg(&b, 1);
                }
                continue;
            }

            if (b == 0xF0u) {
                in_sysex = true;
                sysex_len = 0;
                sysex[sysex_len++] = b;
                running_status = 0;
                continue;
            }

            if (in_sysex) {
                if (b == 0xF7u) {
                    if (sysex_len < kParserBufCap) {
                        sysex[sysex_len++] = b;
                        on_msg(sysex.data(), sysex_len);
                    }
                    in_sysex = false;
                    sysex_len = 0;
                    continue;
                }
                if ((b & 0x80u) != 0u) {
                    /* Unterminated SysEx aborted by status byte: recover cleanly. */
                    in_sysex = false;
                    sysex_len = 0;
                } else {
                    if (sysex_len < kParserBufCap - 1) {
                        sysex[sysex_len++] = b;
                    }
                    continue;
                }
            }

            if (b >= 0x80u) {
                if (b >= 0xF0u) {
                    running_status = 0;
                    msg_len = 0;
                    expected_len = 0;
                } else {
                    running_status = b;
                    msg[0] = b;
                    msg_len = 1;
                    expected_len = expectedChannelLen(b);
                }
                continue;
            }

            if (msg_len == 0 && running_status != 0) {
                msg[0] = running_status;
                msg_len = 1;
                expected_len = expectedChannelLen(running_status);
            }

            if (expected_len > 0 && msg_len < msg.size()) {
                msg[msg_len++] = b;
                if (msg_len == expected_len) {
                    on_msg(msg.data(), msg_len);
                    msg_len = 0;
                }
            }
        }
    }
};

enum class HwSourceKind : uintptr_t {
    Daw = 1,
    Mpe = 2,
};

enum class VirtDestKind : uintptr_t {
    Mcu = 1,
    Grid = 2,
    Display = 3,
    Mpe = 4,
};

struct BridgeApp {
    sentia::logic::StudioLink link;
    sentia::logic::LogicManager manager;
    sentia::logic::MIDILogger logger;
    sentia::logic::MIDIMonitor monitor;
    PrioMutex lock;
    std::atomic<uint64_t> scheduled_expiry_ms{0};
    std::atomic<bool> log_flush_pending{false};
    std::atomic<bool> monitor_flush_pending{false};

    MIDIClientRef client = 0;
    MIDIPortRef in_port = 0;
    MIDIPortRef out_port = 0;

    MIDIEndpointRef hw_daw_src = 0;
    MIDIEndpointRef hw_daw_dst = 0;
    MIDIEndpointRef hw_mpe_src = 0;
    MIDIEndpointRef hw_mpe_dst = 0;

    MIDIEndpointRef virt_mcu_src = 0;
    MIDIEndpointRef virt_mcu_dst = 0;
    MIDIEndpointRef virt_grid_src = 0;
    MIDIEndpointRef virt_grid_dst = 0;
    MIDIEndpointRef virt_display_dst = 0;
    MIDIEndpointRef virt_mpe_src = 0;
    MIDIEndpointRef virt_mpe_dst = 0;

    MidiStreamParser hw_daw_parser;
    MidiStreamParser hw_mpe_parser;
    MidiStreamParser mcu_parser;
    MidiStreamParser grid_parser;
    MidiStreamParser display_parser;
    MidiStreamParser logic_mpe_parser;

    uint8_t requested_unit = 0; /* 0 = auto, 1..4 = specific unit */
    bool verbose = false;
    mach_timebase_info_data_t timebase{};
};

BridgeApp* g_app_ptr = nullptr;

uint64_t monotonicMs(const BridgeApp& app) {
    const uint64_t abs_t = mach_absolute_time();
    const uint64_t ns = (abs_t * app.timebase.numer) / app.timebase.denom;
    return ns / 1000000ULL;
}

void sysexCompleteFree(MIDISysexSendRequest* req) {
    if (req) {
        std::free(const_cast<Byte*>(req->data));
        std::free(req);
    }
}

void sendBytesToDest(MIDIPortRef out_port, MIDIEndpointRef dest, const uint8_t* bytes, size_t len) {
    if (!dest || !bytes || len == 0) return;

    if (bytes[0] == 0xF0u) {
        auto* copy = static_cast<uint8_t*>(std::malloc(len));
        auto* req = static_cast<MIDISysexSendRequest*>(std::calloc(1, sizeof(MIDISysexSendRequest)));
        if (!copy || !req) {
            std::free(copy);
            std::free(req);
            return;
        }
        std::memcpy(copy, bytes, len);
        req->destination = dest;
        req->data = copy;
        req->bytesToSend = static_cast<UInt32>(len);
        req->complete = false;
        req->completionProc = sysexCompleteFree;
        req->completionRefCon = nullptr;
        if (MIDISendSysex(req) != noErr) {
            sysexCompleteFree(req);
        }
        return;
    }

    if (len > 256) return;
    union {
        MIDIPacketList list;
        uint8_t raw[sizeof(MIDIPacketList) + 256];
    } pkt_buf{};

    MIDIPacket* pkt = MIDIPacketListInit(&pkt_buf.list);
    pkt = MIDIPacketListAdd(&pkt_buf.list, sizeof(pkt_buf), pkt, 0, static_cast<ByteCount>(len), bytes);
    if (pkt) {
        MIDISend(out_port, dest, &pkt_buf.list);
    }
}

void sendBytesToVirtual(MIDIEndpointRef src, const uint8_t* bytes, size_t len) {
    if (!src || !bytes || len == 0 || len > 480) return;

    union {
        MIDIPacketList list;
        uint8_t raw[sizeof(MIDIPacketList) + 512];
    } pkt_buf{};

    MIDIPacket* pkt = MIDIPacketListInit(&pkt_buf.list);
    pkt = MIDIPacketListAdd(&pkt_buf.list, sizeof(pkt_buf), pkt, 0, static_cast<ByteCount>(len), bytes);
    if (pkt) {
        MIDIReceived(src, &pkt_buf.list);
    }
}

void scheduleLogFlush(BridgeApp& app) {
    if (!app.logger.isEnabled()) return;
    bool expected = false;
    if (!app.log_flush_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        BridgeApp* live = g_app_ptr;
        if (!live) return;
        live->logger.flush();
        live->log_flush_pending.store(false, std::memory_order_release);
    });
}

void scheduleMonitorFlush(BridgeApp& app) {
    if (!app.monitor.isWindowOpen()) return;
    bool expected = false;
    if (!app.monitor_flush_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        BridgeApp* live = g_app_ptr;
        if (!live) return;
        live->monitor_flush_pending.store(false, std::memory_order_release);
        live->monitor.drainAndRefreshUi();
    });
}

void bridgeEmitCb(tiles_out_target_t target, const uint8_t* bytes, size_t len, void* user_data) {
    auto* app = static_cast<BridgeApp*>(user_data);
    const uint64_t now_ms = monotonicMs(*app);
    if (target == TILES_OUT_TILES_DAW) {
        app->logger.log(now_ms, sentia::logic::MIDILogger::Port::TilesDaw, sentia::logic::MIDILogger::Direction::TxToHardware, bytes, len);
        app->monitor.recordPacket(now_ms, sentia::logic::MIDIMonitor::Port::TilesDaw, sentia::logic::MIDIMonitor::Direction::TxToHardware, bytes, len);
        sendBytesToDest(app->out_port, app->hw_daw_dst, bytes, len);
    } else if (target == TILES_OUT_LOGIC_MCU) {
        app->logger.log(now_ms, sentia::logic::MIDILogger::Port::LogicMcu, sentia::logic::MIDILogger::Direction::TxToLogic, bytes, len);
        app->monitor.recordPacket(now_ms, sentia::logic::MIDIMonitor::Port::LogicMcu, sentia::logic::MIDIMonitor::Direction::TxToLogic, bytes, len);
        sendBytesToVirtual(app->virt_mcu_src, bytes, len);
    } else if (target == TILES_OUT_LOGIC_GRID) {
        app->logger.log(now_ms, sentia::logic::MIDILogger::Port::LogicGrid, sentia::logic::MIDILogger::Direction::TxToLogic, bytes, len);
        app->monitor.recordPacket(now_ms, sentia::logic::MIDIMonitor::Port::LogicGrid, sentia::logic::MIDIMonitor::Direction::TxToLogic, bytes, len);
        sendBytesToVirtual(app->virt_grid_src, bytes, len);
    }
    scheduleLogFlush(*app);
    scheduleMonitorFlush(*app);
}

/* Schedules one-shot wakeups only when StudioLink has an active haptic pulse to expire. */
void scheduleHapticExpiryLocked(BridgeApp& app, uint64_t now_ms) {
    const uint64_t next_ms = app.link.nextExpiryMs();
    if (next_ms == 0) {
        app.scheduled_expiry_ms.store(0);
        return;
    }

    const uint64_t prev_scheduled = app.scheduled_expiry_ms.load();
    if (prev_scheduled != 0 && prev_scheduled <= next_ms) {
        return;
    }

    app.scheduled_expiry_ms.store(next_ms);
    const uint64_t delay_ms = (next_ms > now_ms) ? (next_ms - now_ms + 2u) : 2u;
    dispatch_after(
        dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(delay_ms) * NSEC_PER_MSEC),
        dispatch_get_main_queue(),
        ^{
            BridgeApp* live = g_app_ptr;
            if (!live) return;
            ScopedLock guard(live->lock);
            const uint64_t t = monotonicMs(*live);
            live->link.tick(t, bridgeEmitCb, live);
            live->scheduled_expiry_ms.store(0);
            scheduleHapticExpiryLocked(*live, t);
        }
    );
}

void syncUiViewAsync(const BridgeApp& app) {
    const tiles_logic_page_t page = app.link.core().page;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (g_app_ptr && g_app_ptr->manager.isUiAttached()) {
            g_app_ptr->manager.syncFromBridgePage(page);
        }
    });
}

void emitMpeLowerZoneConfig(MIDIEndpointRef src) {
    /* MPE Lower Zone Configuration Message: Master Ch 1, 8 member channels (Ch 2..9). */
    const uint8_t rpn_msb[3] = {0xB0u, 101u, 0u};
    const uint8_t rpn_lsb[3] = {0xB0u, 100u, 6u};
    const uint8_t data_msb[3] = {0xB0u, 6u, 8u};
    sendBytesToVirtual(src, rpn_msb, sizeof(rpn_msb));
    sendBytesToVirtual(src, rpn_lsb, sizeof(rpn_lsb));
    sendBytesToVirtual(src, data_msb, sizeof(data_msb));
}

bool endpointIsOffline(MIDIEndpointRef ep) {
    SInt32 offline = 0;
    if (MIDIObjectGetIntegerProperty(ep, kMIDIPropertyOffline, &offline) == noErr) {
        return offline != 0;
    }
    return false;
}

void endpointDisplayName(MIDIEndpointRef ep, char* out, size_t out_cap) {
    out[0] = '\0';
    CFStringRef name = nullptr;
    if (MIDIObjectGetStringProperty(ep, kMIDIPropertyDisplayName, &name) != noErr || !name) {
        if (MIDIObjectGetStringProperty(ep, kMIDIPropertyName, &name) != noErr || !name) {
            return;
        }
    }
    CFStringGetCString(name, out, static_cast<CFIndex>(out_cap), kCFStringEncodingUTF8);
    CFRelease(name);
}

bool matchTilesPortName(const char* name, const char* port_role, uint8_t requested_unit, uint8_t* found_unit) {
    if (!std::strstr(name, "SENTIA TILES")) return false;
    if (!std::strstr(name, port_role)) return false;

    uint8_t detected = 1;
    if (const char* p = std::strstr(name, "SENTIA TILES ")) {
        p += std::strlen("SENTIA TILES ");
        if (*p >= '1' && *p <= '4') {
            detected = static_cast<uint8_t>(*p - '0');
        }
    }
    if (requested_unit != 0 && detected != requested_unit) {
        return false;
    }
    if (found_unit) *found_unit = detected;
    return true;
}

void disconnectHardwareSources(BridgeApp& app) {
    if (app.hw_daw_src) {
        MIDIPortDisconnectSource(app.in_port, app.hw_daw_src);
        app.hw_daw_src = 0;
    }
    if (app.hw_mpe_src) {
        MIDIPortDisconnectSource(app.in_port, app.hw_mpe_src);
        app.hw_mpe_src = 0;
    }
    app.hw_daw_dst = 0;
    app.hw_mpe_dst = 0;
    app.hw_daw_parser.reset();
    app.hw_mpe_parser.reset();
    app.logic_mpe_parser.reset();
}

void scanAndConnectHardware(BridgeApp& app) {
    ScopedLock guard(app.lock);
    disconnectHardwareSources(app);

    const ItemCount num_src = MIDIGetNumberOfSources();
    for (ItemCount i = 0; i < num_src; ++i) {
        const MIDIEndpointRef ep = MIDIGetSource(i);
        if (endpointIsOffline(ep)) continue;

        char name[128];
        endpointDisplayName(ep, name, sizeof(name));
        uint8_t unit = 1;

        if (!app.hw_daw_src && matchTilesPortName(name, "DAW", app.requested_unit, &unit)) {
            if (MIDIPortConnectSource(app.in_port, ep, reinterpret_cast<void*>(static_cast<uintptr_t>(HwSourceKind::Daw))) == noErr) {
                app.hw_daw_src = ep;
                app.link.core().unit_number = unit;
                if (app.verbose) std::fprintf(stderr, "[tiles-logic] connected DAW input: %s (unit %u)\n", name, unit);
            }
        } else if (!app.hw_mpe_src && matchTilesPortName(name, "MIDI", app.requested_unit, &unit)) {
            if (MIDIPortConnectSource(app.in_port, ep, reinterpret_cast<void*>(static_cast<uintptr_t>(HwSourceKind::Mpe))) == noErr) {
                app.hw_mpe_src = ep;
                emitMpeLowerZoneConfig(app.virt_mpe_src);
                if (app.verbose) std::fprintf(stderr, "[tiles-logic] connected MPE input: %s\n", name);
            }
        }
    }

    const ItemCount num_dst = MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < num_dst; ++i) {
        const MIDIEndpointRef ep = MIDIGetDestination(i);
        if (endpointIsOffline(ep)) continue;

        char name[128];
        endpointDisplayName(ep, name, sizeof(name));
        if (!app.hw_daw_dst && matchTilesPortName(name, "DAW", app.requested_unit, nullptr)) {
            app.hw_daw_dst = ep;
            if (app.verbose) std::fprintf(stderr, "[tiles-logic] connected DAW output: %s\n", name);
        } else if (!app.hw_mpe_dst && matchTilesPortName(name, "MIDI", app.requested_unit, nullptr)) {
            app.hw_mpe_dst = ep;
            if (app.verbose) std::fprintf(stderr, "[tiles-logic] connected MPE output: %s\n", name);
        }
    }

    if (app.hw_daw_dst) {
        app.link.repaint(true, bridgeEmitCb, &app);
    }
}

void midiNotifyProc(const MIDINotification* message, void* refCon) {
    auto* app = static_cast<BridgeApp*>(refCon);
    if (app && message && message->messageID == kMIDIMsgSetupChanged) {
        scanAndConnectHardware(*app);
    }
}

void midiHwReadProc(const MIDIPacketList* pktlist, void* readProcRefCon, void* srcConnRefCon) {
    auto* app = static_cast<BridgeApp*>(readProcRefCon);
    if (!app || !pktlist) return;

    const auto kind = static_cast<HwSourceKind>(reinterpret_cast<uintptr_t>(srcConnRefCon));
    if (kind == HwSourceKind::Mpe && app->virt_mpe_src) {
        /* Forward MPE packet list directly to Logic Pro before tap processing for minimum jitter. */
        MIDIReceived(app->virt_mpe_src, pktlist);
    }

    ScopedLock guard(app->lock);
    const uint64_t now_ms = monotonicMs(*app);
    const MIDIPacket* pkt = &pktlist->packet[0];
    for (UInt32 p = 0; p < pktlist->numPackets; ++p) {
        if (kind == HwSourceKind::Daw) {
            app->hw_daw_parser.feed(pkt->data, pkt->length, false, [app, now_ms](const uint8_t* msg, size_t msg_len) {
                app->logger.log(now_ms, sentia::logic::MIDILogger::Port::TilesDaw, sentia::logic::MIDILogger::Direction::RxFromHardware, msg, msg_len);
                app->monitor.recordPacket(now_ms, sentia::logic::MIDIMonitor::Port::TilesDaw, sentia::logic::MIDIMonitor::Direction::RxFromHardware, msg, msg_len);
                app->link.onHardwareDawMidi(msg, msg_len, now_ms, bridgeEmitCb, app);
            });
        } else if (kind == HwSourceKind::Mpe) {
            app->hw_mpe_parser.feed(pkt->data, pkt->length, false, [app, now_ms](const uint8_t* msg, size_t msg_len) {
                app->logger.log(now_ms, sentia::logic::MIDILogger::Port::TilesMpe, sentia::logic::MIDILogger::Direction::RxFromHardware, msg, msg_len);
                app->monitor.recordPacket(now_ms, sentia::logic::MIDIMonitor::Port::TilesMpe, sentia::logic::MIDIMonitor::Direction::RxFromHardware, msg, msg_len);
                if (app->virt_mpe_src) {
                    app->monitor.recordPacket(now_ms, sentia::logic::MIDIMonitor::Port::LogicMpe, sentia::logic::MIDIMonitor::Direction::TxToLogic, msg, msg_len);
                }
                app->link.onHardwareMpeMidi(msg, msg_len, bridgeEmitCb, app);
            });
        }
        pkt = MIDIPacketNext(pkt);
    }
    scheduleHapticExpiryLocked(*app, now_ms);
    scheduleLogFlush(*app);
    scheduleMonitorFlush(*app);
    syncUiViewAsync(*app);
}

void midiVirtDestReadProc(const MIDIPacketList* pktlist, void* readProcRefCon, void* srcConnRefCon) {
    (void)srcConnRefCon;
    auto* app = g_app_ptr;
    if (!app || !pktlist) return;

    const auto kind = static_cast<VirtDestKind>(reinterpret_cast<uintptr_t>(readProcRefCon));
    if (kind == VirtDestKind::Mpe) {
        if (app->hw_mpe_dst && app->out_port) {
            MIDISend(app->out_port, app->hw_mpe_dst, pktlist);
        }
        ScopedLock guard(app->lock);
        const uint64_t now_ms = monotonicMs(*app);
        const MIDIPacket* mpe_pkt = &pktlist->packet[0];
        for (UInt32 p = 0; p < pktlist->numPackets; ++p) {
            app->logic_mpe_parser.feed(mpe_pkt->data, mpe_pkt->length, false, [app, now_ms](const uint8_t* msg, size_t msg_len) {
                app->logger.log(now_ms, sentia::logic::MIDILogger::Port::LogicMpe, sentia::logic::MIDILogger::Direction::RxFromLogic, msg, msg_len);
                app->monitor.recordPacket(now_ms, sentia::logic::MIDIMonitor::Port::LogicMpe, sentia::logic::MIDIMonitor::Direction::RxFromLogic, msg, msg_len);
                if (app->hw_mpe_dst && app->out_port) {
                    app->monitor.recordPacket(now_ms, sentia::logic::MIDIMonitor::Port::TilesMpe, sentia::logic::MIDIMonitor::Direction::TxToHardware, msg, msg_len);
                }
            });
            mpe_pkt = MIDIPacketNext(mpe_pkt);
        }
        scheduleLogFlush(*app);
        scheduleMonitorFlush(*app);
        return;
    }

    ScopedLock guard(app->lock);
    const uint64_t now_ms = monotonicMs(*app);
    const MIDIPacket* pkt = &pktlist->packet[0];
    for (UInt32 p = 0; p < pktlist->numPackets; ++p) {
        if (kind == VirtDestKind::Mcu) {
            app->mcu_parser.feed(pkt->data, pkt->length, false, [app, now_ms](const uint8_t* msg, size_t msg_len) {
                app->logger.log(now_ms, sentia::logic::MIDILogger::Port::LogicMcu, sentia::logic::MIDILogger::Direction::RxFromLogic, msg, msg_len);
                app->monitor.recordPacket(now_ms, sentia::logic::MIDIMonitor::Port::LogicMcu, sentia::logic::MIDIMonitor::Direction::RxFromLogic, msg, msg_len);
                app->link.onLogicMcuMidi(msg, msg_len, now_ms, bridgeEmitCb, app);
            });
        } else if (kind == VirtDestKind::Grid) {
            app->grid_parser.feed(pkt->data, pkt->length, true, [app, now_ms](const uint8_t* msg, size_t msg_len) {
                if (msg_len >= 3 && (msg[0] & 0xF0u) == 0xB0u && (msg[0] & 0x0Fu) == 0u) {
                    const uint8_t cc = static_cast<uint8_t>(msg[1] & 0x7Fu);
                    const uint8_t val = static_cast<uint8_t>(msg[2] & 0x7Fu);
                    if (cc == TILES_LOGIC_CC_LOG_ENABLE) {
                        const bool en = val > 0u;
                        app->logger.setEnabled(en);
                        if (en) {
                            app->logger.log(now_ms, sentia::logic::MIDILogger::Port::LogicGrid, sentia::logic::MIDILogger::Direction::RxFromLogic, msg, msg_len);
                        } else {
                            app->logger.flush();
                        }
                        std::fprintf(stderr, "[tiles-logic] MIDILogger console %s (via Scripter CC 85)\n", en ? "ENABLED" : "DISABLED");
                        return;
                    }
                    if (cc == TILES_LOGIC_CC_LOG_FILTER) {
                        uint32_t mask = sentia::logic::MIDILogger::CatAll;
                        if (val == 1u) mask = sentia::logic::MIDILogger::CatMpe;
                        else if (val == 2u) mask = sentia::logic::MIDILogger::CatHaptic;
                        else if (val == 3u) mask = sentia::logic::MIDILogger::CatGrid;
                        else if (val == 4u) mask = sentia::logic::MIDILogger::CatMcu;
                        app->logger.setCategoryMask(mask);
                        if (app->logger.isEnabled()) {
                            app->logger.log(now_ms, sentia::logic::MIDILogger::Port::LogicGrid, sentia::logic::MIDILogger::Direction::RxFromLogic, msg, msg_len);
                        }
                        return;
                    }
                }
                app->logger.log(now_ms, sentia::logic::MIDILogger::Port::LogicGrid, sentia::logic::MIDILogger::Direction::RxFromLogic, msg, msg_len);
                app->monitor.recordPacket(now_ms, sentia::logic::MIDIMonitor::Port::LogicGrid, sentia::logic::MIDIMonitor::Direction::RxFromLogic, msg, msg_len);
                app->link.onLogicGridMidi(msg, msg_len, now_ms, bridgeEmitCb, app);
            });
        } else if (kind == VirtDestKind::Display) {
            app->display_parser.feed(pkt->data, pkt->length, false, [app, now_ms](const uint8_t* msg, size_t msg_len) {
                app->logger.log(now_ms, sentia::logic::MIDILogger::Port::LogicDisplay, sentia::logic::MIDILogger::Direction::RxFromLogic, msg, msg_len);
                app->monitor.recordPacket(now_ms, sentia::logic::MIDIMonitor::Port::LogicDisplay, sentia::logic::MIDIMonitor::Direction::RxFromLogic, msg, msg_len);
                app->link.onLogicDisplayMidi(msg, msg_len, bridgeEmitCb, app);
            });
        }
        pkt = MIDIPacketNext(pkt);
    }
    scheduleHapticExpiryLocked(*app, now_ms);
    scheduleLogFlush(*app);
    scheduleMonitorFlush(*app);
    syncUiViewAsync(*app);
}

dispatch_source_t installSignalStopSource(int sig) {
    signal(sig, SIG_IGN);
    dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, static_cast<uintptr_t>(sig), 0, dispatch_get_main_queue());
    if (src) {
        dispatch_source_set_event_handler(src, ^{
            CFRunLoopStop(CFRunLoopGetMain());
        });
        dispatch_resume(src);
    }
    return src;
}

void assignVirtualProps(MIDIEndpointRef ep, SInt32 base_uid, uint8_t unit_tag) {
    if (!ep) return;
    MIDIObjectSetIntegerProperty(ep, kMIDIPropertyUniqueID, base_uid + static_cast<SInt32>(unit_tag));
    MIDIObjectSetIntegerProperty(ep, kMIDIPropertyAdvanceScheduleTimeMuSec, 0);
}

CFStringRef createPortLabel(const char* base, uint8_t unit) {
    if (unit >= 1 && unit <= 4) {
        return CFStringCreateWithFormat(kCFAllocatorDefault, nullptr, CFSTR("%s %u"), base, unit);
    }
    return CFStringCreateWithCString(kCFAllocatorDefault, base, kCFStringEncodingUTF8);
}

void printUsage(const char* prog) {
    std::fprintf(stderr, "Usage: %s [--unit 1..4] [--toggle-transport] [--no-expr-split] [--headless] [--monitor] [--log-midi] [--log-filter all|mpe|haptic|grid|mcu] [--verbose]\n", prog);
}

uint32_t parseLogFilter(const char* name) {
    if (std::strcmp(name, "mpe") == 0) return sentia::logic::MIDILogger::CatMpe;
    if (std::strcmp(name, "haptic") == 0) return sentia::logic::MIDILogger::CatHaptic;
    if (std::strcmp(name, "grid") == 0) return sentia::logic::MIDILogger::CatGrid;
    if (std::strcmp(name, "mcu") == 0) return sentia::logic::MIDILogger::CatMcu;
    return sentia::logic::MIDILogger::CatAll;
}

} // namespace

int main(int argc, char** argv) {
    BridgeApp app;
    g_app_ptr = &app;
    mach_timebase_info(&app.timebase);

    uint8_t init_unit = 1;
    bool toggle_transport = false;
    bool no_expr_split = false;
    bool headless = false;
    bool open_monitor = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--unit") == 0 && i + 1 < argc) {
            const int u = std::atoi(argv[++i]);
            if (u < 1 || u > 4) {
                std::fprintf(stderr, "Invalid --unit value (expected 1..4)\n");
                return 2;
            }
            app.requested_unit = static_cast<uint8_t>(u);
            init_unit = static_cast<uint8_t>(u);
        } else if (std::strcmp(argv[i], "--toggle-transport") == 0) {
            toggle_transport = true;
        } else if (std::strcmp(argv[i], "--no-expr-split") == 0) {
            no_expr_split = true;
        } else if (std::strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (std::strcmp(argv[i], "--monitor") == 0) {
            open_monitor = true;
        } else if (std::strcmp(argv[i], "--log-midi") == 0) {
            app.logger.setEnabled(true);
        } else if (std::strcmp(argv[i], "--log-filter") == 0 && i + 1 < argc) {
            app.logger.setEnabled(true);
            app.logger.setCategoryMask(parseLogFilter(argv[++i]));
        } else if (std::strcmp(argv[i], "--verbose") == 0 || std::strcmp(argv[i], "-v") == 0) {
            app.verbose = true;
        } else {
            printUsage(argv[0]);
            return 2;
        }
    }

    app.link.reset(init_unit);
    if (toggle_transport) app.link.setPerformanceTransport(false);
    if (no_expr_split) app.link.setExpressionSplit(false);

    app.manager.setOnViewChanged([&app](sentia::logic::LogicManager::ViewState next_state) {
        ScopedLock guard(app.lock);
        tiles_logic_set_page(&app.link.core(), static_cast<tiles_logic_page_t>(next_state), bridgeEmitCb, &app);
    });
    app.manager.setOnOpenMonitor([&app]() {
        app.monitor.openWindow();
        app.monitor.drainAndRefreshUi();
    });

    dispatch_source_t sigint_src = installSignalStopSource(SIGINT);
    dispatch_source_t sigterm_src = installSignalStopSource(SIGTERM);

    if (MIDIClientCreate(CFSTR("SENTIA TILES Logic Bridge"), midiNotifyProc, &app, &app.client) != noErr) {
        std::fprintf(stderr, "[tiles-logic] MIDIClientCreate failed\n");
        return 1;
    }

    if (MIDIInputPortCreate(app.client, CFSTR("TILES Hardware In"), midiHwReadProc, &app, &app.in_port) != noErr) {
        std::fprintf(stderr, "[tiles-logic] MIDIInputPortCreate failed\n");
        return 1;
    }

    if (MIDIOutputPortCreate(app.client, CFSTR("TILES Hardware Out"), &app.out_port) != noErr) {
        std::fprintf(stderr, "[tiles-logic] MIDIOutputPortCreate failed\n");
        return 1;
    }

    CFStringRef mcu_name = createPortLabel("TILES Logic MCU", app.requested_unit);
    CFStringRef grid_name = createPortLabel("TILES Logic Grid", app.requested_unit);
    CFStringRef disp_name = createPortLabel("TILES Logic Display", app.requested_unit);
    CFStringRef mpe_name = createPortLabel("TILES Logic MPE", app.requested_unit);

    const bool virt_ok =
        MIDISourceCreate(app.client, mcu_name, &app.virt_mcu_src) == noErr &&
        MIDIDestinationCreate(app.client, mcu_name, midiVirtDestReadProc,
                              reinterpret_cast<void*>(static_cast<uintptr_t>(VirtDestKind::Mcu)), &app.virt_mcu_dst) == noErr &&
        MIDISourceCreate(app.client, grid_name, &app.virt_grid_src) == noErr &&
        MIDIDestinationCreate(app.client, grid_name, midiVirtDestReadProc,
                              reinterpret_cast<void*>(static_cast<uintptr_t>(VirtDestKind::Grid)), &app.virt_grid_dst) == noErr &&
        MIDIDestinationCreate(app.client, disp_name, midiVirtDestReadProc,
                              reinterpret_cast<void*>(static_cast<uintptr_t>(VirtDestKind::Display)), &app.virt_display_dst) == noErr &&
        MIDISourceCreate(app.client, mpe_name, &app.virt_mpe_src) == noErr &&
        MIDIDestinationCreate(app.client, mpe_name, midiVirtDestReadProc,
                              reinterpret_cast<void*>(static_cast<uintptr_t>(VirtDestKind::Mpe)), &app.virt_mpe_dst) == noErr;

    CFRelease(mcu_name);
    CFRelease(grid_name);
    CFRelease(disp_name);
    CFRelease(mpe_name);

    if (!virt_ok) {
        std::fprintf(stderr, "[tiles-logic] failed to create CoreMIDI virtual endpoints\n");
        return 1;
    }

    assignVirtualProps(app.virt_mcu_src, 0x544C4D00, app.requested_unit);
    assignVirtualProps(app.virt_mcu_dst, 0x544C4D10, app.requested_unit);
    assignVirtualProps(app.virt_grid_src, 0x544C4700, app.requested_unit);
    assignVirtualProps(app.virt_grid_dst, 0x544C4710, app.requested_unit);
    assignVirtualProps(app.virt_display_dst, 0x544C4400, app.requested_unit);
    assignVirtualProps(app.virt_mpe_src, 0x544C4500, app.requested_unit);
    assignVirtualProps(app.virt_mpe_dst, 0x544C4510, app.requested_unit);

    scanAndConnectHardware(app);
    if (!headless) {
        app.manager.attachUi();
        if (open_monitor) {
            app.monitor.openWindow();
        }
    }

    if (app.verbose) {
        std::fprintf(stderr, "[tiles-logic] C++ CoreMIDI bridge + StudioLink + LogicManager active (0%% polling idle)\n");
    }

    /* Event-driven run loop blocks at zero CPU usage until CoreMIDI or SIGTERM wakes it. */
    CFRunLoopRun();

    app.logger.flush();
    app.monitor.closeWindow();
    app.manager.detachUi();
    {
        ScopedLock guard(app.lock);
        disconnectHardwareSources(app);
    }

    if (sigint_src) dispatch_source_cancel(sigint_src);
    if (sigterm_src) dispatch_source_cancel(sigterm_src);
    if (app.virt_mcu_src) MIDIEndpointDispose(app.virt_mcu_src);
    if (app.virt_mcu_dst) MIDIEndpointDispose(app.virt_mcu_dst);
    if (app.virt_grid_src) MIDIEndpointDispose(app.virt_grid_src);
    if (app.virt_grid_dst) MIDIEndpointDispose(app.virt_grid_dst);
    if (app.virt_display_dst) MIDIEndpointDispose(app.virt_display_dst);
    if (app.virt_mpe_src) MIDIEndpointDispose(app.virt_mpe_src);
    if (app.virt_mpe_dst) MIDIEndpointDispose(app.virt_mpe_dst);
    if (app.in_port) MIDIPortDispose(app.in_port);
    if (app.out_port) MIDIPortDispose(app.out_port);
    if (app.client) MIDIClientDispose(app.client);
    g_app_ptr = nullptr;

    return 0;
}
