#include "midi_in.h"

#include "din_midi.h"
#include "usb_midi_packet.h"

#include "tusb.h"

#include "pico/time.h"

#include <stdbool.h>

#define MIDI_REALTIME_CLOCK 0xF8u
#define MIDI_REALTIME_START 0xFAu
#define MIDI_REALTIME_CONTINUE 0xFBu
#define MIDI_REALTIME_STOP 0xFCu
/* Start of the whole System Real-Time range (0xF8-0xFF). Real-Time bytes
 * may appear anywhere, even mid-message or mid-SysEx, and must not disturb
 * framing or running status, so the whole range is skipped before any
 * parser state is touched. Callbacks fire only for Clock/Start/Continue/
 * Stop; the rest (0xF9, 0xFD, Active Sensing, Reset) pass through. */
#define MIDI_REALTIME_RANGE_START 0xF8u
#define MIDI_SYSEX_START 0xF0u
#define MIDI_SYSEX_END 0xF7u

/* Largest SysEx frame accepted. The biggest message defined today is well
 * under 16 bytes; a sender that never sends 0xF7 can't grow this. */
#define MIDI_IN_SYSEX_MAX 64u

static tiles_midi_in_realtime_callback_t s_realtime_callbacks[TILES_MIDI_IN_MAX_CALLBACKS];
static uint8_t s_realtime_callback_count;
static tiles_midi_in_sysex_callback_t s_sysex_callbacks[TILES_MIDI_IN_MAX_CALLBACKS];
static uint8_t s_sysex_callback_count;
static tiles_midi_in_note_callback_t s_note_callbacks[TILES_MIDI_IN_MAX_CALLBACKS];
static uint8_t s_note_callback_count;

/* See tiles_midi_in_activity_count() in midi_in.h. */
static uint32_t s_activity_count;

/* One parser per source: the two USB cables (MAIN, DAW) and the DIN jack.
 * Each needs its own SysEx and running-status state, or interleaved cables
 * would splice one message onto another. (Real-Time bytes are handled
 * before any of this state; see feed_byte().) All parsers fire the same
 * callbacks; SysEx listeners also get the port (Scene Launch accepts only
 * the DAW port; Identity replies go back where the request came from).
 *
 * Running status: cv_status is the current status byte (0 = none, e.g.
 * after boot or a System Common byte); cv_data_needed is 1 for Program
 * Change/Channel Pressure, else 2; cv_data_count counts this message's data
 * bytes and resets on dispatch, so the next bytes repeat the same status. */
typedef struct {
    bool in_sysex;
    uint8_t sysex_buf[MIDI_IN_SYSEX_MAX];
    size_t sysex_len;
    /* Set once a frame overflows MIDI_IN_SYSEX_MAX: the whole frame is dropped
     * at 0xF7 rather than delivered truncated. */
    bool sysex_overflowed;
    uint8_t cv_status;
    uint8_t cv_data[2];
    uint8_t cv_data_needed;
    uint8_t cv_data_count;
} midi_parser_t;

/* Indexed by tiles_midi_port_t. */
#define MIDI_SOURCE_COUNT 3u
typedef tiles_midi_port_t midi_source_t;

static midi_parser_t s_parsers[MIDI_SOURCE_COUNT];

/* Only one source may drive the clock: USB and DIN both at 24 PPQN would
 * count double tempo (and fight over Start/Stop). The first source to send
 * Real-Time owns it while it keeps sending, and loses it after
 * MIDI_RT_OWNER_HOLD_MS of silence. */
#define MIDI_RT_OWNER_HOLD_MS 500u
static int8_t s_rt_owner = -1;
static uint32_t s_rt_owner_last_ms;

static void parser_reset(midi_parser_t *p) {
    p->in_sysex = false;
    p->sysex_len = 0u;
    p->sysex_overflowed = false;
    p->cv_status = 0u;
    p->cv_data_needed = 0u;
    p->cv_data_count = 0u;
}

