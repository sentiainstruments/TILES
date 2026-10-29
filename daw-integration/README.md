# daw-integration/

Software that runs on the computer and makes TILES behave like a
dedicated controller in a DAW, with no per-button MIDI mapping. Ableton
Live only for now:

| Path | What it is |
|---|---|
| `ableton/TILES/` | The TILES Control Surface script: the diamond transport, and the Session View clip grid of Ableton mode (Scene Launch) with Live's own clip colors. |
| `ableton/TILES_DISPLAY/` | TILES DISPLAY, a Max for Live MIDI Effect that shows the notes a track is playing on TILES's pads. |

`HISTORY.md` has the design history: the feedback, dead ends and bugs
behind each part (why CCs rather than SysEx or Note-On, why a separate
DAW port, the listener bugs). Read it before changing how the script or
the device talks to TILES.

## Two MIDI ports

TILES shows up as two USB MIDI ports, like Launchkey, Push or KeyLab
(`firmware/src/midi/midi_ports.h`). Each unit's name carries its unit
number (`firmware/src/board/unit_id.h`); unit 2's are:

- **SENTIA TILES 2 (MIDI)** in Live ("SENTIA TILES 2 MIDI" elsewhere):
  the instrument. Notes, MPE, pedals, and clock/Start/Stop for sync.
  Record and play synths from this one. The DIN jack carries the same.
- **SENTIA TILES 2 (DAW)** in Live ("SENTIA TILES 2 DAW" elsewhere): the
  control surface script's own port. Transport and Ableton-mode
  messages go out on it; clip colors and the TILES DISPLAY notes come
  back on it. Never on DIN, never meant for a track.

