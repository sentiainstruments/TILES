# TILES's synth engines

How TILES's own synth sounds are built: the sound we aim for, the techniques,
what is built, what it measures, and what comes next. The plumbing (tracks,
voices, guardrails, USB audio, drums) is `audio-engine.md`; the code is
`firmware/src/audio/`.

Decided with Mats2000 on 2026-11-14.

## Decisions

| Question | Decision |
|---|---|
| Sound direction | The inspiration's world: Oklou's glossy trance arps and airy pads, late-90s/2000s trance and Eurodance supersaws, 90s breakbeat and rave (hoovers, Reese, acid, stabs), bass music (wobbles, growls). **Glossy by default, grit per patch**: clean and tightly tuned (arps and unison stay crisp), space from the effects; a patch adds drive, drift or crush. |
| Code | **Our own code** after published techniques (below). Nothing copied from other projects, so nothing to ship with the firmware; sources are credited here only. |
| First engine | Wavetable (built). |
| Quality vs voices | **Balanced** at 150 MHz: filter coefficients at 6 kHz, linear table reads, ~12 voices + 5 drums + reverb and delay as the target. |
| Expression | Pressure opens the filter and sweeps the wavetable, with a little level; slide (CC74) moves the timbre; bend is pitch. |
| Unison | Up to 3 copies a voice. |
| Voice modes | Per patch: poly, mono, legato (glide only on overlapping notes, as on the classic acid box). |
| Bass music | Tempo-synced wobble LFO, drive and wavefold, FM growls (with the FM engine). |
| Mix effects | Built (2026-11-15): a modulated plate at the full 48 kHz, a ping-pong delay, an ensemble, a sidechain pump, and per-track overdrive and bit-crush. Amounts per sound, a track can change its own. Their memory is shared with the pad capture (a capture quiets them). |
| FM | Six operators that **load DX7 patch files** (.syx), first through `tools/tiles_control.py`, later the companion app. |
| Patches | A factory set first; a patch format in `shared/` once the engines settle. |
| Track sounds at power-up | 1 acid bass, 2 pluck (melodic), 3 haze pads (chords), 4 glass bells (physics modes). |
| Clock | 150 MHz (rated). |

## Techniques

Each from a public paper, book or open project, written fresh for TILES.

- **Band-limited wavetables (mip-maps).** Every frame is stored once per
  octave, each copy holding only the harmonics that stay inside the audio
  band for the notes it plays; built at boot from spectra by inverse FFT.
  The top harmonic may pass Nyquist up to 28 kHz, since what folds back
  lands above 20 kHz. Copies are 8 to 16 times longer than their harmonic
  count so linear interpolation stays clean.
- **State-variable filter, trapezoidal (TPT/ZDF):** A. Simper (Cytomic),
  "Solving the continuous SVF equations using trapezoidal integration";
  V. Zavalishin, *The Art of VA Filter Design*. Exact cutoff to Nyquist,
  stable under fast modulation.
- **4-pole ladder, zero-delay feedback:** Zavalishin ch. 5, the loop
  solved each sample with a saturator on its input (squelch without
  blowing up; self-oscillates at full resonance).
- **Analog-style envelopes:** exponential segments; the attack aims past
  full scale like an RC charging to a higher rail.
- **Fast maths that stays in RAM:** 2^x by exponent bits and a short
  series, sine by its series after folding, tan by Lambert's continued
  fraction (control rate only).
