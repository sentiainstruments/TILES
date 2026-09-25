#include "midi_in.h"

#include "din_midi.h"

#include "tusb.h"

#include "pico/time.h"

#include <stdbool.h>

#define MIDI_REALTIME_CLOCK 0xF8u
#define MIDI_REALTIME_START 0xFAu
#define MIDI_REALTIME_CONTINUE 0xFBu
#define MIDI_REALTIME_STOP 0xFCu
/* First byte of the full System Real-Time range (0xF8-0xFF) -- NOT just
 * the four bytes above this file fires callbacks for. Real-Time bytes
 * are legally injected ANYWHERE in the stream, including mid-channel-
 * voice-message or mid-SysEx, and per the MIDI spec must never disturb
 * anything around them (neither SysEx framing state nor running
 * status). The whole range is skipped unconditionally in the scan loop
 * below, before any of that state is touched, even though this file
 * only actually fires a callback for the four above -- Undefined
 * (0xF9/0xFD), Active Sensing (0xFE), and Reset (0xFF) still need to
 * pass through transparently rather than being misread as a channel-
 * voice status byte (which would wrongly cancel running status) or a
 * stray SysEx-abort byte. */
#define MIDI_REALTIME_RANGE_START 0xF8u
#define MIDI_SYSEX_START 0xF0u
#define MIDI_SYSEX_END 0xF7u

/* Generous ceiling for one SysEx frame -- the largest message this
 * codebase currently defines (see op_mode.c's own Scene Launch section)
 * is well under 16 bytes; this leaves comfortable headroom for that
 * protocol to grow without needing to revisit this constant, while
 * still being small enough that a stuck/malformed sender (SysEx start
 * with no end ever arriving) can't grow this buffer's cost. */
#define MIDI_IN_SYSEX_MAX 64u

static tiles_midi_in_realtime_callback_t s_realtime_callbacks[TILES_MIDI_IN_MAX_CALLBACKS];
static uint8_t s_realtime_callback_count;
static tiles_midi_in_sysex_callback_t s_sysex_callbacks[TILES_MIDI_IN_MAX_CALLBACKS];
static uint8_t s_sysex_callback_count;
static tiles_midi_in_note_callback_t s_note_callbacks[TILES_MIDI_IN_MAX_CALLBACKS];
static uint8_t s_note_callback_count;

/* See tiles_midi_in_activity_count()'s own comment in midi_in.h. */
static uint32_t s_activity_count;

/* One parser per byte SOURCE -- USB MIDI IN and the DIN jack (real feedback:
 * "yes build DIN MIDI"). Each source needs its own SysEx-framing and
 * running-status state: a message can arrive in pieces, and bytes from two
 * different cables interleaving into ONE parser would splice half a
 * Note-On from one onto the data bytes of another. (The only bytes that may
 * legally land in the middle of a message are Real-Time bytes, and those are
 * handled before any of this state is touched -- see feed_byte().) Both
 * parsers fire the SAME callbacks, so everything that reacts to USB MIDI
 * (clock/transport, the melodic echo, Scene Launch SysEx) reacts to DIN
 * identically.
 *
 * The channel-voice running-status state -- see this file's own header
 * comment. cv_status is the current status byte (0 = none/unknown, e.g.
 * right after boot or after a System Common byte cancels it, per the MIDI
 * spec's own running-status rule); cv_data_needed is 1 for Program
 * Change/Channel Pressure, 2 for everything else channel-voice;
 * cv_data_count counts how many of THIS message's data bytes have arrived
 * so far, reset to 0 once a complete message dispatches so the next
 * data-byte pair (no repeated status byte needed) is read as a repeat of
 * the same message under running status. */
typedef struct {
    bool in_sysex;
    uint8_t sysex_buf[MIDI_IN_SYSEX_MAX];
    size_t sysex_len;
    /* Set the instant a frame overflows MIDI_IN_SYSEX_MAX -- suppresses
     * delivering a truncated, silently-wrong-looking frame on the eventual
     * 0xF7; the whole oversized frame is dropped instead, matching this
     * file's own header comment ("dropped, not truncated-and-delivered"). */
    bool sysex_overflowed;
    uint8_t cv_status;
    uint8_t cv_data[2];
    uint8_t cv_data_needed;
    uint8_t cv_data_count;
} midi_parser_t;

typedef enum {
    MIDI_SOURCE_USB = 0,
    MIDI_SOURCE_DIN = 1,
    MIDI_SOURCE_COUNT
} midi_source_t;

static midi_parser_t s_parsers[MIDI_SOURCE_COUNT];

/* Two clock sources must never both drive services/midi_clock.c's pulse
 * counter: a DAW sending 24 PPQN over USB while a drum machine sends 24 PPQN
 * over DIN would count 48 pulses per beat and run the sequencer at double
 * tempo (and Start/Stop from either would fight). So Real-Time bytes are
 * only forwarded from the source that currently "owns" the clock: the first
 * source to send one, which keeps ownership while it keeps sending, and
 * loses it after MIDI_RT_OWNER_HOLD_MS of silence (transport stopped, cable
 * pulled), so the other source can take over. */
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

static void fire_sysex(const uint8_t *data, size_t len) {
    for (uint8_t i = 0; i < s_sysex_callback_count; i++) {
        s_sysex_callbacks[i](data, len);
    }
}

