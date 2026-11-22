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

## FM (built 2026-11-16)

Six operators playing DX7 voice data (`firmware/src/audio/fm_voice.c`,
tested on the computer by `test/test_fm.c`). Studied: the DX7 engine
behind Dexed (Google's *Music Synthesizer for Android*, Apache 2.0),
written fresh for TILES. What it settled:

- **Algorithms:** 32 routings, computed operator 6 first; each operator
  reads bus 1 or 2 (or nothing), writes bus 1, bus 2 or the output, and
  adds or overwrites. Algorithms 4 and 6's three-operator feedback loops
  are approximated as operator 6's own feedback, as Dexed does.
- **Units:** phase 1 << 24 = one cycle; a modulator's output (Q24) is
  added to its carrier's phase, so full scale = +-1 cycle; levels are log2
  Q24, gain = 2^(level - 14 octaves), at most 2.0 (+-4 pi of modulation).
  Feedback: the mean of the last two outputs >> (8 - feedback).
- **Envelope:** levels 0.75 dB a step (`scaleoutlevel`: below 20 a table,
  else 28 + level); rate step (4 + (q & 3)) << (q >> 2) per sample at
  44.1 kHz, q = rate x 41 / 64 + rate scaling; decay linear in dB; attack
  jumps to 1716 << 16, then climbs by (17 - level in octaves) x step.
- **Pitch:** coarse 0 = x0.5, else x coarse; fine x(1 + fine/100); detune
  0.0209 e^(-0.396 octaves) / 7 x (detune - 7) of the log pitch; fixed mode
  10^(coarse & 3 + fine / 100) Hz. Keyboard level scaling in groups of 3
  notes from break point + 17 (linear: group x depth x 329 >> 12;
  exponential from a 33-entry table); rate scaling (sens x clamp(note/3 -
  7, 0, 31)) >> 3; velocity from a 64-entry curve.
- **LFO** speeds 0.06-49 Hz (a 100-entry table), six waves, pitch depth up
  to an octave; **pitch envelope** levels up to +-4 octaves.
- **Bank format:** F0 43 0n 09 20 00, 32 x 128 packed bytes, checksum
  (128 - sum) & 127, F7 (4,104 bytes).

How it is built:

- **Integer operators:** a 4,096-entry 16-bit sine (8 KB of RAM), SMULWB
  gains, 16-sample blocks with the gains ramped across each; the
  envelopes, LFO, pitch and gains are worked out once a block. An
  operator below -84 dB isn't computed (a bass using two of its six
  pays for two). Measured on unit 2 (`synth bench`, 2026-11-17): tine
  191, chime 194, solid 157 cycles a voice (solid plays 4 operators).
  The operator loops were ~17 cycles an operator-sample and every
  operator's control each block ~90 a sample; cut since (one pitch
  exponential a voice, each operator its ratio of it; integer gains from
  a 64-step table; envelope steps per block precomputed; operators no
  stage of which is audible never computed; the loop fixed at 16 samples,
  a shorter block taking a generic path). Same sound (loudness within
  0.3 dB of before). Then (bit-identical): an unbent held note keeps its
  steps, a held level its gain, and the output loop knows left and right
  don't overlap. Unit 2, 2026-11-17: tine 158, solid 125, chime 160 (from
  191 / 157 / 194). Per sample, profiled (tine): operator loops ~95,
  their control ~39, output ~18, pitch and LFO 7; at 230.4 MHz about 17
  voices fit, so FM no longer limits a track.
- **Any sound is wavetable or FM:** `synth_patch_t.engine`; an FM sound
  points at its DX7 voice and keeps from the patch only the voice mode,
  glide, levels and effects. The engine's voices are a union of the two
  (`audio_engine.c`); a voice that last played the other engine starts
  clean.
- **Expression:** pressure and slide (CC74) raise the modulators by
  `pressure_db` / `slide_db` at full (brighter, as on the wavetable
  sounds); bend moves every operator but a fixed-frequency one; the
  carriers follow the patch's level, velocity and pressure shares.
- **A voice ends** when its carriers fall below -84 dB with the key up:
  DX7 release times are kept (rate 50 is ~1.3 s from full).
- **Factory FM sounds** (`fm_patches.c`, our own): *tine* (electric
  piano, algorithm 5, a x14 strike pair), *solid* (sine plus sub bass,
  algorithm 1, legato with glide), *chime* (bell, inharmonic x3.5 /
  x7.07 / x1.41 pairs). In the sound menu: basses, plucks and bells rows.
