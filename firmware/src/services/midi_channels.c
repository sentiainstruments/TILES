#include "midi_channels.h"

#include <stddef.h>

/* Descending: index 0 is channel 9 (the top of the shared pool), index 7 is
 * channel 2 (the bottom) -- see midi_channels.h's own header on why "claim
 * the highest free first" is what keeps this simple. */
static uint8_t s_pool_channel[TILES_MIDI_SHARED_POOL_SIZE];
static bool s_pool_in_use[TILES_MIDI_SHARED_POOL_SIZE];

static uint8_t s_last_declared_zone_size;
static bool s_zone_size_dirty;

/* Indexed by (channel - TILES_MIDI_SHARED_POOL_FIRST) -- see this file's own
 * header comment on tiles_midi_channels_note_channel_claimed(). */
static bool s_live_note_active[TILES_MIDI_SHARED_POOL_SIZE];

void tiles_midi_channels_init(void) {
    for (uint8_t i = 0u; i < TILES_MIDI_SHARED_POOL_SIZE; i++) {
        s_pool_channel[i] = (uint8_t)(TILES_MIDI_SHARED_POOL_FIRST + TILES_MIDI_SHARED_POOL_SIZE - 1u - i);
        s_pool_in_use[i] = false;
    }
    /* An impossible sentinel (the real range is 0..TILES_MIDI_SHARED_POOL_SIZE)
     * so the first tiles_midi_channels_zone_size_changed() call always finds a
     * "change" and fires -- boot always needs an initial declaration, even
     * though the freshly-reset pool's real size (8) happens to match this
     * module's own compile-time default. */
    s_last_declared_zone_size = 0xFFu;
    s_zone_size_dirty = true;
    for (uint8_t i = 0u; i < TILES_MIDI_SHARED_POOL_SIZE; i++) {
        s_live_note_active[i] = false;
    }
}

bool tiles_midi_channels_song_claim(uint8_t *out_channel) {
    for (uint8_t i = 0u; i < TILES_MIDI_SONG_MAX_CONCURRENT; i++) {
        uint8_t channel = s_pool_channel[i];
        /* Skip a channel a live MPE note is using right now -- see this
         * file's own header comment on tiles_midi_channels_note_channel_
         * claimed() for why this check has to live here. A skip never
         * leaves a permanent gap in Song's own held set: whichever channel
         * this passed over is exactly what the NEXT claim (once nothing is
         * blocking it) picks up, same self-healing "always take the
         * highest currently-claimable one" property the rest of this file
         * already relies on. */
        if (!s_pool_in_use[i] && !s_live_note_active[channel - TILES_MIDI_SHARED_POOL_FIRST]) {
            s_pool_in_use[i] = true;
            *out_channel = channel;
            s_zone_size_dirty = true;
            return true;
        }
    }
    return false;
}

void tiles_midi_channels_song_release(uint8_t channel) {
    for (uint8_t i = 0u; i < TILES_MIDI_SHARED_POOL_SIZE; i++) {
        if (s_pool_channel[i] == channel) {
            if (s_pool_in_use[i]) {
                s_pool_in_use[i] = false;
                s_zone_size_dirty = true;
            }
            return;
        }
    }
}

uint8_t tiles_midi_channels_song_in_use_count(void) {
    uint8_t count = 0u;
    for (uint8_t i = 0u; i < TILES_MIDI_SHARED_POOL_SIZE; i++) {
        if (s_pool_in_use[i]) {
            count++;
        }
    }
    return count;
}

uint8_t tiles_midi_channels_lower_zone_size(void) {
    /* Song claims top-down (index 0 = channel 9 first), so at any instant
     * its held set is exactly a prefix of indices 0..k-1 for some k -- the
     * zone is simply how many of the REMAINING (bottom) indices are free,
     * counted from the bottom up. Scanning from the bottom and stopping at
     * the first in-use slot is equivalent to (and cheaper than) counting
     * every free slot, and is robust even if that top-down invariant were
     * ever violated by a future bug: it reports the zone that is ACTUALLY
     * safe to scan contiguously from channel 2, not a raw free-count that
     * could overstate it if a gap existed in the middle. */
    uint8_t size = 0u;
    for (uint8_t i = TILES_MIDI_SHARED_POOL_SIZE; i-- > 0u;) {
        if (s_pool_in_use[i]) {
            break;
        }
        size++;
    }
    return size;
}

void tiles_midi_channels_note_channel_claimed(uint8_t channel) {
    s_live_note_active[channel - TILES_MIDI_SHARED_POOL_FIRST] = true;
}

void tiles_midi_channels_note_channel_released(uint8_t channel) {
    s_live_note_active[channel - TILES_MIDI_SHARED_POOL_FIRST] = false;
}

bool tiles_midi_channels_zone_size_changed(void) {
    if (!s_zone_size_dirty) {
        return false;
    }
    uint8_t current = tiles_midi_channels_lower_zone_size();
    s_zone_size_dirty = false;
    if (current == s_last_declared_zone_size) {
        return false; /* claim+release cancelled out between polls; nothing to tell the receiver */
    }
    s_last_declared_zone_size = current;
    return true;
}
