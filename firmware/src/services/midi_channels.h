#pragma once

/* How TILES divides the 16 MIDI channels (tested in test_midi_channels.c).
 * Fixed parts sit outside anything live MPE may use, so they can't collide.
 *   1      MPE Master: RPNs and pedals (MPE off: every live note)
 *   2-9    shared pool: Song tracks claim from 9 down, the live MPE zone
 *          counts up from 2, so they never overlap
 *   10     drum sequencer (GM percussion)
 *   11     game mode, Bouncing notes and the physics modes
 *   12     chord mode
 *   13-16  sequencer lanes (lane 0 = 16)
 * Channels 10-16 are permanent. A zone change (RPN 6 + RPN 0) stops every
 * note on the receiver, so a new size is declared only when nothing sounds;
 * until then live MPE uses the last declared size minus Song's channels. */

#include <stdbool.h>
#include <stdint.h>

/* Permanent: never shared, whether or not the part is sounding. */
#define TILES_MIDI_CH_SEQ_LANE_0 15u /* channel 16 */
#define TILES_MIDI_CH_SEQ_LANE_1 14u /* channel 15 */
#define TILES_MIDI_CH_SEQ_LANE_2 13u /* channel 14 */
#define TILES_MIDI_CH_SEQ_LANE_3 12u /* channel 13 */
#define TILES_MIDI_CH_CHORD 11u      /* channel 12 */
#define TILES_MIDI_CH_GAME 10u       /* channel 11 */
#define TILES_MIDI_CH_BOUNCE TILES_MIDI_CH_GAME /* bouncing notes (bounce.h) */

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

/* Marks a pool channel as carrying a live MPE note, so Song's claim skips
 * it. Doesn't change the zone size. */
void tiles_midi_channels_note_channel_claimed(uint8_t channel);
void tiles_midi_channels_note_channel_released(uint8_t channel);
