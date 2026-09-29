#pragma once

/*
 * The single source of truth for how this board's one MIDI port divides its
 * 16 channels -- which nibbles are permanently a fixed part's own channel,
 * which are shared between Song mode and live MPE polyphony, and how big
 * the live MPE zone honestly is right now. Everything in this file is pure
 * arithmetic over a few small pieces of state (no MIDI I/O, no Pico SDK),
 * tested natively (firmware/test/test_midi_channels.c) before anything that
 * actually sends a note trusts it.
 *
 * Real feedback: "i feel like the midi channel asignement is weirtd and not
 * consistent or standaerd for industry compatibility" -> "large [rework],
 * and since layouts work like apps we know ableton mode and song mode cant
 * run at once so compensate mappigns fotr that. we need it to be robust so
 * do good research on protocols. we want ease of use and consistency" ->
 * (on scope) "we need mpe to work well and fully but we also want to be
 * able to control at least 6 channels or devices independently... look for
 * what would be mosty logical and standard and do that."
 *
 * This replaces a scattered set of one-off constants and ad hoc "is this
 * channel already somebody else's" checks that had accumulated feature by
 * feature (services/op_mode.c's own OP_CHORD_CHANNEL/s_seq_lane_channel[],
 * services/game_mode.c's own GM_MELODY_CHANNEL, services/expression.c's own
 * harmonic_channel_is_reserved()) -- two of which had a REAL gap found
 * auditing this: a harmonic voice could claim a channel a running sequencer
 * lane was actively using (claim_harmonic_channel() never checked the
 * sequencer's own reservation), and a live melodic touch in chord mode's
 * "mini melodic" sub-grid could claim chord mode's own fixed channel out
 * from under its currently-sounding chord voicings (claim_mpe_channel()
 * never checked chord's reservation at all). Both were possible because
 * each feature's channel was "reserved" by its own separate, easy-to-miss
 * condition scattered across two files, checked ad hoc by whoever happened
 * to remember to. This file makes that structurally impossible instead of
 * relying on every future caller remembering every check: the fixed parts'
 * channels are OUTSIDE the range live MPE is ever allowed to scan at all,
 * by construction, not excluded by a checklist.
 *
 * ==== Layout (nibbles; "channel N" = nibble N-1 everywhere else in this
 * codebase's own convention -- see midi/midi_out.h's header) ====
 *
 *   Channel  1        MPE Zone Master. RPN only, never a note. Fixed by
 *                     the MPE spec (MMA, "MIDI Polyphonic Expression"
 *                     v1.1): https://midi.org/mpe-midi-polyphonic-expression
 *   Channels 2-9      SHARED POOL (8 channels) -- see below.
 *   Channel  10       NEVER ASSIGNED. General MIDI reserves channel 10 for
 *                     percussion (Wikipedia "General MIDI"; MIDI.org forum
 *                     "General Midi Level 2 ch 11 percussion") -- a GM-
 *                     aware receiver reinterprets whatever note numbers
 *                     land there as drum hits regardless of what was
 *                     meant, which is real, common, and worth avoiding by
 *                     construction rather than hoping nothing downstream
 *                     defaults to GM.
 *   Channel  11       Game mode's own fixed channel (TILES_MIDI_CH_GAME).
 *   Channel  12       Chord mode's own fixed channel (TILES_MIDI_CH_CHORD).
 *   Channels 13-16    The 4 regular sequencer lanes, one each
 *                     (TILES_MIDI_CH_SEQ_LANE_0.._3), lane 0 = channel 16.
 *
 * Channels 11-16 (game, chord, the 4 lanes -- 6 total) are PERMANENT: a
 * given lane/chord/game always uses the exact same channel whether or not
 * it's currently sounding anything, so a player who has patched an
 * external synth to "TILES sequencer lane 2's channel" can rely on that
 * assignment never moving depending on what else happens to be running.
 * This satisfies "at least 6 channels or devices independently [addressable
 * and controllable]" on its own, before Song mode's own pool below is even
 * considered -- deliberately NOT conditional on "is chord mode currently
 * active" the way the two channels used to be reserved, which is exactly
 * the shape that let the "mini melodic" collision above happen: permanent,
 * unconditional exclusion removes that whole bug class rather than adding
 * one more condition to remember.
 *
 * ==== The shared pool (channels 2-9) ====
 *
 * Split, at any instant, between Song mode's own slots (services/op_mode.c,
 * "we can assign a costume midi channel for each bank kinda like a
 * looper") and the live MPE Lower Zone (services/expression.c's
 * claim_mpe_channel(), genuine polyphonic melodic/chord/guitar touches).
 * Both draw from the SAME 8 channels because the two needs are mutually
 * exclusive in total size, not because either is arbitrarily capped: every
 * channel Song is using right now is one fewer available to a live touch,
 * and vice versa, which is the honest picture of a single 16-channel port,
 * not two independently-sized pretend budgets that happen to add up
 * correctly on paper.
 *
 * Song claims from the TOP of this pool downward (channel 9 first),
 * exactly like it already did before this file existed
 * (op_mode.c's own s_song_channel_pool, unchanged in spirit) -- a claim
 * always takes the highest currently-free channel, so whatever Song
 * currently holds is always the TOP-K channels of the pool, which is what
 * makes the live MPE zone's own math below trivial: the zone is simply
 * "however many channels, counting up from channel 2, are still free."
 * Live MPE's declared zone can therefore NEVER be a channel Song already
 * holds (Song only ever holds a prefix from the top; the zone only ever
 * claims a prefix from the bottom; the two meet somewhere in the middle
 * and never cross), so this needs no separate collision check either --
 * same "structurally impossible, not remembered" property as the fixed
 * channels above.
 *
 * Song's own historical cap was 9 simultaneous tracks (real feedback:
 * "9 song tracks, 1 channel stays free for live MPE" -- from when chord
 * and game's channels were only conditionally reserved and could
 * sometimes be borrowed). That borrowing is exactly the bug-prone shape
 * this file removes, so Song's real ceiling is now 8, the size of the
 * pool it actually shares with live MPE -- see this header's own note in
 * the real-feedback trail in services/README.md for why that's an
 * accepted, deliberate trade for the robustness this file buys.
 *
 * ==== The live MPE zone's size, and keeping the receiver honestly told ====
 *
 * The MPE Configuration Message (RPN 6, sent on the Master Channel) is
 * defined by the spec as "Master Channel plus a COUNT of ascending Member
 * Channels" -- a receiver is told exactly how large the zone is, not
 * always "15" regardless of reality (MIDI.org community, "How MIDI MPE
 * pitch bend works": "sending an RPN 6 value of 10 ... configure an
 * instrument to use channels 2-11 for notes"; JUCE's own MPE tutorial:
 * "An MPE zone can be turned off by sending an MCM without any member
 * channels"). Before this file, TILES always declared 15 at boot and
 * never updated or withdrew that declaration -- meaning a strict MPE
 * receiver had no way to know some of "its" 15 channels might actually be
 * carrying a sequencer lane or a chord voicing, and turning MPE off in
 * TILES's own runtime setting never told the receiver its zone was gone.
 * tiles_midi_channels_lower_zone_size() below is the SAME number
 * services/expression.c declares via RPN 6, and sends as 0 (withdrawing the
 * zone entirely) while MPE is toggled off -- see expression.c's own
 * tiles_expression_set_mpe_enabled().
 *
 * WHEN it re-declares matters as much as what. Real feedback: "what else
 * does it look like we need to fix for standarization and compatibility" ->
 * "do all." This used to re-send RPN 6 the moment Song mode claimed or freed
 * a channel -- mid-performance. The MPE spec has a receiver stop every note
 * and reset every controller on a channel that enters or leaves a zone
 * (section 2.1.4), and JUCE-based synths (ROLI Equator among them) also
 * reset the zone's pitch-bend range to the spec's 48-semitone default on
 * every RPN 6 -- the re-declaration was sent WITHOUT the RPN 0 that follows
 * it at boot, so after a Song claim the synth was back at 48 semitones. So
 * the receiver is now only told about a new size when nothing is sounding
 * (services/expression.c's mpe_zone_is_idle()), always as the full
 * declaration (RPN 6 plus RPN 0), and until then live MPE keeps using the
 * size the receiver was LAST told (tiles_midi_channels_declared_zone_size())
 * minus whatever Song holds inside it (tiles_midi_channels_song_holds()).
 *
 * Because the zone must stay a single CONTIGUOUS run starting at channel 2
 * (the spec's Lower Zone shape -- there's no "channels 2-5 and 11-16 but
 * not 6-10" zone), and channel 10 can never be a member, its maximum
 * possible size is a hard 8 (channels 2-9), never 15 -- a direct,
 * unavoidable consequence of honoring the GM channel-10 convention
 * *and* staying spec-correct, not a choice this file makes lightly. A
 * spec-legal Upper Zone (Master channel 16, members descending from 15)
 * running alongside the Lower Zone could theoretically recover a few more
 * (up to 5, channels 11-15, before hitting the same channel-10 wall from
 * the other side) at the cost of a second, independent zone declaration a
 * receiver has to understand as one instrument -- real, spec-legal, more
 * fragile in practice, and NOT built here; worth revisiting only if 8
 * genuinely proves too few in real playing.
 */

