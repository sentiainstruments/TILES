#include "midi_channels.h"

#include <stddef.h>

/* Descending: index 0 = channel 9 (top of the pool), index 7 = channel 2. */
static uint8_t s_pool_channel[TILES_MIDI_SHARED_POOL_SIZE];
static bool s_pool_in_use[TILES_MIDI_SHARED_POOL_SIZE];

/* What the receiver was last told (RPN 6); starts at the full pool of 8. */
static uint8_t s_declared_zone_size;

/* Indexed by channel - TILES_MIDI_SHARED_POOL_FIRST; see
 * tiles_midi_channels_note_channel_claimed(). */
static bool s_live_note_active[TILES_MIDI_SHARED_POOL_SIZE];

void tiles_midi_channels_init(void) {
    for (uint8_t i = 0u; i < TILES_MIDI_SHARED_POOL_SIZE; i++) {
        s_pool_channel[i] = (uint8_t)(TILES_MIDI_SHARED_POOL_FIRST + TILES_MIDI_SHARED_POOL_SIZE - 1u - i);
        s_pool_in_use[i] = false;
    }
    s_declared_zone_size = TILES_MIDI_SHARED_POOL_SIZE;
    for (uint8_t i = 0u; i < TILES_MIDI_SHARED_POOL_SIZE; i++) {
        s_live_note_active[i] = false;
    }
}

bool tiles_midi_channels_song_claim(uint8_t *out_channel) {
    for (uint8_t i = 0u; i < TILES_MIDI_SONG_MAX_CONCURRENT; i++) {
        uint8_t channel = s_pool_channel[i];
        /* Skip a channel a live note is using. That channel is simply picked up
         * by a later claim once free, so no permanent gap forms. */
        if (!s_pool_in_use[i] && !s_live_note_active[channel - TILES_MIDI_SHARED_POOL_FIRST]) {
            s_pool_in_use[i] = true;
            *out_channel = channel;
            return true;
        }
    }
    return false;
}

void tiles_midi_channels_song_release(uint8_t channel) {
    for (uint8_t i = 0u; i < TILES_MIDI_SHARED_POOL_SIZE; i++) {
        if (s_pool_channel[i] == channel) {
            s_pool_in_use[i] = false;
            return;
        }
    }
}

bool tiles_midi_channels_song_holds(uint8_t channel) {
    for (uint8_t i = 0u; i < TILES_MIDI_SHARED_POOL_SIZE; i++) {
        if (s_pool_channel[i] == channel) {
            return s_pool_in_use[i];
        }
    }
    return false;
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
    /* Count free slots from the bottom (channel 2) up, stopping at the first
     * held one. Song's held set is a prefix from the top, so this equals the
     * free count; and if that ever broke, this still reports only what is
     * safe to use contiguously from channel 2. */
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

uint8_t tiles_midi_channels_declare_zone(void) {
    s_declared_zone_size = tiles_midi_channels_lower_zone_size();
    return s_declared_zone_size;
}

uint8_t tiles_midi_channels_declared_zone_size(void) {
    return s_declared_zone_size;
}

bool tiles_midi_channels_zone_redeclare_pending(void) {
    return tiles_midi_channels_lower_zone_size() != s_declared_zone_size;
}
