# Standalone mode: the Deck, a tape recorder (design)

Status: **design, 2026-11-21, not built.** "Owner" marks the owner's
decisions; the rest is proposed and open to change. Controller mode stays
as it is.

## The idea (owner)

Standalone mode becomes a **4-track tape recorder with a drum track**.
Its home screen is the **Deck** (Song mode, renamed). The sequencer,
drums and Song leave standalone's menu: the Deck does their jobs. They
stay in controller mode, and Ableton and Volca move there only (owner,
2026-11-21). Each track holds
**tapes** (clips); a track plays one tape at a time, so up to 4 synth
tapes and 1 drum tape play together, launched like clips. A tape is a
step sequencer: 16 steps a page, up to 8 pages. You fill a tape step by
step, or open it in a layout (melodic, bass guitar, ...) and record by
playing.

The reference studied was Ableton Move (internal only; summary at the
end). User-facing text describes TILES in its own terms.

## Three screens

### The Deck (home)

| Columns | 1-4: synth tracks 1-4 · 5: drums · 6: scenes |
|---|---|
| Rows | tape slots 1-4; the wheel scrolls to rows 5-8 |

The pads are the transport: there is no play key (owner, 2026-11-21: no
keys added for it).

- A pad is a tape. Empty: dark. Holds a tape: its track's colour, dim.
  Playing: bright, breathing with the beat. Waiting for the bar to start
  or stop: blinking. Recording: red.
- **Tap a tape:** it starts at the next bar (that track's other tape
  stops); tap it again and it stops at the bar end. When nothing plays,
  a tap starts the clock with it.
- **Hold a tape** (or an empty slot) **½ s:** open it.
- **Touch any pad:** it's the selected slot (a white edge): the wheel's
  click records into it, the sound menu sets its track.
- **Scene pad (column 6):** starts that row on every track at the next
  bar; a track whose pad in the row is empty stops. **Shift + a scene
  pad:** everything stops.
- **Shift + a tape, then another pad:** copy it there. **Shift + hold a
  tape 2 s:** delete (it flashes red twice).

### A tape: steps (owner)

| Columns 1-4 | the page's 16 steps (row by row) |
|---|---|
| Columns 5-6 | the 8 page pads: row 1 = pages 1-2 ... row 4 = pages 7-8, in their own colour, not the track's |

- Page pads: the page shown bright, pages that count dim, pages off dark.
  The page the head is on pulses with the beat.
- **A page that's off reads empty and doesn't count** (owner); hold its
  page pad 2 s to switch it on, or off again. Writing a step on it
  switches it on too (owner). A page switched off keeps its notes and
  they're back when it's on again (owner). The tape plays only the pages
  that are on.
- **Tap a page pad** to show it; **the wheel** steps through the pages
  (owner).
- **Tap a step:** on or off. A new step plays the **brush**: the last
  note or chord played in this tape's layout (a new tape: the track's
  root; the drum track: the last drum played).
- **Hold a step:** the grid turns into the tape's layout for a moment:
  play the step's notes, let go to keep them (Song's step editor already
  works this way). Or hold a step and **turn encoder 1**: its pitch, in
  the scale; press the held step deeper: its velocity.
- **Shift + a page pad, then another:** copy the page.

### A tape: playing a layout (owner)

**Triangle** opens the menu: the Deck first (home), then the layouts for
the tape: steps, or a way to play its track (synth tracks: melodic,
Duet, bass guitar, Prism; the drum track: drum pads). In a layout the
whole grid plays, "-"/"+" stay octave (owner) and the wheel's click
records what you play into the tape.

## Recording: the wheel's click (owner, 2026-11-21)

