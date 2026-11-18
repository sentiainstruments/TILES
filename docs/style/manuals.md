# Sentia manual style

How a TILES manual looks and reads: the quick start and the full manual
at start.sentiainstruments.com, and every manual `manuals/` makes for a
custom mapping. Owner: Mats2000.

**Fixed:** the colours and the two typefaces (Exo 2 and Sora). Everything
else here is the current direction and can evolve: change it by pull
request, not by drifting from it.

## Principles

- **An instrument manual, not a web page.** A drawing of TILES, a short
  name, one action per numbered frame. No badges, kickers, spec strips,
  ledes, subtitles, captions, notes, small print, progress bars or other
  interface chrome.
- **Show, don't tell.** The drawing carries the instruction; the words
  name the move ("Play C", "Tap steps").
- **One accent with one job.** Magenta marks what you press and what
  lights. Nothing else is magenta.
- **Few words.** One idea per section, a few words per move. Concrete
  settings beat general advice ("input TILES (MIDI), track armed", not
  "set up your DAW").
- **Calm.** Cool, muted blues and lavender, flat surfaces, line drawings.
  Never glossy, never candy-coloured. What moves is slow, soft and eases
  in and out; nothing blinks, snaps or shimmers.

## The page

- **One dark page, the cover.** Sentia ink with the owner's photo of TILES
  at night blended into it, and the TILES wordmark with the page's name
  ("Manual", "Quick start") at its foot. Nothing else on it: no hints,
  taglines or breadcrumbs.
- **Everything after it is white, with no frame.** Content runs edge to
  edge on one gutter, lined up with the bars. No cards, panels or boxes
  around content.
- **Navigation is TILES's own keys,** centred on top, labelled and evenly
  spaced in four equal columns whatever the labels say: triangle =
  contents (or the manual), diamond = player, square = Sentia (the SENTIA
  mark inside the key), circle = quick start.

| | Quick start | Full manual |
|---|---|---|
| Page | The cover, then the steps on white, scrolling freely | The cover, then one chapter per sheet on white; sheets slide sideways, a chapter scrolls inside its sheet |
| Moving around | The keys scroll away with the cover; no bar stays on screen | The keys on top; every chapter in one row of big lowercase names between Minus and Plus at the bottom |
| Never | Cards, page heights, page numbers, fixed bars, scroll effects | Cards, flip cards, a front and back; frosted panels |

Agree with Mats2000 which format a generated manual uses before
building it.

## A section

1. Its name under a 1.5 px ink rule, its number grey beside it ("2.1").
2. Numbered frames, one action each: TILES's face with the key to press
   lit magenta and the pad to press ringed magenta. Same view, same scale
   in every frame; three a row on wide screens, two on phones. A right
   frame beside a wrong one when a mistake is likely.
3. A key table: the keys, then what they do in one to three words.

Numbers (a step, a chapter) sit in one pad-shaped ink box with a white
Exo 2 numeral.

## Colour (fixed)

| Name | Value | Job |
|---|---|---|
| Ink | `#171d3a` | The dark page; text and line art on light pages |
| Ink, light | `#e6e9f6` | Text and line art on the dark page |
| Ink 2 | `#5a6088` light / `#9ca3c6` dark | Secondary text |
| Paper | `#ffffff` on `#eef0f7` | Light pages |
| Line | `#d9ddf0` light / `rgba(201,207,238,.16)` dark | Hairlines and rules |
| Periwinkle | `#94abf9` | Second colour: what you see, hear or feel |
| Magenta | `#ff00ff` | Only a pressed key or a lit pad |

Pads in a drawing show TILES's own lights (each mode's colours); that is
the instrument's colour, not the page's.

## Type

- Fixed: **Exo 2** for titles and numerals (600/700) and **Sora** for
  everything else (400 to 600). No other typeface anywhere — not for
  labels, not for numbers, not for a "technical" look. Get that from
  spacing, uppercase and tracking.
- Five sizes, nothing else, in `rem` so a reader's own text size carries
  through; each grows a step from phone to desktop:

| Size | Value | Use |
|---|---|---|
| Title | `clamp(1.75rem, 1.35rem + 1.25vw, 2.375rem)` | Chapter names, the chapter row, the cover's page name |
| Lede | `clamp(1.0625rem, 1rem + .25vw, 1.25rem)` | Section names, number boxes |
| Text | `clamp(1rem, .95rem + .2vw, 1.125rem)` | Body |
| Small | `clamp(.875rem, .84rem + .15vw, 1rem)` | Frame words, key tables |
| Label | `clamp(.75rem, .72rem + .1vw, .8125rem)` | Uppercase, tracked labels; never under 12 px |

## Drawing TILES

- Draw only from the footprint: `shared/footprint/` (from the
  instrument's own `buttons&pads.ai`). Never redraw a key or pad by eye,
  and don't draw an outer plate.
- One drawing of the face, at one scale and one line weight (1.5 px,
  non-scaling stroke; 2.5 px for magenta) everywhere in a manual.
- Keys to press are filled magenta; lit pads are magenta.
- A drawing of the whole instrument never gets huge: it stops growing at
  about 34rem and, on wide screens, its text sits beside it as one list.
- Pad names take one size per drawing; leave them out where they would be
  under 9 px. Every picture opens large on a click.
- Gestures are side views of the pad with magenta marking the action.
  Never draw a finger or a hand. Photos only when Mats2000 provides them,
  at their full resolution, never shrunk or re-encoded.
- Keys in running text are small icons cut from the same footprint
  outlines. Minus and plus are two half-pills with a small gap — the
  same shape everywhere.
- The keys are called Minus, Plus, Triangle, Diamond, Square and Circle.
  Pads are numbered 1–24 row by row from the top left. All jacks are on
  the back (GATE, CV1, CV2, MIDI IN and OUT, USB-C, 12 V) except the
  pedal, on the side.

## Everyone can read it

- One `h1`, chapters `h2`, sections `h3`. Every picture named for screen
  readers; key icons named ("triangle button"); a picture that opens
  large gets its own "Enlarge" button so it keeps its description.
- A "skip to" control first; every control reachable by keyboard, with a
  visible focus ring. Nothing scrolls sideways at 375 px wide or at 150 %
  text. No motion when the reader asks for reduced motion.

## Don't

- Gloss, frosted glass, backdrop blur, big soft shadows.
- Dot grids, grid paper, drifting light, floating specks or grain.
- Frames or cards around content; hint text on the cover.
- A third typeface. Drawn fingers or hands. Flip cards.
- Naming another instrument or product, or describing a TILES feature by
  comparison with one, without Mats2000's OK.

The published quick start and manual are the reference implementation;
their source is private (its detailed design notes follow this page) —
ask Mats2000 for anything this page doesn't answer, then add the answer
here.
