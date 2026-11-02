# Sentia manual style

How a TILES manual looks and reads: the quick start and the full manual
at start.sentiainstruments.com, and every manual `manuals/` makes for a
custom mapping. Owner: Mats2000.

**Fixed:** the colours and the two typefaces (Exo 2 and Sora). Everything
else here is the current direction and can evolve: change it by pull
request, not by drifting from it.

## Principles

- **An instrument manual, not a web page.** A drawing of TILES, a short
  title, one to three numbered moves. No badges, kickers, spec strips,
  figure captions, progress bars or other interface chrome.
- **Show, don't tell.** The drawing carries the instruction; the words
  name the move. Put a caption right under its drawing.
- **One accent with one job.** Magenta marks what you press and what
  lights. Nothing else is magenta.
- **Few words.** One idea per section, a few words per move. Concrete
  settings beat general advice ("input TILES (MIDI), track armed", not
  "set up your DAW").
- **Calm.** Cool, muted blues and lavender, flat surfaces, hairline
  drawings. Never glossy, never candy-coloured.

## Two formats

| | Quick start | Full manual |
|---|---|---|
| Page | One dark page that scrolls freely: steps flow one after another | A booklet of one-sided cards, swiped sideways like pages |
| Background | Solid Sentia ink | Flat white cards on a pale blue-grey page |
| Never | Cards, page heights, page numbers, nav bars, scroll effects | Flip cards or a front and back; frosted panels |

Agree with Mats2000 which format a generated manual uses before
building it.

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

## Type

- Fixed: **Exo 2** for titles (600/700) and **Sora** for everything else
  (300/400). No other typeface anywhere — not for labels, not for
  numbers, not for a "technical" look. Get that from spacing, uppercase
  and tracking.
- Five sizes, nothing else:

| Size | Value | Use |
|---|---|---|
| Title | `clamp(1.75rem, 3.6vw, 2.25rem)` | Section or card title |
| Lede | `1.0625rem` | The line under a title |
| Text | `1rem` | Moves and descriptions |
| Small | `.875rem` | Small print under a move, notes |
| Label | `.6875rem` | Uppercase, tracked labels |

## Drawing TILES

- Draw only from the footprint: `shared/footprint/` (from the
  instrument's own `buttons&pads.ai`). Never redraw a key or pad by eye,
  and don't draw an outer plate.
- One drawing of the face, at one scale and one hairline weight (1.5 px,
  non-scaling stroke) everywhere in a manual.
- Keys to press are filled magenta; lit pads are magenta.
- Gestures are side views of the pad with magenta marking the action.
  Never draw a finger or a hand.
- Keys in running text are small icons cut from the same footprint
  outlines. Minus and plus are two half-pills with a small gap — the
  same shape everywhere.
- The keys are called Minus, Plus, Triangle, Diamond, Square and Circle.
  Pads are numbered 1–24 row by row from the top left.

## Don't

- Gloss, frosted glass, backdrop blur, big soft shadows.
- Dot grids, grid paper, drifting light or grain behind the quick start.
- A third typeface. Drawn fingers. Flip cards.
- Naming another instrument or product, or describing a TILES feature by
  comparison with one, without Mats2000's OK.

The published quick start and manual are the reference implementation;
their source is private — ask Mats2000 for anything this page doesn't
answer, then add the answer here.