void tiles_midi_in_init(void) {
    s_realtime_callback_count = 0u;
    s_sysex_callback_count = 0u;
    s_note_callback_count = 0u;
    for (uint8_t i = 0u; i < (uint8_t)MIDI_SOURCE_COUNT; i++) {
        parser_reset(&s_parsers[i]);
    }
    s_rt_owner = -1;
}

bool tiles_midi_in_register_realtime_callback(tiles_midi_in_realtime_callback_t callback) {
    if (s_realtime_callback_count >= TILES_MIDI_IN_MAX_CALLBACKS) {
        return false;
    }
    s_realtime_callbacks[s_realtime_callback_count++] = callback;
    return true;
}

bool tiles_midi_in_register_sysex_callback(tiles_midi_in_sysex_callback_t callback) {
    if (s_sysex_callback_count >= TILES_MIDI_IN_MAX_CALLBACKS) {
        return false;
    }
    s_sysex_callbacks[s_sysex_callback_count++] = callback;
    return true;
}

bool tiles_midi_in_register_note_callback(tiles_midi_in_note_callback_t callback) {
    if (s_note_callback_count >= TILES_MIDI_IN_MAX_CALLBACKS) {
        return false;
    }
    s_note_callbacks[s_note_callback_count++] = callback;
    return true;
}

uint32_t tiles_midi_in_activity_count(void) {
    return s_activity_count;
}

static void fire_realtime(uint8_t byte, uint32_t now_ms) {
    s_activity_count++;
    for (uint8_t i = 0; i < s_realtime_callback_count; i++) {
        s_realtime_callbacks[i](byte, now_ms);
    }
}

static void fire_sysex(tiles_midi_port_t port, const uint8_t *data, size_t len) {
    for (uint8_t i = 0; i < s_sysex_callback_count; i++) {
        s_sysex_callbacks[i](port, data, len);
    }
}

static void fire_note(uint8_t channel, uint8_t note, uint8_t velocity, bool note_on, uint32_t now_ms) {
    s_activity_count++;
    for (uint8_t i = 0; i < s_note_callback_count; i++) {
        s_note_callbacks[i](channel, note, velocity, note_on, now_ms);
    }
}

/* Data bytes for a channel-voice status: 1 for Program Change (0xCn) and
 * Channel Pressure (0xDn), 2 for the rest. */
static uint8_t channel_voice_data_len(uint8_t status) {
    uint8_t high_nibble = (uint8_t)(status & 0xF0u);
    if (high_nibble == 0xC0u || high_nibble == 0xD0u) {
        return 1u;
    }
    return 2u;
}

/* Dispatches one complete channel-voice message. Only Note-On/Off fire a
 * callback; other types are consumed to keep alignment. Note-On with
 * velocity 0 is reported as note_on=false. */
static void dispatch_channel_voice(const midi_parser_t *p, uint32_t now_ms) {
    uint8_t high_nibble = (uint8_t)(p->cv_status & 0xF0u);
    uint8_t channel = (uint8_t)(p->cv_status & 0x0Fu);
    if (high_nibble == 0x90u) {
        uint8_t velocity = p->cv_data[1];
        fire_note(channel, p->cv_data[0], velocity, velocity > 0u, now_ms);
    } else if (high_nibble == 0x80u) {
        fire_note(channel, p->cv_data[0], 0u, false, now_ms);
    }
}

/* True if a Real-Time byte from `source` may reach the callbacks (see
 * s_rt_owner above). */
static bool realtime_source_allowed(midi_source_t source, uint32_t now_ms) {
    if (s_rt_owner < 0 || (uint32_t)(now_ms - s_rt_owner_last_ms) > MIDI_RT_OWNER_HOLD_MS) {
        s_rt_owner = (int8_t)source;
    }
    if (s_rt_owner != (int8_t)source) {
        return false;
    }
    s_rt_owner_last_ms = now_ms;
    return true;
}

