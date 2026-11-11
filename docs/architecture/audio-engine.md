# TILES's own sound

The plan for the sound TILES makes itself, on the chip it already has
(RP2350 on the Pico 2). Built so far: the plumbing and a placeholder
test voice streamed as a USB audio input (Alpha 7,
`firmware/src/audio/README.md`). TILES v1 (First Edition) adds a stereo
DAC and one phones/line jack (`../hardware/audio-output.md`).

Agreed 2026-11-11. Every cycle figure below is an estimate until the
test voice is measured on a board (see "Order of work").

## Budget

| | Today | Notes |
|---|---|---|
| CPU | core 1 only, ~3,100 cycles per sample at 150 MHz / 48 kHz; plan on ~2,300 | core 0 scans the pads and stays as it is |
| RAM | ~275 KB free of 520 KB | ~355 KB in release builds: the 80 KB sensor-capture buffer is a development tool and is compiled out |
| Flash | ~3.5 MB free of 4 MB | code, wavetables, small samples; not a full sampled piano |

## Voices

| Pool | Voices | Plays |
|---|---|---|
| Played | 8 | what you play on the pads, MPE per note |
| Sequencer | 4 | one per sequencer lane; its own pool, so a running pattern never steals a held note |
| Drums | the kit (8 sounds) | drum mode and the drum lanes; only a few ring at once |

An engine never runs more than 12 voices (8 + 4).

**Load guard, from the first engine on:** if core 1 runs short of time,
it releases the quietest voice instead of glitching. The worst case is a
shortened tail, never a click in a recording.

## Engines

The web player's sounds (`TILES_QuickStart/play/synth-worklet.js`) are
the starting point, ported to C on core 1.

| Engine | Kind | Cycles per voice per sample |
|---|---|---|
| Wavetable, morphed by pressure (Pure Sine) | wavetable | ~40-60 |
| Plucked / physical string | physical model | ~40-80 |
| Electric piano | 4-operator FM | ~100-150 |
| Analog-style pad (Aurora 80s): 2 oscillators, filter, 2 envelopes, LFO | virtual analog | ~150-250 |
| Grain Cloud | granular, one cloud of ~24 grains | ~500-700 in all |
| Drum kit, synthesized (Drum Rack order from C1) | drum synthesis | ~50-150 per hit |

The web player's sampled grand piano (hundreds of MB) doesn't fit; a
modelled or FM piano takes its place. Sound names stay Sentia's own
(no instrument or product names).

## Effects

One shared bus after the voices, with a send level per sound (drums
lighter on reverb).

| Effect | Cycles per sample | RAM |
|---|---|---|
| Reverb: plate/room, run at 24 kHz (a tail needs no top octave) | ~150-250 | ~35-80 KB |
| Delay: stereo ping-pong, tempo-synced, feedback with tone; 2 s (a bar at 120 BPM) stored at 24 kHz, so repeats darken like tape | ~40-80 | 192 KB |
| Chorus (optional) | ~60-80 | ~4 KB |

About 250 KB of RAM in all: this is why the capture buffer leaves
release builds.

## Does it fit

Everything at once: 8 played + 4 sequencer + drums + the effects bus.

| Main sound | 8 played | 4 sequencer | Drums | Effects | Total (budget ~2,300) |
|---|---|---|---|---|---|
| Wavetable, FM, plucked | ~400-1,000 | ~200-500 | ~300-500 | ~350-400 | ~1,250-2,400 |
| Analog-style pad | ~1,600 | ~800 | ~300-500 | ~350-400 | ~3,100 |

The analog-style pad with everything running needs one of:

1. a lighter engine on the sequencer lanes (bass, pluck), which suits
   most patterns anyway;
2. filters and mixing in fixed point with the M33's DSP instructions
   (roughly 2x where it matters);
3. a faster clock (below), once qualified.

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

1. Measure the test voice on unit 2 (Alpha 7): hold 16 notes with the
   USB input open and read `audio.load_pct` (estimated ~25 %). If it
   reads much higher, every figure here scales down.
2. Engine framework: the three voice pools, the load guard, the effects
   bus and per-sound sends.
3. Port the engines one at a time, wavetable first.
4. v1 bring-up: I2S, the clock change, mute and headphone detect.
