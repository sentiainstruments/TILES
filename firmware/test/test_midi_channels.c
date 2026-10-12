/* services/midi_channels.c -- the shared pool (Song mode <-> live MPE Lower Zone),
 * its "claim the highest free first" invariant, the contiguous zone-size math, and
 * the declared-vs-honest zone size tracking.
 *
 * Convention (matches the rest of the firmware -- see midi/midi_out.h's own header):
 * every channel value here is a raw 0-15 NIBBLE, not a 1-indexed MIDI channel number
 * -- nibble 0 = MIDI channel 1. The shared pool is nibbles 1-8 (MIDI channels 2-9);
 * this test's own comments say "channel N" for a human reading the wire, but every
 * numeric literal is the nibble. */
#include "midi_channels.h"

#include <assert.h>
#include <stdio.h>

static void reset(void) { tiles_midi_channels_init(); }

int main(void) {
    uint8_t ch;

    /* 1. fixed channels are exactly the seven documented values, all distinct, none
     * inside the shared pool (2-9); drums are on 10 (GM percussion). */
    uint8_t fixed[] = {TILES_MIDI_CH_SEQ_LANE_0, TILES_MIDI_CH_SEQ_LANE_1, TILES_MIDI_CH_SEQ_LANE_2,
                       TILES_MIDI_CH_SEQ_LANE_3, TILES_MIDI_CH_CHORD, TILES_MIDI_CH_GAME, TILES_MIDI_CH_DRUMS};
    assert(TILES_MIDI_CH_SEQ_LANE_0 == 15 && TILES_MIDI_CH_SEQ_LANE_1 == 14 && TILES_MIDI_CH_SEQ_LANE_2 == 13 &&
           TILES_MIDI_CH_SEQ_LANE_3 == 12 && TILES_MIDI_CH_CHORD == 11 && TILES_MIDI_CH_GAME == 10);
    assert(TILES_MIDI_CH_DRUMS == 9); /* nibble 9 = MIDI channel 10 */
    for (int i = 0; i < 7; i++) {
        assert(fixed[i] < TILES_MIDI_SHARED_POOL_FIRST || fixed[i] >= TILES_MIDI_SHARED_POOL_FIRST + TILES_MIDI_SHARED_POOL_SIZE);
        for (int j = 0; j < 7; j++) assert(i == j || fixed[i] != fixed[j]);
    }
    /* the shared pool (2-9, i.e. nibbles 1-8) plus the 7 fixed channels (channel 10 among them)
     * plus the master (nibble 0) account for exactly all 16 channels, no gaps, no overlap. */
    assert(TILES_MIDI_SHARED_POOL_FIRST == 1 && TILES_MIDI_SHARED_POOL_SIZE == 8);

    /* 2. boot: full zone (8), nothing claimed, the declared size already matches
     * (the boot declaration is always the full 8), nothing pending. */
    reset();
    assert(tiles_midi_channels_lower_zone_size() == 8);
    assert(tiles_midi_channels_song_in_use_count() == 0);
    assert(tiles_midi_channels_declared_zone_size() == 8);
    assert(!tiles_midi_channels_zone_redeclare_pending());
    assert(tiles_midi_channels_declare_zone() == 8);

    /* 3. claims come from the TOP of the pool (channel 9 first), shrinking the
     * zone by exactly one per claim, in order. */
    reset();
    uint8_t expect_nibble[] = {8, 7, 6, 5, 4, 3, 2, 1}; /* MIDI channels 9,8,7,6,5,4,3,2 */
    for (int i = 0; i < 8; i++) {
        assert(tiles_midi_channels_song_claim(&ch));
        assert(ch == expect_nibble[i]);
        assert(tiles_midi_channels_lower_zone_size() == (uint8_t)(7 - i));
        assert(tiles_midi_channels_song_in_use_count() == (uint8_t)(i + 1));
    }
    assert(tiles_midi_channels_lower_zone_size() == 0);
    /* the 9th claim fails cleanly -- output untouched, not corrupted */
    ch = 0xAA;
    assert(!tiles_midi_channels_song_claim(&ch) && ch == 0xAA);

    /* 4a. releasing the BOTTOM-held channel (nibble 1 = channel 2, the one the
     * contiguous-from-channel-2 scan checks first) grows the zone immediately. */
    reset();
    for (int i = 0; i < 8; i++) { tiles_midi_channels_song_claim(&ch); }
    tiles_midi_channels_song_release(1); /* nibble 1 = channel 2 */
    assert(tiles_midi_channels_lower_zone_size() == 1 && tiles_midi_channels_song_in_use_count() == 7);

    /* 4b. releasing a channel ABOVE still-held lower channels does NOT grow the
     * zone yet -- a hole at nibble 8 (channel 9) isn't reachable as part of a
     * contiguous run from channel 2 while nibbles 7..1 are all still held. The
     * gap self-heals on the NEXT claim instead (test 5 below covers that), not
     * on the release itself -- a release only ever grows the zone if it happens
     * to be exactly the current bottom boundary. */
    reset();
    for (int i = 0; i < 8; i++) { tiles_midi_channels_song_claim(&ch); }
    tiles_midi_channels_song_release(8); /* nibble 8 = channel 9, the very first one claimed */
    assert(tiles_midi_channels_lower_zone_size() == 0 && tiles_midi_channels_song_in_use_count() == 7);

    /* 4c. releasing something already released, or never claimed, or entirely
     * outside the pool, is a harmless no-op -- never corrupts the count. */
    tiles_midi_channels_song_release(8); /* already released -- no-op, not a double-decrement */
    assert(tiles_midi_channels_song_in_use_count() == 7);
    reset();
    tiles_midi_channels_song_release(1); /* nibble 1 = channel 2, never claimed at all -- no-op */
    assert(tiles_midi_channels_song_in_use_count() == 0);
    tiles_midi_channels_song_release(200); /* not even in the pool -- must not corrupt anything */
    assert(tiles_midi_channels_song_in_use_count() == 0 && tiles_midi_channels_lower_zone_size() == 8);

    /* 5. a claim always re-fills the numerically highest currently-free channel,
     * even after a gap opens up in the middle -- this is what keeps the zone's
     * own math (a plain contiguous count from the bottom) correct without ever
     * having to check for holes. */
    reset();
    for (int i = 0; i < 5; i++) tiles_midi_channels_song_claim(&ch); /* holds nibbles 8,7,6,5,4 = channels 9,8,7,6,5 */
    tiles_midi_channels_song_release(6);                            /* a hole opens at nibble 6 (channel 7) */
    assert(tiles_midi_channels_song_claim(&ch) && ch == 6);         /* refilled exactly the hole, not nibble 3 */
    assert(tiles_midi_channels_lower_zone_size() == 3);             /* nibbles 3,2,1 (channels 4,3,2) still contiguous-free */

    /* 6. a genuinely free channel ABOVE a still-held lower one must not count toward
     * the zone: hold every nibble, then free only nibble 4 (channel 5) -- 1,2,3
     * (channels 2,3,4) stay held underneath it. Zone must read 0, not "1 free
     * channel exists somewhere" -- proves the "stop at the first in-use slot
     * counting from the bottom" logic, not a naive total-free-count (which would
     * wrongly report 1 here and tell expression.c channel 5 is safe to hand a live
     * touch when channels 2-4, right below it, are still someone else's). */
    reset();
    for (int i = 0; i < 8; i++) tiles_midi_channels_song_claim(&ch); /* holds every nibble, 8 down to 1 */
    tiles_midi_channels_song_release(4);                            /* channel 5 frees up; 1,2,3 stay held */
    assert(tiles_midi_channels_lower_zone_size() == 0);
    assert(tiles_midi_channels_song_in_use_count() == 7);

    /* 7. declared vs honest size: a Song claim leaves the DECLARED size alone (the
     * receiver hasn't been told yet) and flags a re-declaration; a claim+release
     * that cancel out leave nothing pending; declaring catches the declared size
     * up in one step however many claims happened in between. */
    reset();
    tiles_midi_channels_song_claim(&ch);
    tiles_midi_channels_song_release(ch);
    assert(!tiles_midi_channels_zone_redeclare_pending()); /* net zero -- 8 the whole time */
    tiles_midi_channels_song_claim(&ch);
    assert(ch == 8 && tiles_midi_channels_song_holds(8) && !tiles_midi_channels_song_holds(7));
    assert(tiles_midi_channels_zone_redeclare_pending());  /* real change: 8 -> 7 */
    assert(tiles_midi_channels_declared_zone_size() == 8); /* still what the receiver was told */
    tiles_midi_channels_song_claim(&ch);
    tiles_midi_channels_song_claim(&ch);
    assert(tiles_midi_channels_declare_zone() == 5);
    assert(tiles_midi_channels_declared_zone_size() == 5);
    assert(!tiles_midi_channels_zone_redeclare_pending());
    tiles_midi_channels_song_release(8);                    /* the top one: nibbles 7,6 still held below it */
    assert(!tiles_midi_channels_zone_redeclare_pending());  /* -> zone stays 5, nothing to re-declare */
    tiles_midi_channels_song_release(6);                    /* the bottom boundary: zone grows to 6 */
    assert(tiles_midi_channels_zone_redeclare_pending() && tiles_midi_channels_lower_zone_size() == 6);
    assert(tiles_midi_channels_song_holds(200) == false);   /* outside the pool: never "held" */

    /* 8. full round trip: claim all 8, release all 8 (in claim order, i.e. NOT
     * highest-first), zone returns to a clean, contiguous 8 with no residue. */
    reset();
    uint8_t claimed[8];
    for (int i = 0; i < 8; i++) tiles_midi_channels_song_claim(&claimed[i]);
    for (int i = 0; i < 8; i++) tiles_midi_channels_song_release(claimed[i]);
    assert(tiles_midi_channels_lower_zone_size() == 8 && tiles_midi_channels_song_in_use_count() == 0);
    /* and the pool is fully reusable afterwards, same top-down order as a fresh boot */
    assert(tiles_midi_channels_song_claim(&ch) && ch == 8); /* nibble 8 = channel 9 again */

    /* 9. THE cross-module invariant: a channel a live MPE note is using
     * right now can never be handed to Song, even though nothing about a
     * live claim touches Song's own bookkeeping directly. Claim every
     * channel as a "live note" (mirroring what expression.c would do),
     * then confirm Song gets nothing at all -- not a collision, a clean
     * "pool full." */
    reset();
    for (uint8_t nibble = 1u; nibble <= 8u; nibble++) tiles_midi_channels_note_channel_claimed(nibble);
    ch = 0xAA;
    assert(!tiles_midi_channels_song_claim(&ch) && ch == 0xAA);
    assert(tiles_midi_channels_lower_zone_size() == 8); /* Song's OWN footprint is still empty -- the zone is unaffected */

    /* 10. a single live-held channel in the middle of the pool is skipped,
     * not collided with -- Song ends up holding every OTHER channel, never
     * that one, and the skip is invisible to zone size (Song's own
     * footprint, not live activity, is what the zone reports). */
    reset();
    tiles_midi_channels_note_channel_claimed(5); /* channel 5 is "live" */
    int got_five = 0;
    for (int i = 0; i < 7; i++) {
        assert(tiles_midi_channels_song_claim(&ch));
        assert(ch != 5);
        if (ch == 5) got_five = 1;
    }
    assert(!got_five);
    ch = 0xAA;
    assert(!tiles_midi_channels_song_claim(&ch) && ch == 0xAA); /* the 8th (channel 5) is genuinely unavailable */

    /* 11. releasing the live hold lets Song claim it afterwards -- the skip
     * was never permanent. */
    tiles_midi_channels_note_channel_released(5);
    assert(tiles_midi_channels_song_claim(&ch) && ch == 5);

    /* 12. the reverse direction still holds after all this: live MPE's own
     * zone-size math is untouched by any of the live-note marking above
     * (only Song's real claims move it), so marking/clearing "live" on a
     * channel Song doesn't hold at all is a pure no-op on the zone. */
    reset();
    uint8_t before = tiles_midi_channels_lower_zone_size();
    tiles_midi_channels_note_channel_claimed(3);
    assert(tiles_midi_channels_lower_zone_size() == before);
    tiles_midi_channels_note_channel_released(3);
    assert(tiles_midi_channels_lower_zone_size() == before);

    printf("midi_channels: all tests pass\n");
    return 0;
}