- **Cortex-M33 specifics** (each measured with `SYNTH BENCH`): frames
  stored interleaved, so one 32-bit load gives both frames a voice
  crossfades and `SMUAD` (the DSP extension's dual 16-bit multiply-add)
  crossfades them; interpolation and unison gains by `SMMUL`/`SMMLA`, in
  integers until one conversion a channel; no fused multiply-add (3
  cycles against 2 for a multiply then an add, `-ffp-contract=off`);
  clamps by `VMAXNM`/`VMINNM` (no branches); no division per sample
  (the saturator is a 5th-order polynomial, the ladder's loop gain is
  inverted at control rate) nor in the control step (without fast-math
  GCC keeps `x * 8 / 48000` as a division); pitch and cutoff worked out
  again only when they move; the stereo filters side by side in one
  loop, so the FPU never waits on one chain.
- Studied for the engines to come: Mutable Instruments' open firmware (MIT)
  for oscillators, strings and reverb structure; the DX7 engine behind
  Dexed (Apache 2.0) and K. Shirriff's DX7 chip analysis for exact DX7
  envelopes and log-sine/exp tables; J. Dattorro, "Effect Design" (1997)
  for the plate reverb.

## Built (2026-11-14)

- `synth_dsp.h`: the SVF, ladder, envelope, LFO, shapers.
- `wavetable.c/.h`, `wavetable_banks.c`: the mip-mapped banks, 8 frames
  each: CLASSIC (sine, triangle, soft saw, saw, square, pulses 30/15/7 %),
  GLASS (sine to glassy partials), VOICE (four vowels, four hard-sync
  sweeps). 38 KB each in RAM, frames interleaved.
- `synth_voice.c/.h`, `synth_patches.c`: the voice and 13 factory sounds:
  acid, pluck, haze, glass, reese, hoover, stab, wobble, air, choir, sub,
  lead, pure.
- `audio_engine.c`: a patch per track, MPE (pressure, CC74 slide, bend),
  mono/legato note stacks, click-free steals (a voice given away fades
  over 5 ms), stereo mix.
- `SYNTH LIST | PATCH | MIDI | BENCH` on the settings link and
  `tiles_control.py synth ...`: pick a track's sound, play the engine
  from the computer, time one voice of each sound.

Measured on the computer (`test/test_synth.c`): saw aliasing below 18 kHz
-70 dB at A3, -73 dB at A5, -61 dB at A7; SVF -3.0 dB at cutoff, -24 dB two
octaves up, exact at 12 kHz; ladder -48 dB two octaves up, bounded at full
resonance; envelope times within 5 %; every factory sound plays a chord
with pressure and slide without clipping and falls silent after release.

## Effects (2026-11-15)

`audio/fx.c` (`fx.h`, `fx_memory.c`). Per track, at the mix level: the
inserts, then the pump, then the sends (mono) to the shared effects;
their returns, the drums and the master's soft clip follow.

| Effect | Design | Cycles a sample (unit 2) |
|---|---|---|
| Plate | J. Dattorro's figure-eight tank (*Effect Design*, 1997) at the full 48 kHz, his lengths scaled from 29,761 Hz: four input diffusers, two halves with a modulated allpass (quadrature LFO, his 16-sample excursion), damping, his seven output taps a side. Decay 0.97-2.85 s over the range (1.6 s by default), L/R correlation -0.01, echo density 0.92 (Abel & Huang's measure, 1 = diffuse). | 473 |
| Ping-pong delay | Two lines crossing over, each echo one feedback down, a 120 Hz-2.5..12 kHz band filter in the loop, a 1.5-sample wobble against metallic repeats; time in sixteenths of the tempo (dotted 1/8 by default), halved until it fits 0.5 s; glides to a new tempo's time. Feeds the plate a little. | 118 |
| Ensemble | Three taps 120 degrees apart, each moved by a slow (0.3-1.2 Hz) and a fast (4.5-7.5 Hz) LFO, as the string ensembles' bucket-brigade lines; L/R correlation -0.18. | 103 |
| Overdrive (insert) | Up to +28 dB into synth_sat's polynomial with first-order antiderivative anti-aliasing (Parker, Zavalishin, Le Bivic 2016): aliases of a hard-driven 4.7 kHz sine -26 dB against -12.5 dB plain; a bias for even harmonics, removed after; a tone low-pass 18 kHz-1.5 kHz; level matched. | 122 a driven track |
| Bit-crush (insert) | 16 down to 5 bits and 48 kHz down to 2.1 kHz together, sample and hold. | 29 |
| Pump | Ducks the tracks that ask (depth per sound) on every kick (drum slot 1), and on every beat of a running clock unless a kick just did; down in 1.5 ms, back along a smooth S over 0.7 beat. | 6 |

How they were made cheap enough: 16-bit storage in the lines' own units
(no scaling per access, SSAT on writes); the plate's lines in two
power-of-two rings (8 K and 32 K, Mutable Instruments' FxEngine approach)
so a wrap is one AND, with every offset a constant; the delay's ramps as
running sums (a per-sample division hid in one). The plate stays at its
seven taps a side: they are what makes it wide and dense, and nearly all
of its cost is its 42 memory accesses a sample.

Memory: 182 KB (plate 80, delay 94, ensemble 4), lent to the pad capture
(`diagnostics/capture.c`) and used as the wavetables' working space at
boot. The voices' budget leaves the effects and drums 1,200 cycles a
sample.

Measured live: the effects took 525 cycles a sample during a demo (pumped
pads under kicks, a pluck through the delay, an overdriven acid line, a
crushed wobble), with no late frame, underrun or shed voice.

## Measured on unit 2

One voice of each sound, cycles per sample (`synth bench`: core 0,
interrupts off, a note held 50 ms; core 1 at 150 MHz has 3,125 a sample,
voices get 1,556). Before: the first build, everything already in RAM.

| Sound | Before | After the optimization pass (2026-11-14) |
|---|---|---|
| pure, glass | 224 | 103, 105 |
| sub | 261 | 116 |
| acid | 356 | 186 |
| choir, air, pluck | 348-354 | 198, 203, 205 |
| lead, haze | 351-352 | 215, 222 |
| stab, hoover | 425, 433 | 239, 253 |
| reese | 570 | 276 |
| wobble | 672 | 354 |

Live, streaming: 7 pluck voices (3 copies, stereo) or 6 haze voices
held at 52-57 % of core 1, none shed; the 40 s tour of all sounds sheds
none (67 before).

## Known issues

None open. Fixed along the way: late USB packets that made macOS drop
the stream for up to a second (core 1's USB code ran from flash:
`firmware/src/midi/HISTORY.md`), and TinyUSB's RP2350 driver panicking
when a recording app restarted the stream ("ep 85 was already
available"). Any panic, hard fault or stalled core 1 now restarts the
board and is reported by INFO (`fault=`).

## Next, in order

1. **Try the sound and effects menus on the board** (built 2026-11-15,
   `firmware/src/services/sound_menu.c`): diamond = sounds, circle +
   diamond = effects, in standalone mode outside the sequencer.
2. **Assignable encoders** (First Edition hardware: two, maybe three,
   `docs/hardware/tiles-v1.md`): any parameter while playing in
   standalone mode, by relative steps (no jump when one is reassigned).
   The effects already have numbered parameters (`TILES_FX_TRACK_*`,
   `TILES_FX_BUS_*` in `audio_engine.h`) shared by the settings link and
   the menus; an assignment would point at one of them (and later at sound
   parameters such as cutoff or the wave position).
3. **FM engine** with DX7 patch import (`patch put bank.syx`).
4. The patch format in `shared/` for the companion app.
5. More voice speed if needed: the ladder (~80 cycles a channel) and the
   wavefolder are the dearest parts left.