/* Runs one received byte through `source`'s parser. */
static void feed_byte(midi_source_t source, uint8_t byte, uint32_t now_ms) {
    midi_parser_t *p = &s_parsers[source];

    /* Real-Time bytes may appear anywhere: skipped before any SysEx or
     * running-status state is touched (see MIDI_REALTIME_RANGE_START). */
    if (byte >= MIDI_REALTIME_RANGE_START) {
        if (byte == MIDI_REALTIME_CLOCK || byte == MIDI_REALTIME_START || byte == MIDI_REALTIME_CONTINUE ||
            byte == MIDI_REALTIME_STOP) {
            if (realtime_source_allowed(source, now_ms)) {
                fire_realtime(byte, now_ms);
            }
        }
        return;
    }

    if (byte == MIDI_SYSEX_START) {
        p->in_sysex = true;
        p->sysex_len = 0u;
        p->sysex_overflowed = false;
        return;
    }

    if (!p->in_sysex) {
        /* Not Real-Time, not SysEx start, no SysEx open: a channel-voice status
         * (or a System Common byte, which cancels running status). */
        if (byte >= 0x80u) {
            if (byte <= 0xEFu) {
                p->cv_status = byte;
                p->cv_data_needed = channel_voice_data_len(byte);
                p->cv_data_count = 0u;
            } else {
                /* System Common (0xF1-0xF7) cancels running status. A stray 0xF7 with no
                 * open frame lands here too; cancelling is all there is to do. */
                p->cv_status = 0u;
            }
            return;
        }
        if (p->cv_status == 0u) {
            /* Data byte with no running status (after boot or System Common): drop it. */
            return;
        }
        p->cv_data[p->cv_data_count++] = byte;
        if (p->cv_data_count >= p->cv_data_needed) {
            dispatch_channel_voice(p, now_ms);
            /* Ready for the next message under the same running status. */
            p->cv_data_count = 0u;
        }
        return;
    }

    if (byte == MIDI_SYSEX_END) {
        if (!p->sysex_overflowed) {
            fire_sysex(source, p->sysex_buf, p->sysex_len);
        }
        p->in_sysex = false;
        return;
    }

    if (byte >= 0x80u) {
        /* Any other status byte before 0xF7 means the SysEx was abandoned: abort. */
        p->in_sysex = false;
        return;
    }

    if (p->sysex_len < MIDI_IN_SYSEX_MAX) {
        p->sysex_buf[p->sysex_len++] = byte;
    } else {
        p->sysex_overflowed = true;
    }
}

void tiles_midi_in_scan(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    /* Drain every packet this pass. Whole USB-MIDI packets, not the byte
     * stream: the stream API merges cables, and the packet's cable number says
     * which port it came from (midi/usb_midi_packet.h). Unknown cables are
     * ignored. */
    uint8_t packet[4];
    while (tud_midi_n_packet_read(0, packet)) {
        uint8_t cable = (uint8_t)(packet[0] >> 4);
        if (cable >= TILES_USB_MIDI_NUM_CABLES) {
            continue;
        }
        midi_source_t source = cable == TILES_USB_MIDI_CABLE_DAW ? TILES_MIDI_PORT_DAW : TILES_MIDI_PORT_MAIN;
        uint8_t n = tiles_usb_midi_cin_length(packet[0]);
        for (uint8_t i = 0; i < n; i++) {
            feed_byte(source, packet[1u + i], now_ms);
        }
    }

    /* DIN: if bytes were lost (ring overflow, UART framing/break/overrun), drop
     * the half-built message so nothing is stitched across the gap; the next
     * status byte re-syncs. */
    if (tiles_din_midi_rx_take_loss()) {
        parser_reset(&s_parsers[TILES_MIDI_PORT_DIN]);
    }
    uint8_t din_byte;
    while (tiles_din_midi_rx_read_byte(&din_byte)) {
        feed_byte(TILES_MIDI_PORT_DIN, din_byte, now_ms);
    }
}