#include <stdbool.h>
#include <stdint.h>

/* Permanent, unconditional -- see this file's own header for why these six
 * are never contested and never shared, regardless of whether the feature
 * that owns one is currently sounding anything. */
#define TILES_MIDI_CH_SEQ_LANE_0 15u /* channel 16 */
#define TILES_MIDI_CH_SEQ_LANE_1 14u /* channel 15 */
#define TILES_MIDI_CH_SEQ_LANE_2 13u /* channel 14 */
#define TILES_MIDI_CH_SEQ_LANE_3 12u /* channel 13 */
#define TILES_MIDI_CH_CHORD 11u      /* channel 12 */
#define TILES_MIDI_CH_GAME 10u       /* channel 11 */

/* Never assigned to anything -- General MIDI's percussion channel. See
 * this file's own header comment. */
#define TILES_MIDI_CH_PERCUSSION_EXCLUDED 9u /* channel 10 */

/* The shared pool: nibbles 1-8 (channels 2-9), split between Song mode and
 * the live MPE Lower Zone -- see this file's own header comment. */
#define TILES_MIDI_SHARED_POOL_FIRST 1u /* channel 2 */
#define TILES_MIDI_SHARED_POOL_SIZE 8u

/* Song mode may hold at most this many of the shared pool's 8 channels at
 * once (today, all 8 -- Song is the only other claimant, so it can use
 * everything live MPE isn't using this instant). A named constant rather
 * than a bare reuse of TILES_MIDI_SHARED_POOL_SIZE so a future cap
 * ("always leave at least 1 for live MPE," matching the old design's own
 * stated floor) is a one-line change here, not a hunt through op_mode.c. */