No keys are added: the wheel the First Edition already has is the tape.
Roll it through the pages (or the Deck's rows), click it to record
(`../hardware/tiles-v1.md`).

| | Does |
|---|---|
| Wheel click | Records into the open tape, or in the Deck into the selected slot (opening it in its layout). Stopped: one bar of count-in clicks first, then the clock starts; playing: it starts at the next bar. Click again to end the take. The underglow blinks red while it waits and stays red while recording. |
| Shift + wheel click | Undo the last take. |

**A new tape grows as it rolls** (owner): page 1 switches on when the
take starts, the next page as the head reaches it, up to 8. A click
closes the loop at the end of that bar and the tape plays on. On a tape
that already has pages on, a take overdubs inside them, pass after pass,
until the next click.

**What a take keeps** (owner): each note lands on its step but keeps its
timing offset (up to half a step either way, 1/96 of a beat), its
length and its velocity. It edits like steps and still sounds played.
Bend, pressure and slide aren't kept for now.

Each track's head runs on its own: tapes of different lengths drift
against each other and meet again, and a track stops when its tape is
tapped in the Deck.

## Shift: circle, latched (owner)

On every screen and in every mode, a click on circle (encoder 1's push)
**latches shift**: circle's light glows, the next key or pad press does
its shifted job, and shift lets go. Holding circle while pressing still
works. A second click after a pause lets go without doing anything.

Encoder 1 is the time knob: **turn it to set the tempo** (one BPM a
detent; shift + turn: swing), **double-click it for the metronome**. Tap
tempo leaves circle.

## Metronome (owner)

- Two clicks made on core 1, outside the tracks and the effects: beat 1
  higher and brighter, beats 2-4 lower and softer (4/4).
- A double click on encoder 1 turns it on or off; it's remembered.
  Circle's light flashes each beat while it's on. The count-in before a
  take always clicks, metronome on or not.
- It goes to the phones / line out and to USB audio. Tapes record notes,
  not sound, so it never ends up on a tape.
- Its level: a setting (`metronome.level`) for now.

## The keys in standalone

| Key | Deck | Tape: steps | Tape: layout |
|---|---|---|---|
| Triangle | menu: the Deck, the layouts | same | same |
| Diamond | sound menu (owner) for the selected track | sound menu | sound menu |
| Shift + diamond | effects menu | effects menu | effects menu |
| Square | bend, Feel menu (as everywhere) | same | same |
| Circle / encoder 1 (push) | shift, latched; double click: metronome | same | same |
| Encoder 1 (turn) | tempo | held step: pitch; else tempo | tempo, or the assigned parameter |
| Wheel (turn) | rows | pages | - |
| Wheel (click) | record into the selected slot | record | record |
| "-" / "+" | - | - | octave (owner) |
| Pads | start, stop, open tapes; scenes | steps, pages | play |

**Rev A0** (four units, no wheel or encoder): shift + "+" records,
"-"/"+" scroll the Deck's rows, page pads only by touch, circle clicks
keep tap tempo and the metronome sits in the Feel menu. A bench
fallback, not designed for playing (in Duet, shift + "-"/"+" is the
chord strip's octave; Rev A0 loses it there).

## Storage

- **A tape** = a short header (which pages are on, its layout, its brush)
  and a list of notes: step, offset, pitch, velocity, length (6 bytes).
  Up to 256 notes a tape (1.5 KB); a step tape is far smaller.
- **RAM:** only the 5 playing tapes, the one open and an undo copy:
  ~10 KB at worst. Song's 12 KB library in RAM goes, so RAM comes out
  even or better (14 KB free today).
- **Flash:** a Deck of 5 tracks x 8 rows = 40 tapes, 60 KB at worst.
  Saves go out a page per pass, only while quiet (as every save does).

## Open questions

1. What the wheel's turn does while a layout is open (nothing yet).
2. How shift's latch and encoder 1's double click tell apart: a second
   click within ~300 ms is the metronome, later it only lets shift go
   (first guess, to try on the board).
3. The physics modes (Ripples, Sand, Orbits, Newton's cradle, bouncing
   notes) as layouts that play into a tape?
4. Projects: how many Decks, how you pick one, auto-save.
5. Song: chaining scenes into a song (repeat counts, or scene launches
   recorded as you play them).
6. Mute and solo per track, and track volume (encoder 1 in the Deck is
   the tempo).
7. The companion app: Decks to and from the computer (a `shared/` file
   format, with Sam).

## Order of work

1. The shift latch, everywhere (`services/buttons.c` and each mode's
   combos).
2. The tape format and engine on core 0: play, loop, pages on and off,
   record, overdub, undo; tests on the computer.
3. The Deck: tapes, scenes, launch and stop at the bar, copy, delete;
   the sequencer, drums, Song, Ableton and Volca off standalone's menu
   (`MENU_SLOTS` in `op_mode.c`).
4. A tape's steps: steps, pages, the brush, held-step notes.
5. Layouts into a tape; recording, count-in, the metronome.
6. The First Edition's wheel and encoder (once the board exists); Rev
   A0's fallback keys meanwhile.

## The reference, in short (internal: Ableton Move manual, 2026-11)

- 32 pads (4 x 8), 16 step buttons, 4 track buttons, 9 encoders, a wheel,
  a small screen, ~15 buttons (Play, Record, Loop, Capture, Copy, Delete,
  Mute, Undo, Shift, ...).
- 4 tracks, each an instrument or kit with two effects.
- Session: a row per track, a column per scene, up to 8 clips a track; a
  press launches at the next bar.
- Steps: 16 steps, pages beyond; hold a step + wheel for length.
- Recording: Record with a 1-bar count-in; overdub; Capture keeps what
  you just played; undo / redo.
- No song mode (a common request): room for TILES to do better.