static void fire_note(uint8_t channel, uint8_t note, uint8_t velocity, bool note_on, uint32_t now_ms) {
    s_activity_count++;
    for (uint8_t i = 0; i < s_note_callback_count; i++) {
        s_note_callbacks[i](channel, note, velocity, note_on, now_ms);
    }
}

/* Number of data bytes a channel-voice status byte's message carries --
 * 1 for Program Change (0xCn) and Channel Pressure (0xDn), 2 for every
 * other channel-voice type (Note Off/On, Poly Pressure, CC, Pitch
 * Bend). `status` is a full status byte (0x80-0xEF); only the high
 * nibble matters. */
static uint8_t channel_voice_data_len(uint8_t status) {
    uint8_t high_nibble = (uint8_t)(status & 0xF0u);
    if (high_nibble == 0xC0u || high_nibble == 0xD0u) {
        return 1u;
    }
    return 2u;
}

/* Dispatches one complete channel-voice message (p->cv_status + however
 * many data bytes it needed, already collected in p->cv_data[]) -- only
 * Note-On/Off (0x9n/0x8n) actually fire tiles_midi_in_note_callback_t;
 * every other type was just consumed to keep running status and byte
 * alignment correct for whatever follows, see this file's own header
 * comment. A Note-On with velocity 0 normalizes to note_on=false here
 * (the standard MIDI running-status convention for a cheap note-off,
 * used by e.g. Ableton's own MIDI output), so callers never see that
 * distinction. */
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

/* True if a Real-Time byte from `source` should reach the callbacks -- see
 * the s_rt_owner comment above. */
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

    /* Real-Time bytes can legally appear ANYWHERE in the stream,
     * including mid-SysEx or mid-channel-voice-message, per the
     * MIDI spec's own real-time priority rule -- checked and
     * skipped unconditionally, before (and regardless of) any
     * SysEx framing OR channel-voice running-status state below,
     * exactly matching midi_clock.c's own prior reasoning for
     * why a plain byte scan is correct for these specifically.
     * The full 0xF8-0xFF range is skipped here (see MIDI_
     * REALTIME_RANGE_START's own comment), even though a
     * callback only fires for the four this file has always
     * cared about. */
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
        /* Not Real-Time, not a SysEx start, and no SysEx
         * currently open -- a channel-voice message (or a
         * System Common byte cancelling running status). See
         * this file's own header comment. */
        if (byte >= 0x80u) {
            if (byte <= 0xEFu) {
                p->cv_status = byte;
                p->cv_data_needed = channel_voice_data_len(byte);
                p->cv_data_count = 0u;
            } else {
                /* System Common (0xF1-0xF7) -- cancels running
                 * status per the MIDI spec. A stray, un-opened
                 * SysEx-end (0xF7) lands here too and is handled
                 * identically -- there's no open frame for it to
                 * close, so canceling running status is all
                 * there is to do with it either way. */
                p->cv_status = 0u;
            }
            return;
        }
        if (p->cv_status == 0u) {
            /* A stray data byte with no known running status
             * (right after boot, or after a System Common byte
             * cancelled it) -- nothing to pair it with. */
            return;
        }
        p->cv_data[p->cv_data_count++] = byte;
        if (p->cv_data_count >= p->cv_data_needed) {
            dispatch_channel_voice(p, now_ms);
            /* Ready for the next repeat under the same running
             * status, with no repeated status byte needed --
             * the standard MIDI running-status convention, e.g.
             * a stream of Note-Ons on one channel. */
            p->cv_data_count = 0u;
        }
        return;
    }

    if (byte == MIDI_SYSEX_END) {
        if (!p->sysex_overflowed) {
            fire_sysex(p->sysex_buf, p->sysex_len);
        }
        p->in_sysex = false;
        return;
    }

    if (byte >= 0x80u) {
        /* Any other status byte arriving before the expected
         * 0xF7 means the sender abandoned this SysEx (or
         * something upstream corrupted the stream) -- abort
         * rather than keep accumulating into a frame that will
         * never make sense. */
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

    uint8_t buf[16];
    uint32_t read;
    /* Loop, not a single read -- same "drain the whole RX FIFO this
     * tick" reasoning services/midi_clock.c's own prior version of this
     * loop already established. */
    while ((read = tud_midi_stream_read(buf, sizeof(buf))) > 0u) {
        for (uint32_t i = 0; i < read; i++) {
            feed_byte(MIDI_SOURCE_USB, buf[i], now_ms);
        }
    }

    /* DIN jack. Bytes were lost or corrupted (ring overflow, UART framing/
     * break/overrun error) -> drop whatever was half-assembled, so the
     * bytes around the gap can't be stitched into a message that was never
     * sent; the next status byte re-syncs. Leftover data bytes from before
     * the gap are then ignored (no running status) rather than misparsed. */
    if (tiles_din_midi_rx_take_loss()) {
        parser_reset(&s_parsers[MIDI_SOURCE_DIN]);
    }
    uint8_t din_byte;
    while (tiles_din_midi_rx_read_byte(&din_byte)) {
        feed_byte(MIDI_SOURCE_DIN, din_byte, now_ms);
    }
}