#define TILES_MIDI_SONG_MAX_CONCURRENT TILES_MIDI_SHARED_POOL_SIZE

void tiles_midi_channels_init(void);

/* Claims the highest-numbered currently-free channel in the shared pool
 * (see this file's own header on why "highest first" is what keeps the
 * live MPE zone's own math a simple contiguous count). False (channel
 * untouched) if all TILES_MIDI_SONG_MAX_CONCURRENT are already claimed. */
bool tiles_midi_channels_song_claim(uint8_t *out_channel);

/* Releases a channel this module previously handed out via the claim
 * above. A channel not currently claimed (already released, or never
 * claimed) is a harmless no-op -- callers that release defensively on
 * every possible exit path (this codebase's own established pattern for
 * "never strand a claim") don't need their own bookkeeping to avoid a
 * double-release. */
void tiles_midi_channels_song_release(uint8_t channel);

/* How many of the shared pool's 8 channels Song currently holds. */
uint8_t tiles_midi_channels_song_in_use_count(void);

/* True if Song mode currently holds `channel` (a nibble). Live MPE skips
 * these inside the declared zone -- see this file's header on why the
 * declared zone can briefly still include a channel Song has just taken. */
bool tiles_midi_channels_song_holds(uint8_t channel);

/* The live MPE Lower Zone's honest size right now: how many channels,
 * counting up from channel 2 (nibble TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL),
 * are NOT currently held by Song mode -- 0..TILES_MIDI_SHARED_POOL_SIZE.
 * What the NEXT declaration will say -- see tiles_midi_channels_declare_
 * zone() below. */
uint8_t tiles_midi_channels_lower_zone_size(void);

/* Records that the zone is being declared to the receiver right now, at
 * its current honest size, and returns that size for the caller to send
 * (tiles_midi_mpe_init()). Every RPN 6 this board sends goes through
 * this, so tiles_midi_channels_declared_zone_size() always matches the
 * wire. */
uint8_t tiles_midi_channels_declare_zone(void);

/* The size the receiver was last told -- what services/expression.c's
 * channel allocator scans (skipping Song-held channels inside it), since a
 * channel the receiver hasn't been told is a Member Channel isn't one. */
uint8_t tiles_midi_channels_declared_zone_size(void);

/* True while the honest size differs from the declared one -- Song claimed
 * or freed a channel since the last declaration. services/expression.c
 * re-declares once nothing is sounding (see this file's header). */
bool tiles_midi_channels_zone_redeclare_pending(void);

/* Marks/clears one shared-pool channel (a nibble in 1..8) as currently
 * carrying a live MPE note. services/expression.c calls this at every
 * point it flips that channel's own in_use bookkeeping (claim, release,
 * steal) -- see that file's claim_mpe_channel()/end_held_note()/etc.
 *
 * Why this exists: tiles_midi_channels_song_claim() and expression.c's own
 * live-note bookkeeping are two INDEPENDENT records of "who's using which
 * shared-pool channel right now." Without this, the two could never learn
 * about each other -- a live note claiming channel 9 doesn't touch this
 * module's own state at all (the whole design deliberately keeps the
 * declared zone size, below, from flickering on every note-on/off), so a
 * Song track starting at that exact moment would have no way to know
 * channel 9 was already spoken for, and could claim it out from under the
 * live note anyway -- exactly the class of bug this entire rework exists
 * to eliminate, just at the boundary between these two systems instead of
 * within either one alone. tiles_midi_channels_song_claim() checks this
 * and skips a live channel in favor of the next one down; it does NOT
 * affect tiles_midi_channels_lower_zone_size() (that stays based purely on
 * Song's own footprint -- a live note coming and going within an already-
 * declared zone is normal MPE behavior, not a structural boundary change
 * a receiver needs to be told about via a fresh RPN 6). */
void tiles_midi_channels_note_channel_claimed(uint8_t channel);
void tiles_midi_channels_note_channel_released(uint8_t channel);
