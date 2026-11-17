# TILES's own sound

The plan for the sound TILES makes itself, on the chip it already has
(RP2350 on the Pico 2). It runs in standalone mode only; controller mode
leaves core 1 off and USB without audio (`firmware/src/services/README.md`,
"Controller and standalone mode"). Built so far: the plumbing and a placeholder
test voice streamed as a USB audio input (Alpha 7,
`firmware/src/audio/README.md`). TILES v1 (First Edition) adds a stereo
DAC and one phones/line jack (`../hardware/audio-output.md`).

Agreed 2026-11-11. Cycle figures are **measured on unit 2** the same day
(the DSP stress test, below), not estimates.

## Budget

| | Today | Notes |
|---|---|---|
| CPU | core 1 only: 3,125 cycles per sample at 150 MHz / 48 kHz; voices are let in up to 85 % and the live guard holds 90 % (Guardrails) | core 0 scans the pads and stays as it is (its loop measured the same, 2.2 ms, during the test; flash reads from core 1 slow it, see Drum samples) |
| RAM | ~275 KB free of 520 KB | ~355 KB in release builds: the 80 KB sensor-capture buffer is a development tool and is compiled out |
| Flash | ~3.7 MB free of 4 MB (the image is ~330 KB) | 1.5 MB for the 8 drum sample slots, ~1 MB kept for code, the rest for wavetables; not a full sampled piano |

## Tracks

Four tracks plus drums play at once, each track one engine (its own
sound). A sequencer lane plays on its track; a mode played live takes
over a track and borrows its lane's voices.

| Track | Sequencer lane | Played on it (and takes the lane's voices) |
|---|---|---|
| 1 | lane 1 | nothing: the sequencer's own |
| 2 | lane 2 | melodic, Duet's melody, bass guitar, Song's tracks (the MPE channels) |
| 3 | lane 3 | Prism, Duet's chord columns, the chord strip (channel 12) |
| 4 | lane 4 | the physics modes, bouncing notes, games (channel 11) |
| Drums | the drum lanes | drum mode and Volca mode: the sampler, 8 sample slots, 5 ringing at once |

How many synth voices sound at once is set by what they cost, not by a
count: see Guardrails. At most 8 on one track.

## Guardrails

Every engine and effect pushed as far as the core allows, and the sound
never breaks up.

1. **Reserved first: the effects and 5 drums.** Reverb, delay, chorus,
   the inserts and the master, plus 5 drum samples ringing, are kept
   1,200 cycles a sample (the built effects measure ~700 shared plus
   ~120 a driven track; drum samples ~65 each). Voices never take it,
   so the effects stay at full quality. The sampler rings 5 at most
   (`TILES_AUDIO_DRUM_VOICES`).
2. **Voices share the rest by cost.** Every voice counts its engine's
   cycles per sample, and all of them together stay under 85 % of core 1
   (1,456 cycles for voices at 150 MHz). Each track's cost starts from
   its engine's measured figure and then follows live timing: every
   block, core 1 times each track's voices. An engine people program
   needs no figure up front; it is measured as it plays.
3. **A note that doesn't fit takes room**, as many voices as it needs: a
   full track (8) gives from itself first, so a played note takes its
   lane's voice. Otherwise a releasing tail anywhere, this track's oldest
   sequencer note, the oldest sequencer note anywhere, then (for a played
   note) the oldest played note on the busiest track. A sequencer note
   never takes a played one; if it can't fit it is skipped and takes
   nothing. A heavy FM note may take two wavetable voices' room. A taken
   voice fades over 5 ms.
4. **The live guard holds 90 %.** The voices' load, smoothed over ~8
   16-frame blocks (2.7 ms, so a USB interrupt in one block isn't an
   overload), over it three blocks running ends the quietest voice over
   5 ms: the worst case is a shortened tail, never a click in a
   recording. A voice fading out is left out of the load (counted, the
   guard ended another while it faded: a 7-voice chord lost 6). The
   engine's cost follows, so fewer voices are let in after. A block a
   flash write paused is neither counted nor learned from. `audio.shed`
   and `audio.cost` in INFO show it working.
5. **Saves never break the sound.** No erase while the clock runs or the
   sound rings; pages go out at least 8 ms apart, so at 90 % core 1
   refills its 3 ms of audio between them.

Built in `firmware/src/audio/audio_engine.c` (with the test voice),
tested in `test/test_audio_engine.c`.

**What fits: measured on unit 2, 150 MHz**, voices dealt over the four
tracks, 5 stereo drum samples ringing, reverb + delay + chorus + master,
at 85 % (`cap_*` in the stress test; samples from RAM):

| Every voice | Voices at once | Each costs |
|---|---|---|
| FM, 6 operators | 7 | ~215 |
| Regular (analog, filter at control rate) | 8 | ~188 |
| Wavetable | 14 | ~105 |
| A third each of the three | 9 | ~169 |

What raises it, roughly a voice per ~105-215 cycles freed:

- **Chorus off:** ~230 cycles (+1-2). It stays optional per sound.
- **Faster FM** (fixed-point operators, envelopes at control rate):
  untested.
- **Clock:** 153.6 MHz (v1) gives voices 1,620 cycles (about FM 7,
  regular 8, wavetable 15, mixed 9); a qualified 200 MHz gives 2,442
  (about FM 11, regular 12, wavetable 23, mixed 14). Worked out from the
  cycle counts, not run.

## Engines

The engines, their sound direction and techniques, and what they measure:
`synth-engines.md`. The wavetable engine is built (2026-11-14). The web
player's sampled grand piano (hundreds of MB) doesn't fit; a modelled or
FM piano takes its place. Sound names stay Sentia's own (no instrument or
product names).