Drum mode plays on **MIDI channel 10** of the MIDI port, in Drum Rack
order (first bank C1-G1 = Live's first 8 pads, `firmware/src/services/
drum_seq.h`). Give the Drum Rack its own track with input "SENTIA TILES N
(MIDI)", channel 10, and monitor In; the melodic track's input then
wants channels 1-9 (or all, if it should hear the drums too).

A Live set made with one unit doesn't find another by itself: pick the
other unit's ports in the set and in Preferences. (Firmware before 0.2.1
named every unit plain "SENTIA TILES".)

## Ableton Live: install (once)

1. Copy `ableton/TILES/` into Ableton's **User** Remote Scripts folder
   (create `Remote Scripts` if it doesn't exist):
   - macOS: `~/Music/Ableton/User Library/Remote Scripts/`
   - Windows: `Documents\Ableton\User Library\Remote Scripts\`

   The result is `.../Remote Scripts/TILES/` containing `__init__.py`,
   `TILES.py` and `scene_launch.py`.
2. Restart Live.
3. Preferences -> Link, Tempo & MIDI -> in a free Control Surface slot
   pick **TILES**, and set that slot's Input and Output to the unit's
   **(DAW)** port, e.g. **SENTIA TILES 2 (DAW)**. Not another
   controller's DAW port: every "(DAW)" entry looks alike, and a slot
   pointed at the wrong one does nothing. Then, in the MIDI Ports list:
   - **In: SENTIA TILES 2 (MIDI)**: Track **on**, MPE **on** (see
     "Sustain pedal and MPE"), Sync **off** (Sync plus Live's EXT button
     makes a clock loop), Remote only if you MIDI-map something from
     TILES.
   - **Out: SENTIA TILES 2 (MIDI)**: Sync **on** if TILES's sequencer
     should follow Live's clock.
   - **In: SENTIA TILES 2 (DAW)**: Track **off** (without the script
     loaded, its messages would otherwise record into clips).
   - **Out: SENTIA TILES 2 (DAW)**: nothing needed.

Ableton only auto-loads scripts it ships with. The script declares its
ports and USB ID to Live (`__init__.py`, `get_capabilities()`, the same
declaration Ableton's Launchkey MK3 script makes), but in testing Live
still didn't select it by itself, so step 3 is needed once. Nothing has
to be redone after restarts, Live updates or new sets.

**After updating the firmware or the script**, re-copy `ableton/TILES/`
and restart Live (or set the TILES slot to None and back). The two must
match: the CC numbers and the DAW port are shared between them.

**macOS: stale device after a firmware update.** macOS caches a USB MIDI
device (by its serial number) and may not notice its ports or name
changed. In **Audio MIDI Setup -> Window -> Show MIDI Studio**:

- Old name (a unit still called plain "SENTIA TILES" after updating to
  0.2.1): double-click its icon and rename it **SENTIA TILES N**, N being
  its unit number (`tools/tiles_control.py info` shows `unit=`). The name
  stays with that board, whichever USB port it's in.
- One port instead of two: quit Live, unplug TILES, delete every
  greyed-out **SENTIA TILES** icon, and plug TILES back in.

## Two units at once

Each unit has its own ports (unit 2's "SENTIA TILES 2 (...)", unit 4's
"SENTIA TILES 4 (...)"), so Live keeps them apart whatever order they're
plugged in. Set up each one as in install step 3:

- One **TILES** Control Surface row per unit, each on that unit's
  **(DAW)** port.
- Each unit's **(MIDI)** input with Track and MPE on.
- Each instrument track's **MIDI From** set to the unit that plays it,
  e.g. "SENTIA TILES 2 (MIDI)", not "All Ins": on All Ins a track hears
  both units, and their MPE notes share channels 2-16.

Then:

- Both units drive the same Live set: either diamond starts and stops
  the transport, and each unit's Ableton mode has its own five-track
  window.
- Recording a new clip from one unit (circle + an empty slot) disarms
  the other armed tracks, except a track whose input is the other unit,
  so the other player keeps playing.
- TILES DISPLAY: each device's **TILES** box picks the unit (1 = the
  first TILES row in Preferences, 2 = the second), and each unit shows
  up to two tracks.

## Transport (diamond)

The diamond drives Live's transport outside the sequencer
(`handle_diamond_transport()` in `firmware/src/services/op_mode.c`). On
the DAW port, channel 1:

**Performance layout (on now, `OP_TRANSPORT_SHIFT_STOP` in `op_mode.c`
and `DIAMOND_IGNORED_WHILE_PLAYING` in `TILES.py`, set together):**

| Gesture | CC | Script action |
|---|---|---|
| Click | 102 | `song.start_playing()`, only if Live is stopped |
| Hold 2 s, release | 104 | `record_mode = True`, only if Live is stopped |
| Circle + diamond | 103 | `record_mode = False`, then `song.stop_playing()` |

While Live plays, diamond alone does nothing: the script checks Live's
own state, so it holds when the other unit or the mouse started the
transport. Circle + diamond's usual jobs (Ableton stop-all, Song capture)
are off in this layout.

**Toggle layout (both switches off):**

| Gesture | CC | Script action |
|---|---|---|
| Click, stopped | 102 | `song.start_playing()` |
| Click, playing or recording | 103 | `record_mode = False`, then `song.stop_playing()` (stopping also ends a recording) |
| Hold 2 s, release | 104 | `record_mode = True` (Live's count-in preference applies) |

"Stopped/playing" is TILES's own transport state, which the diamond
toggles. Each trigger is sent as 127 then 0; the script acts on the 127
only. Unless it is following an external clock, TILES also sends MIDI
Start/Stop on the MIDI port and DIN, for gear synced to it.

## Ableton mode (Scene Launch)

Open with triangle -> pad 6 (teal). The grid is a window onto Session
View:

- Rows 1-4 = scenes 1-4 (scenes don't page). Columns 1-5 = five tracks'
  clip slots; "-"/"+" pan the five-track window. Column 6 launches the
  whole scene.
- A **touch** only gives haptics: a strong click on a pad with a clip, a
  steady buzz while that clip plays. A **pressure click** (press past
  half way, like picking in a menu) acts:
  - on a clip: fire it; on a playing clip: stop it;
  - on column 6: launch the scene;
  - on an empty slot: nothing (a plain press never records).
- **Circle held + a pressure click on an empty slot**: record a new
  clip there. The script disarms the other armed tracks (except one
  played by another TILES unit, see "Two units at once"), arms this one
  and fires the slot. If the track takes MIDI, TILES switches to melodic
  mode once your fingers are off the pads, so you can play straight into
  the recording. **Circle + diamond** then ends the recording (the clip
  starts looping) and returns to Ableton mode, in either transport
  layout.
- **Circle + diamond** (not recording): with the performance transport
  layout (on now), stops Live's transport, which also stops every clip;
  with the toggle layout, stop all clips and leave the transport
  running.
- **Circle held + a clip's pad touched for 3 s**: delete the clip (the
  pad blinks red, the underglow goes red). Releasing either cancels;
  Live's undo brings it back.
- Lighting uses Live's own clip colors: empty = off, clip = dim, playing
  = pulsing, queued = blinking. Column 6 is Sentia magenta while any
  track has a clip in that scene. The underglow is teal, flashing
  magenta on a scene launch, the clip's color on a clip action, or red
  when recording into an empty slot (when that's on).
- Ableton's session-ring box follows the five-track window in Session
  View (built; not yet confirmed that Live draws it for this script).

Wire format: `shared/protocol/README.md`, "Scene Launch". Firmware side:
the "Scene Launch (Ableton) mode" section of `op_mode.c`.

## Sustain pedal and MPE

TILES sends the sustain pedal (CC 64) the standard MPE way, on the
zone's Master Channel (channel 1) only. So in MPE mode the receiving
side must be set up for MPE, as with any MPE controller:

- Ableton: tick **MPE** (and Track) on the unit's **(MIDI)** input,
  and use an MPE-enabled instrument (Serum: its MPE switch on; Equator:
  MPE by default).
- Anything not set up for MPE: switch TILES to plain MIDI (circle +
  square); every note and the pedal then go on channel 1.

`pedal.sustain_style` (settings, `tools/README.md`) picks who sustains:
`synth` (default, standard CC 64) or `hold` (TILES holds its own notes
and sends no CC 64, so harmonic plucks ring out on their own).

## Troubleshooting the script

`scene_launch.py` logs every action to Live's log (Help -> Show Log, or
`~/Library/Preferences/Ableton/Live <version>/Log.txt` on macOS). Look
for lines starting `[TILES scene_launch]`:

- none at all: the script isn't loaded (check the Control Surface slot);
- `failed to connect: ...`: names the error. Transport keeps working
  either way; only Ableton mode is disabled;
- `connected` but no `clip_state` / `scene_state` lines when clips
  change: the listeners aren't firing.

## TILES DISPLAY: a track's notes on the pads

After an instrument a track outputs audio, so there's no MIDI left to
route to TILES, and a Remote Script can't see a track's MIDI. TILES
DISPLAY is a Max for Live **MIDI Effect placed before the instrument**:
it sees every note that reaches the instrument (clip playback, live
input, anything an earlier arpeggiator or chord device made) and sends
a copy to TILES through the Live Object Model's
`ControlSurface.send_midi` on the TILES control surface, which writes to
the DAW port. TILES lights the matching pad while in melodic mode.

### Install

1. Copy `ableton/TILES_DISPLAY/TILES DISPLAY.amxd` into your User
   Library (e.g. `~/Music/Ableton/User Library/Presets/MIDI Effects/Max
   MIDI Effect/`), or drag it from Finder onto a track. Needs Max for
   Live (included in Live Suite).
2. Put it on a MIDI track **before the instrument**.

After updating the device, replace the instances already in your sets
(delete and drag the new one in): a saved instance keeps its old
patcher, and old and new instances don't coordinate.

### Use

- **VIEW** arms the device: that track's notes show on TILES. Up to two
  can be armed per TILES unit. The first armed is that unit's primary
  (pink button, green pads, MIDI channel 1); the second is its secondary
  (soft red button and pads, channel 2). Arming a third on the same
  unit replaces its secondary; turning the primary off promotes the
  secondary. Instances on different units never affect each other.
  They coordinate through Max's global send/receive names, with no
  setup. Turning VIEW off clears any lit pad. VIEW is a normal Live
  parameter (saved with the set, mappable; always loads off).
- **TILES** (1-4) picks the unit: 1 = the first TILES row in
  Preferences' Control Surface list, 2 = the second, top to bottom. The
  device resolves it by asking each loaded surface for its script name
  (the LOM `type_name`, "TILES"), 1.5 s after loading, on every VIEW arm
  and when TILES changes; the chosen unit's pads flash for about 0.3 s
  so you can see which one it is. Changing TILES while armed disarms
  (clearing the old unit's pads); arm again. If Live can't name the
  surfaces, the number is used as the loaded-surface position instead:
  step it until the right unit's pads flash.
- TILES must be in melodic mode (not chord mode's melody grid). A note
  with no pad in the current scale/octave/key isn't shown.
- The secondary's red is set by `look.echo_secondary_g_percent` /
  `look.echo_secondary_b_percent` (settings, defaults in
  `firmware/src/services/lighting.c`).

**MPE passes through untouched.** The MIDI thru is a single direct
`midiin -> midiout` line with nothing parsed or filtered (no
`midiparse`/`midiformat`, which truncate per-note pitch bend), and the
patcher declares MPE support (`is_mpe`: 1), without which Live doesn't
pass the per-note MPE stream through the device at all. Everything the
device adds runs off to the side of the chain.

### Source

`ableton/TILES_DISPLAY/build_tiles_display.py` generates the `.amxd`.
Edit the device there and re-run `python3 build_tiles_display.py`
rather than editing the file. The generator checks that every
connection points at a real inlet/outlet before writing.

### Status and first-run checklist

The single-unit version was used in Live, including two instances at
once. The per-unit version (the TILES box, two tracks per unit) is built
and structurally checked but not yet confirmed in Live.

1. Drop the device on a MIDI track before an instrument: no errors in
   Max's console; a dark panel with a pink line, **VIEW** and **TILES**.
2. TILES in melodic mode, click **VIEW**: the button turns pink, pads
   flash on unit 1. Notes on that track light green pads, dark again on
   release.
3. Set **TILES** to 2: unit 2's pads flash (and VIEW turns off). Arm
   again: notes now light unit 2.
4. On unit 1, arm a second instance: soft red, while the first stays
   pink/green. Arm a third on unit 1: the soft-red one switches off; an
   instance on unit 2 is untouched throughout.
5. Turn unit 1's pink one off: its soft-red one turns pink, notes go
   green.
6. Stop the transport or disarm mid-note: no pad stays lit.

If the device loads but nothing lights: check the TILES slot's
**Output** is the unit's (DAW) port, then the TILES number, then that
TILES is in melodic mode. In Live's log, `call send_midi 144 60 100: no
valid object set` means notes are tapped but no TILES was found for that
number (is the script loaded, and are there that many TILES rows?).

## Other DAWs

The script is Ableton-only; every other DAW has its own control-surface
API and would need its own script. The firmware sends the same transport
CCs (102/103/104 on the DAW port) regardless, so in another DAW map them
once with its MIDI Learn to Play/Stop/Record.
