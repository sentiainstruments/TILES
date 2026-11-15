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
| Mix effects | Reverb and delay (planned), plus sidechain pump, chorus/ensemble, bit-crush / rate reduction. |
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
  sweeps). 38 KB each in RAM.
- `synth_voice.c/.h`, `synth_patches.c`: the voice and 13 factory sounds:
  acid, pluck, haze, glass, reese, hoover, stab, wobble, air, choir, sub,
  lead, pure.
- `audio_engine.c`: a patch per track, MPE (pressure, CC74 slide, bend),
  mono/legato note stacks, click-free steals (a voice given away fades
  over 5 ms), stereo mix.
- `SYNTH LIST | PATCH | MIDI` on the settings link and `tiles_control.py
  synth ...`: pick a track's sound and play the engine from the computer.

Measured on the computer (`test/test_synth.c`): saw aliasing below 18 kHz
-70 dB at A3, -73 dB at A5, -61 dB at A7; SVF -3.0 dB at cutoff, -24 dB two
octaves up, exact at 12 kHz; ladder -48 dB two octaves up, bounded at full
resonance; envelope times within 5 %; every factory sound plays a chord
with pressure and slide without clipping and falls silent after release.

## Measured on unit 2

Cost per voice in cycles per sample (core 1 at 150 MHz has 3,125; voices get
1,556), one note held while streaming, after moving everything the voices
touch into RAM (it was twice this from flash):

| pure, glass | sub | acid, pluck, haze | wobble |
|---|---|---|---|
| ~250 | ~285 | ~370 | ~700 |

That is about four voices of the heavier sounds, against the ~12 planned;
the guard keeps it safe by shedding (67 shed over a 40 s tour of all
sounds). Four held haze voices: 63 % of core 1, every USB packet on time.

## Known issues

- **Cost is about twice the target.** Next: the optimization pass (below).
- **Late USB packets under load reset the stream.** With four pluck voices
  10 packets came late and macOS restarted the stream, losing ~0.9 s.
  TinyUSB's RP2350 driver used to panic on that restart ("ep 85 was
  already available"), which left USB dead: now the armed packet is
  revoked when the stream stops (`audio/usb_audio.c`), and any panic or
  hard fault restarts the board and is reported by INFO (`fault=`).

## Next, in order

1. **Optimization pass**, measured with the DSP bench (`audio-engine.md`):
   float or pair-packed table reads instead of 16-bit conversions, one
   frame read when the wave position rests, division-free filter
   coefficients updated only when they move, skip unused LFO/drift work.
   Target: ~150 cycles for a plain voice, ~250 for 3-copy stereo.
2. **Late packets:** find what holds core 1 off USB for a whole frame
   under load.
3. **Effects bus:** plate reverb, ping-pong delay, chorus/ensemble,
   sidechain pump from the drum track, bit-crush; per-track sends.
4. **FM engine** with DX7 patch import (`patch put bank.syx`).
5. Patch choice on the instrument (a menu) and the patch format in
   `shared/` for the companion app.