## Measured cost

Cycles per output sample at 150 MHz, measured on unit 2 with the
instrument loop running on core 0, two runs within 2 % of each other (a
third run, adding six-operator FM and sample drums, repeated the earlier
scenes within 1 %).
High-quality float prototypes (`firmware/src/audio/dsp_bench.c`), run
from RAM.

**Voices** (per voice)

| Kernel | Cycles |
|---|---|
| Wavetable: 8 frames x 2048, morph by pressure, SVF low-pass | ~112 |
| Analog: 2 anti-aliased saws, zero-delay 4-pole ladder, 2 envelopes, LFO; filter worked out every sample | ~325 |
| The same analog voice, filter worked out every 8 samples (6 kHz control rate) | ~192 |
| The same analog voice run from flash instead of RAM | ~670 |
| FM: 4 operators in two stacks with feedback (not yet optimized) | ~190 |
| FM: 6 operators (DX7-style), one algorithm written out, envelopes every sample | ~267 |
| FM: 6 operators, any algorithm from a routing table, one operator over a 32-sample block at a time, envelopes per block | ~222 |
| Plucked string: Karplus-Strong with allpass tuning | ~69 |
| Granular: one grain (window, interpolated read) | ~75 |

**Drums** (per sounding hit): kick ~155 (~135 with its state in
registers), snare ~160, clap ~125; hat from six anti-aliased squares
~460, from filtered noise ~130. **Sample drums** (per sounding slot,
16-bit mono, tuned by linear interpolation): ~65 from RAM, ~113 read
straight from flash through the XIP cache, ~126 from flash uncached.

**Effects** (per 48 kHz sample): the prototypes, plate reverb run at
24 kHz ~400, ping-pong delay at 24 kHz ~160, chorus ~210, master (clip
and 24-bit output) ~100-130. The built effects (`synth-engines.md`,
"Effects"): plate at 48 kHz 473, delay 118, ensemble 103, overdrive 122
a driven track.

**Everything at once** (the real test; the parts overlap a little, so
this is less than their sum)