- **RAM:** ~21 KB (the sine, the render loop's eight operator variants,
  the voices' larger union); ~22 KB stays free.

- **DX7 banks** (built 2026-11-17): `tiles_control.py synth bank put
  file.syx` loads a 32-voice bank (the 4,104-byte sysex, checked here and
  on the board: header, DX7 checksum, CRC-32) into its own 2-sector
  region (`firmware/src/audio/fm_bank.c`, the voices first, the header
  last); `synth bank` lists the names, `synth voice 2 11` gives track 2
  voice 11. A track's bank voice is unpacked into one of two RAM slots it
  owns (the old sound's voices keep the other while they fade): ~3 KB.
  Its level follows its carrier count (0.6 / sqrt(carriers), first
  guess), pressure and slide brighten as on the factory FM sounds. The
  bank stays across restarts; which voice a track plays doesn't yet (nor
  does a factory choice).

Next: more factory FM sounds (a pluck, a stab); bank voices in the sound
menu; the tracks' choices saved.

## Granular (built 2026-11-21)

A cloud of short grains a note (`firmware/src/audio/grain_voice.c`,
tested on the computer by `test/test_grain.c`): asynchronous granular
synthesis after C. Roads, *Microsound* (2001), our own code. On the First
Edition's spec sheet (owner, 2026-11-21).

- **A grain** is a windowed slice of a source at its own pitch, place and
  side: up to 8 a voice, started at the sound's density (grains a
  second), the spacing jittered from even to random. The window is a
  smoothstep on a triangle, a Hann-like bell whose peak `skew` moves
  forward (a strike and a tail); worked out each control step, ramped
  between.
- **Two sources.** *Wave:* a wavetable bank, each grain one of its 8
  frames (between two, either, by nearness) at the mip level for its own
  pitch, so octave grains don't alias; no memory needed. *Tape:* a drum
  slot, mixed to mono, copied into the delay's line (1 s at 48 kHz); the
  slot plays at its own speed on the sound's root note. The delay rests
  while a tape sound is chosen on any track (wave sounds keep it); the
  line goes back cleared. The sampler copies the slot between its own
  fetches (the head at once, then 512-byte chunks by DMA, ~0.1 s for a
  second), so the drums never wait. A slot reloaded is copied again; the
  effects' memory lent to the pad capture takes the tape with it until it
  comes back. The last tape sound chosen picks the slot; an empty slot
  falls back to the last one loaded.
- **The place** is the patch's wave position and everything that moves
  it (pressure, slide, envelope, LFO), plus `scan`: the tape crossed at a
  multiple of its own speed (0.25 = stretched four times), or the frames
  swept so many times a second. `spray` scatters each grain's place.
- **Each grain's pitch:** `scatter_cents` at random, and chances of an
  octave up, an octave down, a fifth; a chance of playing backwards. A
  bend moves grains already playing (a tape grain about to leave the tape
  turns back).
- **Expression:** pressure lengthens or shortens grains
  (`size_pressure`, octaves), thickens the cloud (`density_pressure`),
  spreads it (`spray_pressure`); slide spreads it and scatters pitch;
  the filter (SVF, stereo), the envelopes and levels are the wavetable
  voice's.
- **Level:** unrelated grains add by power, so the voice is scaled by 1 /
  sqrt(grains overlapping): measured within 0.1 dB from 1 grain to 7.5.
- **Factory sounds** (`grain_patches.c`, first guesses to tune by ear):
  *cloud* (saw and square grains, octaves above and below; pressure
  shorter and denser), *dust* (short vowel grains at random, drifting),
  *shimmer* (glass grains, octaves and fifths above), *scatter* (drum
  slot 8 stretched to a quarter speed; slide scrubs through it). In the
  sound menu: cloud, dust and scatter on the pads row, shimmer on the
  bells row.
- **Cost, unit 2** (`synth bench`, 2026-11-21, 230.4 MHz; the bench
  holds a granular note 0.5 s first, its cloud full): ~45 cycles a voice,
  ~33 more for its filter while grains or their tail sound, ~26 a grain:
  *cloud* 314, *shimmer* 298 (8 grains each), *dust* 134 (~1.7),
  *scatter* without its tape 47. At the voices' 2,880 cycles about 9
  clouds or 20 dusts. How it got there, from 353 / 334 / 145 for a full
  cloud as first built (measured the same way):
  - the place, size, density and spray worked out only as a grain
    starts, the pitch only when it moves, a grain's mip level only then;
  - 16 samples a step for the grains and the filter (the envelopes still
    step every 8): each grain's own work a step counts half;
  - the grains mixed in integers into one buffer of left/right pairs,
    a grain's two gains packed in one register (SADD16 steps both,
    SMLABB/SMLABT add the sample): ~30 cycles a grain-sample to ~26;
  - no filter between grains once it has rung out.
  Tried and dropped: LDRD/STRD for the pairs (no faster).
