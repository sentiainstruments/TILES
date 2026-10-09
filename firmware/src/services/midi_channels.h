#pragma once

/* The single source of truth for how TILES divides the 16 MIDI channels:
 * which are a fixed part's own channel, which are shared between Song mode
 * and live MPE, and how big the live MPE zone is. Pure arithmetic, tested
 * natively (firmware/test/test_midi_channels.c).
 *
 * Fixed parts sit OUTSIDE the range live MPE may ever use, so a collision
 * (a harmonic taking a sequencer lane's channel, a live note taking chord
 * mode's) is impossible by construction rather than prevented by checks
 * each caller must remember. Both of those bugs happened before this file.
 *
 * ==== Layout ("channel N" = nibble N-1, as everywhere in this codebase) ====
 *
 *   Channel  1      MPE Zone Master: RPNs and pedals, never an MPE note
 *                   (with MPE off, every live note goes here).
 *   Channels 2-9    SHARED POOL, 8 channels (below).
 *   Channel  10     The drum sequencer (TILES_MIDI_CH_DRUMS): General MIDI's
 *                   percussion channel, so a GM kit or a Drum Rack plays it
 *                   as drums. Nothing else ever sends there.
 *   Channel  11     Game mode (TILES_MIDI_CH_GAME).
 *   Channel  12     Chord mode (TILES_MIDI_CH_CHORD).
 *   Channels 13-16  The 4 sequencer lanes (lane 0 = channel 16).
 *
 * Channels 10-16 are PERMANENT, whether or not the part is sounding, so an
 * external synth patched to "sequencer lane 2" can rely on it. That gives
 * 7 independently addressable parts.
 *
 * ==== Shared pool (channels 2-9) ====
 *
 * Split at any moment between Song mode's tracks and the live MPE Lower
 * Zone. Every channel Song holds is one fewer for live notes, the honest
 * picture of one 16-channel port. Song claims from the TOP down (channel 9
 * first) and the zone counts up from channel 2, so the two never overlap
 * and no collision check is needed. Song can hold all 8
 * (TILES_MIDI_SONG_MAX_CONCURRENT).
 *
 * ==== Zone size and telling the receiver ====
 *
 * RPN 6 declares the zone as the Master Channel plus a COUNT of member
 * channels, so the receiver is told the real size (0 withdraws the zone,
 * used when MPE is switched off). tiles_midi_channels_lower_zone_size() is
 * that size.
 *
 * WHEN it is re-declared matters: a zone change makes the receiver stop
 * every note (spec 2.1.4), and JUCE synths (e.g. Equator) reset the bend
 * range to 48 on every RPN 6. So a new size is only declared when nothing
 * is sounding (services/expression.c mpe_zone_is_idle()), always as RPN 6
 * + RPN 0. Until then live MPE uses the LAST declared size
 * (tiles_midi_channels_declared_zone_size()), skipping channels Song holds
 * inside it (tiles_midi_channels_song_holds()).
 *
 * The zone must be one contiguous run from channel 2 and can't include
 * channel 10, so its maximum is 8. An Upper Zone (Master 16) could add a
 * few more channels but is more fragile in practice; not built. */

#include <stdbool.h>
#include <stdint.h>

/* Permanent: never shared, whether or not the part is sounding. */
#define TILES_MIDI_CH_SEQ_LANE_0 15u /* channel 16 */
#define TILES_MIDI_CH_SEQ_LANE_1 14u /* channel 15 */
#define TILES_MIDI_CH_SEQ_LANE_2 13u /* channel 14 */
#define TILES_MIDI_CH_SEQ_LANE_3 12u /* channel 13 */
#define TILES_MIDI_CH_CHORD 11u      /* channel 12 */
#define TILES_MIDI_CH_GAME 10u       /* channel 11 */

/* General MIDI percussion: the drum sequencer, and nothing else. */
#define TILES_MIDI_CH_DRUMS 9u /* channel 10 */

/* The shared pool: channels 2-9, split between Song mode and the live
 * MPE zone. */
#define TILES_MIDI_SHARED_POOL_FIRST 1u /* channel 2 */
#define TILES_MIDI_SHARED_POOL_SIZE 8u

/* How many pool channels Song may hold at once (all 8 today). A named
 * constant so a future floor for live MPE is a one-line change. */
#define TILES_MIDI_SONG_MAX_CONCURRENT TILES_MIDI_SHARED_POOL_SIZE

void tiles_midi_channels_init(void);

/* Claims the highest free channel in the pool (keeps the zone a
 * contiguous count from the bottom). False if Song already holds
 * TILES_MIDI_SONG_MAX_CONCURRENT. */
bool tiles_midi_channels_song_claim(uint8_t *out_channel);

/* Releases a channel from the claim above. Releasing one that isn't held
 * is a no-op, so callers can release defensively on every exit path. */
void tiles_midi_channels_song_release(uint8_t channel);

/* How many pool channels Song holds. */
uint8_t tiles_midi_channels_song_in_use_count(void);

/* True if Song holds `channel` (a nibble). Live MPE skips these inside the
 * declared zone, which can briefly include a channel Song just took. */
bool tiles_midi_channels_song_holds(uint8_t channel);

/* The zone's real size now: free channels counting up from channel 2,
 * 0..TILES_MIDI_SHARED_POOL_SIZE. What the NEXT declaration will say. */
uint8_t tiles_midi_channels_lower_zone_size(void);

/* Records that the zone is being declared now at its real size and returns
 * that size for tiles_midi_mpe_init(). Every RPN 6 goes through this, so
 * the declared size always matches the wire. */
uint8_t tiles_midi_channels_declare_zone(void);

/* The size the receiver was last told, which is what the channel
 * allocator uses: a channel not declared as a member isn't one. */
uint8_t tiles_midi_channels_declared_zone_size(void);

/* True while the real size differs from the declared one (Song claimed or
 * freed a channel). services/expression.c re-declares once idle. */
bool tiles_midi_channels_zone_redeclare_pending(void);

/* Marks one pool channel as carrying (or no longer carrying) a live MPE
 * note. services/expression.c calls these wherever it claims, releases or
 * steals. Song's claim skips such channels, so a Song track starting
 * can't take a sounding note's channel. Doesn't change the zone size: a
 * note coming and going inside the declared zone is normal MPE. */
void tiles_midi_channels_note_channel_claimed(uint8_t channel);
void tiles_midi_channels_note_channel_released(uint8_t channel);
