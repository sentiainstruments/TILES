# TILES's own sound

The plan for the sound TILES makes itself, on the chip it already has
(RP2350 on the Pico 2). Built so far: the plumbing and a placeholder
test voice streamed as a USB audio input (Alpha 7,
`firmware/src/audio/README.md`). TILES v1 (First Edition) adds a stereo
DAC and one phones/line jack (`../hardware/audio-output.md`).

Agreed 2026-11-11. Cycle figures are **measured on unit 2** the same day
(the DSP stress test, below), not estimates.

## Budget

| | Today | Notes |
|---|---|---|
| CPU | core 1 only: 3,125 cycles per sample at 150 MHz / 48 kHz; the load guard aims at 75 % (~2,340) | core 0 scans the pads and stays as it is (its loop measured the same, 2.2 ms, during the test; flash reads from core 1 slow it, see Drum samples) |
| RAM | ~275 KB free of 520 KB | ~355 KB in release builds: the 80 KB sensor-capture buffer is a development tool and is compiled out |
| Flash | ~3.7 MB free of 4 MB (the image is ~330 KB) | 1.5 MB for the 8 drum sample slots, ~1 MB kept for code, the rest for wavetables; not a full sampled piano |

## Voices

| Pool | Voices | Plays |
|---|---|---|
| Played | 8 | what you play on the pads, MPE per note |
| Sequencer | 4 | one per sequencer lane; its own pool, so a running pattern never steals a held note |
| Drums | the kit (8 sounds, each synthesized or one sample slot) | drum mode and the drum lanes; only a few ring at once |

An engine never runs more than 12 voices (8 + 4).

**Load guard, from the first engine on:** if core 1 runs short of time,
it releases the quietest voice instead of glitching. The worst case is a
shortened tail, never a click in a recording.

## Engines

The web player's sounds (`TILES_QuickStart/play/synth-worklet.js`) are
the starting point, ported to C on core 1. The web player's sampled grand
piano (hundreds of MB) doesn't fit; a modelled or FM piano takes its
place. Sound names stay Sentia's own (no instrument or product names).

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

**Effects** (per 48 kHz sample): plate reverb run at 24 kHz ~400,
ping-pong delay at 24 kHz ~160, chorus ~210, master (clip and 24-bit
output) ~100-130.

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

- **8 played + 4 sequencer + drums + reverb + delay fits at 150 MHz**
  with wavetable and plucked sounds (78 %) and with the analog voice once
  its filter runs at control rate (88 %; ~80 % without the chorus).
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
4. **No six-oscillator metallic hats**: filtered noise, or a rendered
   sample, at a quarter of the cost.
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

## Drum samples

**8 slots**, one per kit sound, fixed size so any slot can be replaced
on its own: up to 2 s each (a crash's tail fades inside it), mono,
16-bit, 48 kHz. 192 KB a slot (three 64 KB erase blocks), 1.5 MB in
all, at a fixed place in flash below the saved data (`flash_map.h` gets
the region when the player lands). Loading a slot from the companion app
erases and writes flash, which pauses core 1, so it only happens while
nothing plays.

Typical one-shot lengths: kick 0.6 s, snare and clap 0.4 s, closed hat
0.15 s, open hat 0.8 s, rim 0.15 s, tom 0.6 s, crash 2 s.

Patches are small next to this: ~512 bytes a synth patch (128 = 64 KB),
~100 bytes a kit, kept with the saved data.


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
- **System clock on v1: 153.6 MHz** (48 kHz x 3,200) for an integer I2S
  divider: 2.4 % above the RP2350's 150 MHz rating, so it is qualified
  across temperature first; the fallback is 150 MHz with a fractional
  divider (more bit-clock jitter). DIN MIDI, the LED chains, I2C and the
  UART compute their dividers from `clock_get_hz()` and follow; timers
  run from the 1 MHz reference. A faster qualified clock (230.4 MHz also
  divides evenly) would add about 50 % to the budget.
- **Volume** is digital, before the DAC, and starts low at power-up for
  headphones. The encoders have no function yet, so volume is a setting
  until they are mapped.

## Order of work

1. ~~Measure on a board~~: done 2026-11-11 (above).
2. Engine framework: the three voice pools, the load guard, the effects
   bus and per-sound sends, following the rules above.
3. Port the engines one at a time, wavetable first.
4. Sample player: the 8 slots, DMA streaming, checked with the soak.
5. v1 bring-up: I2S, the clock change, mute and headphone detect.