- **RAM:** a grain voice fits the voices' union as it was (364 bytes:
  8 grains of 28); the code in RAM and the four sounds ~9 KB, the code
  run once a note, a grain or 16 samples compiled for size (3 cycles a
  voice slower, ~1 KB smaller). ~5 KB stays free.
- **Levels:** cloud, dust and shimmer within ~1 dB of the other pads and
  bells (test_synth's chord, loudest 0.1 s).

Next: listen on unit 2 and tune the four sounds; a tape sound choosing
its slot from the sound menu or the companion app.

## Measured on unit 2

Where a wavetable voice's cycles go, per sample (profiled 2026-11-17 at
230.4 MHz): control (envelopes, LFO, pitch, filter settings every 8
samples) 20-50 on every sound; each unison copy ~32; drive ~15 a
channel, the folder ~50 a channel; the SVF ~31 mono / 46 stereo, the
ladder ~81 / 99 (bound by its chain's FPU latency). What's left would
change the sound, so it waits for listening: the control step every 16
samples (about -20 on every sound), a cheaper ladder saturation or
folder (acid, reese, wobble).

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

- **A restart while playing** (unit 2, 2026-11-16, standalone, melodic
  mode, firmware c901d17): no red pulse, no record (that build kept a
  caught crash only in RAM, lost at the unplug). Thought to be the synth
  engine, maybe headroom; to look at first: core 1's 4 KB stack (its
  deepest use under the heaviest sounds and effects), the load near the
  guard's 90 % ceiling. Caught crashes now pulse red and stay in flash
  (INFO `fault.saved`), so the next one says where; `boot.reset` and
  `uptime_s` tell a crash from a power dip (`bor`) or the reset pin
  (`run`). Not reproduced on 2026-11-17 (firmware with FM): 9.5 minutes
  of stress while streaming (10 sounds, all four tracks, every effect at
  100, full-range MPE bends, note storms; load up to 97 %) and 2.5
  minutes of melodic playing: no restart, no voice shed, no underrun;
  core 1's stack at most 1,584 of 4,096 bytes, so not the stack.

Fixed along the way: late USB packets that made macOS drop
the stream for up to a second (core 1's USB code ran from flash:
`firmware/src/midi/HISTORY.md`), and TinyUSB's RP2350 driver panicking
when a recording app restarted the stream ("ep 85 was already
available"). Any panic, hard fault or stalled core 1 now restarts the
board and is reported by INFO (`fault=`).

## Next, in order

Pinned: **the clock, 230.4 MHz** (decided 2026-11-17), the same on Rev A0
and the First Edition: the DAC's bit clock divides from it exactly
(`../hardware/audio-output.md`), 200 and 250 MHz don't. Voices get 2,880
cycles a sample instead of 1,456 (the effects and drums take the same
cycles); the flash runs at half, 115 MHz, inside its 133 MHz rating.
Qualified on units 1 and 2 before it is the default: margin steps above
it, then a soak (full load, streaming, flash writes, read-back).
**Unit 2, 2026-11-17** (`cmake -DTILES_CLK_KHZ=...`, the stress tool at
~500 MIDI messages a second, every sound, every effect at 100, streaming,
the program read back every 30 s): 230.4 MHz at 1.15 V 10 min, 268.8 and
288 MHz at 1.20 V 10 min each (25 % margin), then a 30 min soak at 230.4:
no restart, no read-back error, nothing shed, no underrun; up to 20
voices at 96-98 % peak load (6 at 150 MHz), 28-33 C. **Every unit runs
230.4 MHz from now on** (the owner, 2026-11-17: no further per-unit test);
it is the build's default.

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
3. **FM:** measure it on the board (`synth bench`), then DX7 bank
   import (`patch put bank.syx`).
4. **Granular** (built and measured 2026-11-21): listen, tune cloud,
   dust, shimmer and scatter.
5. The patch format in `shared/` for the companion app.
6. More voice speed if needed: the ladder (~80 cycles a channel) and the
   wavefolder are the dearest parts left.
