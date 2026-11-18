# Sound roadmap: presets and new engines (planning)

Status: **planning, 2026-11-17.** The owner's direction: 96 presets in 4
pages, and more engines for TILES's focus: **breakbeat, bubbly sounds,
long swelling pads**. Nothing here is decided yet.

## Headroom now (unit 2, 230.4 MHz, 2026-11-17)

| | Now | What it means |
|---|---|---|
| CPU (core 1) | 4,800 cycles a sample; the voices get 2,880 (85 % less 1,200 for the effects and 5 drum voices) | Plenty. At the budget: wobble 8 voices, reese 10, pluck 13, tine 18, glass 26 (the engine caps a track at 8 and all at 20). |
| RAM | **14 KB free** of 512 KB | The constraint. Taken: the effects 178 KB, three wavetable banks 114 KB, code run from RAM ~77 KB, the sampler's heads and rings ~18 KB, Song's library ~12 KB, the FM sine 8 KB, voices ~7 KB. |
| Flash | 1.85 MB free in the image area (the image is 405 KB of 2.25 MB); the 8 drum slots 1.5 MB; the saved band 256 KB (~76 KB used) | Room for presets, more DX7 banks, longer samples. |

So engines that only compute (modal, superwave, formant, harmonic,
waveshaper) fit as they are; engines that need memory (delay lines for
plucked strings, grain windows) need RAM found first. Where it could come
from: Song's library read from flash instead (12 KB; the recorder
replaces it anyway, `standalone-recorder.md`); one wavetable bank fewer
(38 KB); the effects' memory lent to a sound that brings its own space
(as the pad capture borrows it now); or, for a later board, a bare
RP2350 with QSPI PSRAM (8 MB; the Pico 2 module has none).

## Standards

TILES already speaks: MIDI 1.0 with MPE, class-compliant USB MIDI and
USB audio (UAC2, no driver), MIDI clock, WAV for samples, DX7 sysex
banks, 48 kHz / 24-bit. Worth adding with the presets:

- **Program Change 0-95** on a track's channel loads its preset, so a DAW
  recalls sounds by number.
- **The standard sound controllers** for the parameters the encoders will
  move (CC 71 resonance, 72 release, 73 attack, 74 brightness (TILES's
  slide already), 91 reverb, 93 chorus, ...), so a DAW automates TILES's
  sound like any synth's.

## Presets: 96 in 4 pages

- **A preset** = the engine and its settings (a wavetable sound's ~50
  values, or an FM sound's DX7 voice), its expression amounts (pressure,
  slide), its effect amounts, a 10-character name. At most ~512 bytes.
- **Storage:** its own flash region in the free image area: 12 sectors
  (8 presets a sector) for 96, twice that for a power-cut-safe copy
  (96 KB). Saving one rewrites its sector (~60 ms, the sound paused:
  saved when the track is quiet, like the other stores).
- **Pages:** 4 x 24 pads. Page 1 could start as the factory sounds (16,
  8 free) and pages 2-4 empty; a reset puts page 1 back.
- **On the device:** the sound menu shows one page; a key steps the pages
  (the underglow says which); a press loads the preset into the track
  with a preview; a long hold saves the track's sound into that pad; a
  longer hold with circle clears it. Names come from the companion app
  (no screen).
- **Later:** presets as files for the companion app (a `shared/`
  format).

## New engines, for the focus

Reference studied (internal only): Arturia MicroFreak, ~22 engines
(basic waves, superwave, wavetable, harmonic, plucked string, modal,
formant, waveshaper, two-operator FM, chords, noise, sample playback,
three granular). Several of its digital engines follow Mutable
Instruments' open-source Plaits (MIT); as with FM, TILES's would be its
own code after the published designs, credited here. Costs are
estimates per voice at 48 kHz, to be benched.

| Engine | For | Pressure / slide | Cycles a voice | RAM | Priority |
|---|---|---|---|---|---|
| **Break slicer** (the drum track's sampler) | breakbeat: a break in a slot, sliced on the computer at its hits, a slice a pad, at the clock's tempo, choke and reverse | pressure: filter / drive; slide: pitch or reverse | ~65 (a drum voice) | none new | 1 |
| **Superwave** (up to 7 detuned saws a voice, drift, spread) | swelling pads, trance stacks | pressure opens and widens; slide: detune | ~250 with its filter | none | 1 |
| **Modal** (8-12 resonant modes struck by a click or noise) | bubbly: marimba, kalimba, glass, water | pressure: strike brightness and damping; slide: the material (mode ratios) | ~150 | none | 1 |
| **Plucked string** (a delay line, damping, dispersion) | bubbly plucks, harps, arps | pressure: brightness and sustain; slide: pluck position | ~50 | a line a voice, 1-2 KB (a pool) | 2 |
| **Drops** (a sine or high-Q filter pinged, a fast falling pitch, a breath of noise) | bubbles, blips, water drops | pressure: pitch drop depth; slide: resonance | ~40 | none | 2 (first as wavetable patches) |
| **Formant** (a pulse through 3-5 formant filters, vowels morphing) | choirs, "aah" pads | slide: the vowel; pressure opens | ~120 | none | 2 |
| **Granular cloud** (grains from a drum slot or a wavetable, a window in RAM) | huge evolving pads, textures | density, size, spray, pitch scatter | ~300 (16 grains), a cloud a track | a 16-32 KB window | 2, once RAM is found |
| **Waveshaper** (sine / triangle through a folder and an asymmetric shaper) | rubbery bass, bubbly leads | pressure: the fold | ~60 | none | 3 |
| **Harmonic** (16-24 partials: tilt, odd / even, spread) | organs, glass pads, spectra that move with pressure | pressure: the tilt | ~150 | none | 3 |
| Pad effects: shimmer (octave-up feedback in the plate), freeze | swells | - | in the effects | the plate's | 2 |

Breaks need more length than a slot holds today (2 s mono): 2 bars at
170 BPM are 2.8 s. Options: slots stored at 24 kHz mono (4 s, the grainy
rate breakbeat grew up with), or two slots joined.

## Order of work (a proposal)

1. **Presets**: the store, the sound menu's pages, Program Change. Every
   new engine then has somewhere to keep its sounds.
2. **Superwave and modal**: no RAM, the pads and the bubbles.
3. **Break slicer**: the breakbeat core.
4. **Plucked string, formant, drops.**
5. **The RAM plan**, then granular.
6. **Waveshaper, harmonic, shimmer.**

Each engine: tests on the computer (pitch, level, expression, no NaN),
the bench, 3-4 factory sounds in its family's row, the docs.

## Open questions

1. Page 1 = editable copies of the factory sounds, or the factory sounds
   fixed and 96 user slots besides?
2. The page key and the track key in the sound menu.
3. The save gesture; names only from the companion app?
4. Breaks: 24 kHz mono 4 s slots, or joined slots?
5. Granular: borrow the effects' memory, or wait for a board with PSRAM?
6. Which engines first, by ear.
