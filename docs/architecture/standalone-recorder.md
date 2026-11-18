# Standalone mode as a 5-track recorder (planning)

Status: **planning, 2026-11-17.** The owner is designing the layout; this
holds the research, what the hardware allows, the budgets and the open
questions. Controller mode stays as it is.

## The idea

Standalone mode becomes a recorder: **4 synth tracks + a drum track** (the
engine already has them: four synth tracks and the 8-slot drum sampler,
`../../firmware/src/audio/README.md`). Each track holds loops (clips) you
record by playing, by capture, or step by step; clips line up in scenes;
scenes chain into a **song**, building on Song mode. The reference
studied is Ableton Move (internal only: user-facing text describes TILES
in its own terms).

## The reference, in short (Move manual, 2026-11)

- 32 pads (4 x 8), 16 step buttons, 4 track buttons, 9 encoders, a wheel,
  a small screen, ~15 buttons (Play, Record, Loop, Capture, Copy, Delete,
  Mute, Undo, Shift, Note/Session ...).
- 4 tracks, each an instrument (or drum kit) plus two effects; 2 global
  effects.
- **Session:** a row per track, a column per scene; up to 8 clips a track.
  A press launches at the next bar; a finger slid down a column plays the
  scene; an empty column stops everything. Hold Copy / Delete + pad.
  Hold Mute + track = mute; Shift + Mute + track = solo.
- **Note:** play the selected track (drums: 16 pads; melodic: in key or
  chromatic).
- **Steps:** pad + step (or hold step + pad) adds a note; hold step +
  wheel = length, + encoder = velocity, + arrows = nudge, +/- = transpose.
  16th grid, pages beyond 16 steps.
- **Recording:** Record with a 1-bar count-in; overdub on a selected
  clip; **Capture** keeps what you just played without recording armed (the
  first note starts the clip; tempo guessed when stopped); quantize;
  undo / redo (Shift + Undo).
- **Loop:** clip length on the step buttons, up to 16 bars; double loop.
- **Sets:** 32, auto-saved. **No song / arrangement mode** (a common
  request): room for TILES to do better.

## What TILES has to work with

| | The reference | TILES |
|---|---|---|
| Pads | 32, velocity | 24 (4 rows x 6 columns), velocity, pressure, tilt (MPE), haptics, RGB |
| Steps | 16 buttons | none: the pad grid becomes the steps in a step view |
| Track select | 4 buttons | none: "-"/"+" (as the sound menu does), or a held key + pad |
| Other keys | ~15 | 6 (triangle, square, diamond, circle, "-", "+"), with LEDs; combos |
| Knobs | 9 + wheel | First Edition: 2 encoders (maybe 3); Rev A0: none |
| Screen | yes | none: colour, pulses, the underglow, haptics |
| Sound | 4 tracks | 4 synth tracks + drum sampler (5 voices, 8 slots) |

Already built that the recorder can reuse: the 4-lane step sequencer
(24 steps, 6 patterns a lane, probability, ratchets, capture), the drum
sequencer (16/32 steps), Song (24 loop slots of 128 steps, 4 notes a
step, capture), tap tempo, the clock, scales, the sound and effects menus,
and every play mode (melodic, Duet, bass guitar, Prism, the physics
modes), which could become *the ways to play a track*.

## A first mapping to react to (not a decision)

- **Session view:** columns 1-4 = synth tracks 1-4, column 5 = drums,
  column 6 = scene launch; rows = 4 scenes ("-"/"+" page to more). A pad
  = a clip: dim = empty, its track's colour = held, pulsing = playing or
  queued. That uses all five tracks at once on the 4 x 6 grid.
- **Track view:** play the selected track with any of today's layouts
  (melodic, bass guitar, Prism, drum pads; the physics modes as
  generators recording into it).
- **Step view:** 16 steps on columns 1-4 + 8 controls on 5-6 (Song's step
  editor already works this way: 16 steps + 8 page pads).
- **Keys** (a guess): diamond = play / record (as the Live transport:
  click / hold); circle = shift (copy, delete, length, undo as circle +
  something); triangle = views; square = capture; "-"/"+" = track or page.
  Encoders on the First Edition: track volume and the effects (already
  numbered parameters, `synth-engines.md`).

## Budgets

- **CPU:** fine at 230.4 MHz (twice the voices' room; the clips play on
  core 0, cheap).
- **RAM:** ~22 KB free. Clips are read from flash as they play (core 0
  runs from flash anyway); RAM only holds the clip being recorded (a few
  KB) and the undo copy.
- **Flash:** 4 MB. The image is ~0.4 MB of a 2.25 MB area, so ~1.5 MB is
  free for projects (keeping the image room to grow); the 8 drum slots
  (1.5 MB) and the saved band (256 KB) stay. For example 16 projects x
  64 KB.
- **Clip size:** a note ~6 bytes (tick at 1/96 beat, note, velocity,
  length). Played MPE expression (bend, pressure, slide per note) is what
  costs: recorded as changes at up to ~50 Hz, ~100-150 bytes a second a
  held note, so a busy 8-bar clip (16 s, 4 notes held) ~8 KB. Quantized
  step clips are tiny. Whether clips keep expression is a design choice
  (below).

## Open questions for the design

1. **Session grid:** tracks as columns (all 5 visible) or rows? Scenes
   per page, and how many in all (8? 16?)?
2. **Recording style:** fixed length set first (1-16 bars) or free (the
   first pass sets the loop, like a looper)? Count-in? Metronome on the
   jack/USB, or the haptics?
3. **Expression:** keep each note's bend/pressure/slide in clips (the
   TILES way, bigger), or notes only?
4. **Timing:** recorded as played (with optional quantize) or snapped to
   16ths?
5. **The drum track:** one kit of the 8 slots; its clips as step patterns
   (like today's drum sequencer) or recorded hits too?
6. **Song:** scenes chained with repeat counts, or scene launches recorded
   as you play them? Both?
7. **Projects:** how many, auto-save, how you pick one (a pad each, like
   the sets)?
8. **What leaves the menu in standalone:** Ableton and Volca (computer and
   Volca only); do the other modes become track layouts?
9. **Undo:** how many steps, on which key?
10. **The companion app:** projects to and from the computer later; the
    file format would then be a `shared/` contract.

## Suggested order of work (once designed)

1. The clip format and engine on core 0: record, loop, overdub, quantize,
   undo; notes and (if chosen) expression; tests on the computer.
2. Session view: clips, scenes, launch / stop at the bar, track select,
   mute.
3. Track view: today's layouts into a track; record, capture, count-in.
4. Step view: edit, length, copy / delete / double.
5. Song and projects (flash region, save / load); the mixer (volume,
   mute / solo, encoders on the First Edition).
6. The companion app: projects in and out (`shared/` format, Sam).
