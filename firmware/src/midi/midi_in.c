#include "midi_in.h"

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

static bool s_in_sysex;
static uint8_t s_sysex_buf[MIDI_IN_SYSEX_MAX];
static size_t s_sysex_len;
/* Set the instant a frame overflows MIDI_IN_SYSEX_MAX -- suppresses
 * delivering a truncated, silently-wrong-looking frame on the eventual
 * 0xF7; the whole oversized frame is dropped instead, matching this
 * file's own header comment ("dropped, not truncated-and-delivered"). */
static bool s_sysex_overflowed;

/* Channel-voice running-status state -- see this file's own header
 * comment. s_cv_status is the current status byte (0 = none/unknown,
 * e.g. right after boot or after a System Common byte cancels it, per
 * the MIDI spec's own running-status rule); s_cv_data_needed is 1 for
 * Program Change/Channel Pressure, 2 for everything else channel-voice;
 * s_cv_data_count counts how many of THIS message's data bytes have
 * arrived so far, reset to 0 once a complete message dispatches so the
 * next data-byte pair (no repeated status byte needed) is read as a
 * repeat of the same message under running status. */
static uint8_t s_cv_status;
static uint8_t s_cv_data[2];
static uint8_t s_cv_data_needed;
static uint8_t s_cv_data_count;

void tiles_midi_in_init(void) {
    s_realtime_callback_count = 0u;
    s_sysex_callback_count = 0u;
    s_note_callback_count = 0u;
    s_in_sysex = false;
    s_sysex_len = 0u;
    s_sysex_overflowed = false;
    s_cv_status = 0u;
    s_cv_data_needed = 0u;
    s_cv_data_count = 0u;
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

static void fire_realtime(uint8_t byte, uint32_t now_ms) {
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

/* Dispatches one complete channel-voice message (s_cv_status + however
 * many data bytes it needed, already collected in s_cv_data[]) -- only
 * Note-On/Off (0x9n/0x8n) actually fire tiles_midi_in_note_callback_t;
 * every other type was just consumed to keep running status and byte
 * alignment correct for whatever follows, see this file's own header
 * comment. A Note-On with velocity 0 normalizes to note_on=false here
 * (the standard MIDI running-status convention for a cheap note-off,
 * used by e.g. Ableton's own MIDI output), so callers never see that
 * distinction. */
static void dispatch_channel_voice(uint32_t now_ms) {
    uint8_t high_nibble = (uint8_t)(s_cv_status & 0xF0u);
    uint8_t channel = (uint8_t)(s_cv_status & 0x0Fu);
    if (high_nibble == 0x90u) {
        uint8_t velocity = s_cv_data[1];
        fire_note(channel, s_cv_data[0], velocity, velocity > 0u, now_ms);
    } else if (high_nibble == 0x80u) {
        fire_note(channel, s_cv_data[0], 0u, false, now_ms);
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
            uint8_t byte = buf[i];

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
                    fire_realtime(byte, now_ms);
                }
                continue;
            }

            if (byte == MIDI_SYSEX_START) {
                s_in_sysex = true;
                s_sysex_len = 0u;
                s_sysex_overflowed = false;
                continue;
            }

            if (!s_in_sysex) {
                /* Not Real-Time, not a SysEx start, and no SysEx
                 * currently open -- a channel-voice message (or a
                 * System Common byte cancelling running status). See
                 * this file's own header comment. */
                if (byte >= 0x80u) {
                    if (byte <= 0xEFu) {
                        s_cv_status = byte;
                        s_cv_data_needed = channel_voice_data_len(byte);
                        s_cv_data_count = 0u;
                    } else {
                        /* System Common (0xF1-0xF7) -- cancels running
                         * status per the MIDI spec. A stray, un-opened
                         * SysEx-end (0xF7) lands here too and is handled
                         * identically -- there's no open frame for it to
                         * close, so canceling running status is all
                         * there is to do with it either way. */
                        s_cv_status = 0u;
                    }
                    continue;
                }
                if (s_cv_status == 0u) {
                    /* A stray data byte with no known running status
                     * (right after boot, or after a System Common byte
                     * cancelled it) -- nothing to pair it with. */
                    continue;
                }
                s_cv_data[s_cv_data_count++] = byte;
                if (s_cv_data_count >= s_cv_data_needed) {
                    dispatch_channel_voice(now_ms);
                    /* Ready for the next repeat under the same running
                     * status, with no repeated status byte needed --
                     * the standard MIDI running-status convention, e.g.
                     * a stream of Note-Ons on one channel. */
                    s_cv_data_count = 0u;
                }
                continue;
            }

            if (byte == MIDI_SYSEX_END) {
                if (!s_sysex_overflowed) {
                    fire_sysex(s_sysex_buf, s_sysex_len);
                }
                s_in_sysex = false;
                continue;
            }

            if (byte >= 0x80u) {
                /* Any other status byte arriving before the expected
                 * 0xF7 means the sender abandoned this SysEx (or
                 * something upstream corrupted the stream) -- abort
                 * rather than keep accumulating into a frame that will
                 * never make sense. */
                s_in_sysex = false;
                continue;
            }

            if (s_sysex_len < MIDI_IN_SYSEX_MAX) {
                s_sysex_buf[s_sysex_len++] = byte;
            } else {
                s_sysex_overflowed = true;
            }
        }
    }
}
