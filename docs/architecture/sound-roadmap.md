# Sound roadmap: presets and new engines (planning)

Status: **planning, 2026-11-17.** The owner's direction: 96 presets in 4
pages, and more engines for TILES's focus: **breakbeat, bubbly sounds,
long swelling pads**. Nothing here is decided yet.

## Headroom now (unit 2, 230.4 MHz, 2026-11-17)

| | Now | What it means |
|---|---|---|
| CPU (core 1) | 4,800 cycles a sample; the voices get 2,880 (85 % less 1,200 for the effects and 5 drum voices) | Plenty. At the budget: wobble 8 voices, reese 10, pluck 13, tine 18, glass 26 (the engine caps a track at 8 and all at 20). |
| RAM | **14 KB free** of 512 KB (~5 KB since granular, 2026-11-21) | The constraint. Taken: the effects 178 KB, three wavetable banks 114 KB, code run from RAM ~77 KB (~86 KB with granular), the sampler's heads and rings ~18 KB, Song's library ~12 KB, the FM sine 8 KB, voices ~7 KB. |
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

## Presets: 96 slots in 4 pages (decided 2026-11-17; the store built 2026-11-23)

Built: `firmware/src/audio/presets.c` (the record: `firmware/src/audio/
README.md`, "Preset record"). A slot never written plays its factory
sound instead of a first boot writing them all; a reset clears the
slots. Writing from the computer: the PRESET commands
(`shared/protocol/README.md`).

- **96 slots, flat:** no rows or columns by family. 4 pages of 24 pads,
  "-"/"+" step through the pages in the sound menu (the underglow says
  which); a press loads the slot into the track with a preview.
- **Every slot is editable from the computer:** the companion app names,
  edits, replaces and loads presets, up to 96. No save gesture on the
  device for now.
- **Shipped ready:** the first boot writes the factory presets into the
  slots (the 16 sounds today, more as engines arrive); they can all be
  replaced. A reset writes them back.
- **A preset** = the engine and its settings (a wavetable sound's values,
  or an FM sound's DX7 voice), the envelope, the expression amounts, the
  effect amounts, a 10-character name. At most ~512 bytes.
- **Storage:** its own flash region in the free image area: 12 sectors
  (8 presets a sector), twice that for a power-cut-safe copy (96 KB).
  Writes come from the computer, sector by sector, only while the track
  is quiet.
- **Program Change 0-95** on a track's channel loads its slot.

## What Teenage Engineering does (internal reference, 2026-11-17)

- **Every engine has exactly four parameters** (the four colour encoders):
  OP-XY's wavetable: table, position, warp, drift; Prism: shape, ratio,
  detune, stereo; Dissolve (tonal noise, pads): swarm, AM, FM, detune;
  Hardsync (stabs, basses): freq, sub, noise, lowcut; Epiano: tone,
  texture, punch, tine. Around the engine, the same pages for every
  sound: envelope (ADSR on the four encoders, plus play mode: poly, mono,
  legato, unison, portamento), effect, LFO. Changing the engine keeps
  those pages; changing the preset sets all of them.
- **Presets are files** with a 10-character name, kept in folders and
  managed over USB; 8 sound slots per mode on the device, Program Change
  0-7 for them, CC 46-49 for the four engine parameters; holding a slot
  key saves a snapshot; shift + a key browses the library; a randomize.
- **Drums are two engines side by side:** a sampler laid across the keys
  (each key a part of a sample: pitch, direction, in, out, play mode,
  level) and DBOX, a dual-oscillator drum synth (pitch, waveform,
  envelope, cross-modulation or filter); OP-XY's Drumline: ten
  percussion models, four parameters each, plus type, tune (random),
  pan, volume.
- **What TILES takes:** four named macros per engine (what the First
  Edition's encoders and the app move, and the standard CCs carry), the
  same envelope / effects / LFO around every engine, presets as named
  files from the app, a drum synth beside the sampler. Their engines'
  names and sounds stay theirs; TILES's are its own.

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
| **Drum synth** (beside the sampler: each of the 8 drum pads a sample or a model: kick, snare, hat, clap, tom, rim, cymbal, zap; four parameters each) | breakbeat kits without samples, tuned and shaped live | pressure: decay / drive; slide: tone | ~40-80 a hit | none | 1 |
| Break slicer (the sampler, a break sliced across pads) | breakbeat | pressure: filter / drive | ~65 (a drum voice) | none new | later (breaks set aside 2026-11-17) |
| **Superwave** (up to 7 detuned saws a voice, drift, spread) | swelling pads, trance stacks | pressure opens and widens; slide: detune | ~250 with its filter | none | 1 |
| **Modal** (8-12 resonant modes struck by a click or noise) | bubbly: marimba, kalimba, glass, water | pressure: strike brightness and damping; slide: the material (mode ratios) | ~150 | none | 1 |
| **Plucked string** (a delay line, damping, dispersion) | bubbly plucks, harps, arps | pressure: brightness and sustain; slide: pluck position | ~50 | a line a voice, 1-2 KB (a pool) | 2 |
| **Drops** (a sine or high-Q filter pinged, a fast falling pitch, a breath of noise) | bubbles, blips, water drops | pressure: pitch drop depth; slide: resonance | ~40 | none | 2 (first as wavetable patches) |
| **Formant** (a pulse through 3-5 formant filters, vowels morphing) | choirs, "aah" pads | slide: the vowel; pressure opens | ~120 | none | 2 |
| **Granular cloud** (built 2026-11-21, `synth-engines.md`: grains from a wavetable, or from a drum slot copied into the delay's line, 1 s mono) | huge evolving pads, textures | density, size, spray, pitch scatter | 134-314 on unit 2 (~45 a voice, ~33 its filter, ~26 a grain) | the delay's line, while a tape sound is chosen (the delay then rests) | built |
| **Waveshaper** (sine / triangle through a folder and an asymmetric shaper) | rubbery bass, bubbly leads | pressure: the fold | ~60 | none | 3 |
| **Harmonic** (16-24 partials: tilt, odd / even, spread) | organs, glass pads, spectra that move with pressure | pressure: the tilt | ~150 | none | 3 |
| Pad effects: shimmer (octave-up feedback in the plate), freeze | swells | - | in the effects | the plate's | 2 |

Breaks are set aside for now (they need more length than a slot holds:
2 bars at 170 BPM are 2.8 s; 24 kHz mono slots or joined slots later).

## Order of work (a proposal)

1. **The engine interface:** four named macros per engine, the shared
   envelope / effects / LFO; the wavetable and FM engines mapped onto it.
2. **Presets:** the 96-slot store, factory presets at first boot, the
   sound menu's 4 pages on "-"/"+", Program Change; loading from the
   computer (the companion app's side with Sam, a `shared/` format).
3. **Granular** (built 2026-11-21; on the First Edition's spec sheet,
   owner), **superwave**, **modal**.
4. **The drum synth** beside the sampler.
5. **Plucked string, formant, drops**; then waveshaper, harmonic, shimmer.

Each engine: tests on the computer (pitch, level, expression, no NaN),
the bench, factory presets, the docs.

## Open questions

1. The four macros of the wavetable and FM engines (which of their
   settings each moves).
2. The track key in the sound menu, now that "-"/"+" are the pages.
3. The preset file format with Sam (`shared/`).
4. While a tape sound is chosen, the delay rests (built so; wave sounds
   keep it): acceptable, or keep a short delay?
5. The drum synth's models, and how a pad chooses sample or model.