| Scene | Cycles | Core 1 |
|---|---|---|
| Drums (kick, snare, hat) + chorus + delay + reverb, no voices | ~1,380 | 44 % |
| Wavetable 8 + plucked 4 + drums + all effects | ~2,440 | **78 %** |
| Analog (filter every 8 samples) 8 + plucked 4 + drums (noise hat) + all effects | ~2,750 | **88 %** |
| FM 8 + plucked 4 + drums + all effects | ~3,090 | 99 % |
| Analog (filter every 8 samples) 12 + plucked 4 + the same | ~3,480 | 111 % |
| Analog (filter every sample) 8 + plucked 4 + the same | ~4,150 | 133 % |
| FM 6-op 12 + all effects, no drums (a 12-voice FM synth's load) | ~3,290 | 105 % |
| FM 6-op 8 + plucked 4 + drums (noise hat) + all effects | ~2,970 | 95 % |
| FM 6-op 8 + plucked 4 + all 8 sample drums from flash + all effects | ~3,520 | 113 % |
| Analog (filter every 8 samples) 8 + plucked 4 + all 8 sample drums from flash + all effects | ~3,340 | 107 % |

### What that means

- **12 voices across the tracks + drums + reverb + delay fits at
  150 MHz** (the scenes' 8 + 4) with wavetable and plucked sounds (78 %)
  and with the analog voice once its filter runs at control rate (88 %;
  ~80 % without the chorus).
- **FM** needs the same treatment (control-rate envelopes, state in
  registers) to come down to about 85 %.
- **Six-operator FM:** 12 voices with the effects need ~200 MHz (79 %);
  at 150 MHz it is 8 voices with drums at 95 %, about 6 inside the
  guard. Working block-wise saved only 17 %; fixed-point operators are
  the next thing to try (not measured).
- **Sample drums must not play straight from flash.** All 8 sounding
  cost ~900 cycles (29 % of the core), most of it waiting on flash,
  cached or not (8-byte cache lines, a new one every 4 samples), and the
  reads slow core 0: its loop went from 2,232 to ~2,380 us on average
  (max 2,789 to ~3,000-3,090 us) while 8 slots streamed in real time.
- **12 + 4 analog voices don't fit at 150 MHz** (111 %); at ~200 MHz the
  same scene would be about 83 %.
- The effects bus is a quarter to a third of the core; the reverb is the
  biggest single item.

### Rules the test settled

1. **All audio code runs from RAM, never inlined into flash code.** From
   flash the analog voice is 2.1x slower; kernels the compiler had folded
   into flash-resident callers measured about 2x slower too
   (`__no_inline_not_in_flash_func`).
2. **Filter coefficients at control rate** (every 8 samples): -40 % on
   the analog voice.
3. **No per-sample divisions** (a division-free clip), and voice state
   in registers for the block (`restrict` buffers, locals): 5-20 %.
4. **Drums are samples** (the sampler), not synthesized: five ringing
   cost less than three synthesized hits did, a metallic hat alone ~460.
5. A grain cloud is 12-16 grains (~900-1,200 cycles), not 24.
6. **Samples stream from flash into RAM by DMA** (the XIP stream
   FIFO, in long sequential reads), and a slot at its own pitch plays
   without interpolation. Measure both with the soak before settling.

## Effects

One shared bus after the voices, with a send level per sound (drums
lighter on reverb).

| Effect | RAM |
|---|---|
| Reverb: plate, run at 24 kHz (a tail needs no top octave) | ~73 KB (float lines) |
| Delay: stereo ping-pong, tempo-synced, feedback with tone; 2 s (a bar at 120 BPM) stored at 24 kHz, so repeats darken like tape | 192 KB |
| Chorus (optional) | ~8 KB |

About 270 KB of RAM in all: this is why the capture buffer leaves
release builds.

## Drum sampler

Drum mode is a sampler: every drum is a sample, played on the drum
track, 5 ringing at once. Everything TILES plays comes out in stereo;
a drum sample is stereo or mono (mono is panned per pad).

**8 slots**, one per kit sound, fixed size so any slot can be replaced
on its own: 192 KB each (three 64 KB erase blocks), 1.5 MB in all, at a
fixed place in flash below the saved data (`flash_map.h` gets the
region when the sampler lands). Stored as 16-bit at 48 kHz, so a sample
at its own pitch plays without resampling.

| A slot holds | Length cap |
|---|---|
| a stereo sample | 1 s |
| a mono sample | 2 s |

A longer sample is trimmed at the cap with a short fade; a long crash or
open hat fits best as mono. Typical one-shots: kick 0.6 s, snare and
clap 0.4 s, closed hat 0.15 s, open hat 0.8 s, rim 0.15 s, tom 0.6 s,
crash 2 s. The companion app converts what it loads (rate, depth,
length). Loading erases and writes flash, which pauses core 1, so it
only happens while nothing plays.

**Playing:** a pad's new hit restarts its own sample; a hit with 5
already ringing takes the oldest one's voice. Velocity sets the level
(squared, so soft hits are clearly softer). A slot's first 1 KB waits in
RAM, so a hit starts at once; the rest streams from flash into a 2 KB
ring per voice by DMA from the XIP stream FIFO, which reads in flash
idle cycles and leaves the cache to core 0 (read straight from flash
samples cost ~2x and slow core 0, see Measured cost). The flash-write
helper stops that DMA first. Next: per-slot level, tune (interpolation
only when tuned), pan, start, length and reverse, as playback settings
that never change the stored audio.

**Built (firmware side, 2026-11-12):** `firmware/src/audio/` (sampler,
sample_slot, sample_store, sample_stream), loading over the settings
link (`SAMPLE` in `shared/protocol/README.md`) and `tools/tiles_control.py
sample put|list|play|delete`. Tested on the computer, playback frame for
frame and a power cut at every step of a load; on unit 2, loads
(CRC-checked against flash), listing, slots kept through reflashing,
hits reaching core 1 and the stream with no underruns. Not yet heard:
the USB audio input stops on macOS (`firmware/src/audio/README.md`,
"Known issue").

**Data management, from what hardware samplers do:**

- **The computer converts, the instrument stores** (Elektron's Transfer,
  Korg's Librarian): rate, depth, channels and trimming happen before the
  load, so the device never resamples or runs out of time mid-load.
- **Store at the playback rate** (Digitakt: 48 kHz 16-bit): a hit at its
  own pitch is a copy, no interpolation.
- **Fixed slots, not a shared pool.** Volca Sample keeps 100 samples in
  one 4 MB pool and owners clear space before loading or hit "memory
  full"; a fixed slot can always be replaced, never fragments, and its
  cap is plain.
- **Playback settings change how a sample plays, not the sample**
  (Volca Sample's start, length, hi-cut, speed, reverse per part): the
  audio is written once, its settings anywhere.
- **Fail-safe writes** (NOR flash practice): the only copy of a slot is
  never half valid; the header is programmed last, after the CRC of what
  flash holds matches the sender's.
- **Don't waste the space:** silence trimmed at -60 dBFS, a stereo file
  with identical sides stored mono, dither when reducing to 16 bits.

**Cost, measured on unit 2:** 5 stereo samples ringing plus reverb,
delay, chorus and master take 1,074 cycles a sample (mono: 1,052;
5 synthesized drums: 1,210). A stereo voice costs the same as a mono one
(~65-70 cycles): one 32-bit read a frame.

Patches are small next to this: ~512 bytes a synth patch (128 = 64 KB),
~100 bytes a kit, kept with the saved data.

## The DSP stress test

`cmake -DTILES_DSP_BENCH=ON` builds it in (never in a release: it holds
180 KB of RAM); `tools/tiles_control.py bench` runs it on core 1 and
prints the cycles per sample of every kernel and scene, then core 0's
loop time while core 1 streams the 8 sample slots from flash in real
time (`BENCH SOAK`). Rerun it after any engine change and before
settling a voice count.

## Outputs and clocks

- **USB audio** (built): stereo 48 kHz / 24-bit input. On Rev A0 it
  follows the computer: each packet carries the frames the USB frames
  since the last one call for.
- **DAC on v1:** PIO I2S fed by DMA. The crystal becomes the audio
  clock: I2S paces core 1, and USB audio switches to true asynchronous
  packets (48 +- 1 frames by the ring level, TinyUSB's flow control), so
  the jack and the recording stay in step. Both get the same mix.
- **System clock on v1: 230.4 MHz** (48 kHz x 4,800) once qualified, for
  an exact I2S divider and twice the voices' budget; **153.6 MHz** the
  fallback (also exact). They are the only two exact clocks from 150 to
  300 MHz (`../hardware/audio-output.md`, "Firmware"). The RP2350 is rated
  150 MHz, so the chosen clock is qualified on the units first (stress,
  soak, flash read-back). DIN MIDI, the LED chains, I2C, SPI and the UART
  compute their dividers from `clock_get_hz()` and follow; timers run
  from the 1 MHz reference, USB and the ADC from their own PLL.
- **Volume** is digital, before the DAC, and starts low at power-up for
  headphones. The encoders have no function yet, so volume is a setting
  until they are mapped.

## Order of work

1. ~~Measure on a board~~: done 2026-11-11 (above).
2. Engine framework: an engine per track (the tracks and guardrails are
   built, with the test voice), the effects bus and per-sound sends,
   following the rules above.
3. Port the engines one at a time, wavetable first: built 2026-11-14
   (`synth-engines.md`, which has the next steps).
4. The drum sampler: built (firmware side and `sample put`). Left: per-slot
   playback settings, loading from the companion app, and hearing it once
   USB audio streams on macOS.
5. v1 bring-up: I2S, the clock change, mute and headphone detect.
