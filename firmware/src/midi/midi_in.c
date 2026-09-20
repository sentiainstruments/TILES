#include "midi_in.h"

#include "tusb.h"

#include "pico/time.h"

#include <stdbool.h>

#define MIDI_REALTIME_CLOCK 0xF8u
#define MIDI_REALTIME_START 0xFAu
#define MIDI_REALTIME_CONTINUE 0xFBu
#define MIDI_REALTIME_STOP 0xFCu
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

static bool s_in_sysex;
static uint8_t s_sysex_buf[MIDI_IN_SYSEX_MAX];
static size_t s_sysex_len;
/* Set the instant a frame overflows MIDI_IN_SYSEX_MAX -- suppresses
 * delivering a truncated, silently-wrong-looking frame on the eventual
 * 0xF7; the whole oversized frame is dropped instead, matching this
 * file's own header comment ("dropped, not truncated-and-delivered"). */
static bool s_sysex_overflowed;

void tiles_midi_in_init(void) {
    s_realtime_callback_count = 0u;
    s_sysex_callback_count = 0u;
    s_in_sysex = false;
    s_sysex_len = 0u;
    s_sysex_overflowed = false;
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
             * including mid-SysEx, per the MIDI spec's own real-time
             * priority rule -- checked and dispatched unconditionally,
             * before (and regardless of) any SysEx framing state below,
             * exactly matching midi_clock.c's own prior reasoning for
             * why a plain byte scan is correct for these specifically. */
            if (byte == MIDI_REALTIME_CLOCK || byte == MIDI_REALTIME_START || byte == MIDI_REALTIME_CONTINUE ||
                byte == MIDI_REALTIME_STOP) {
                fire_realtime(byte, now_ms);
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
                 * currently open -- out of scope for this parser (a
                 * Note-On/CC/etc.), silently discarded. See this file's
                 * own header comment. */
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
