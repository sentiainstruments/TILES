# daw-integration/

Companion software that runs on the computer, not the board -- distinct
from `firmware/` (runs on the Pico 2) and `companion-app/` (the
configurator). This is what lets TILES's diamond transport remote (play/
stop/record -- see `firmware/src/services/op_mode.c`'s own
`handle_diamond_transport()`) control a DAW's transport directly, the
way a factory-recognized controller does, instead of needing a manual
per-button MIDI Map.

## Why this exists

Real feedback, in order:

1. "the diamond for now will play and stop in ableton like a toggle...
   if we hold it for 2 sec it arms record" -- first attempt sent plain
   MIDI Start/Stop (System Realtime bytes), which need the DAW's MIDI
   input to be in full external-sync mode (a continuous MIDI Clock
   stream, not just isolated Start/Stop) -- TILES never sent clock, so
   "diamond is still not doing anything."
2. Researched how a real Novation Launchkey does it instead of
   re-guessing: its transport buttons send plain MIDI CCs that Ableton
   recognizes and turns into transport actions because Ableton BUNDLES
   a Launchkey-specific Control Surface script -- not because CCs are
   inherently special. Rebuilt TILES's own side to send the same kind
   of dedicated, mappable CC per action (102/103/104 for Play/Stop/
   Record, see `OP_TRANSPORT_PLAY_CC`'s own comment in op_mode.c).
3. That still needed the user to manually MIDI-Map each of the three
   CCs by hand in Ableton's own Map Mode, once per project/setup: "i
   dont want to map manually, this should just work like it does for
   launchkey out of the box."

The honest technical ceiling: Ableton only *auto-loads* a script for
controllers it ships bundled support for. There's no way for
third-party hardware to make Ableton auto-select an unbundled script
with truly zero user action -- a brand-new Launchkey model doesn't work
out of the box either, until Ableton has been updated to include it.
What's actually achievable, and what this folder provides, is the
closest real equivalent: **one manual step, done once, ever** -- copy a
folder in, pick "TILES" from a dropdown -- not a MIDI-Map dance per
button, and nothing to redo after restarts, DAW updates, or new
projects.

## Ableton Live: one-time install

1. Copy the `ableton/TILES/` folder from this repo into Ableton's
   **User** Remote Scripts folder (this is the officially-supported
   location for third-party scripts -- no admin permission needed, and
   it survives Ableton version updates, unlike copying into the app
   bundle itself):
   - macOS: `~/Music/Ableton/User Library/Remote Scripts/`
   - Windows: `Documents\Ableton\User Library\Remote Scripts\`

   (Create the `Remote Scripts` folder if it doesn't already exist.)
   The result should be a `.../Remote Scripts/TILES/` folder containing
   `__init__.py` and `TILES.py`.
2. Restart Ableton Live if it was already open.
3. Preferences -> Link, Tempo & MIDI -> in a free Control Surface slot,
   choose **TILES** from the dropdown, then set that slot's Input and
   Output to TILES's own MIDI port.
4. Done. Diamond's short click (play/stop) and 2-second-hold-then-
   release (record) now directly drive Ableton's transport -- no MIDI
   Map Mode, no per-button setup, and nothing to repeat next session.

See `ableton/TILES/TILES.py`'s own module docstring for exactly what it
listens for and why, and `firmware/src/services/op_mode.c`'s
`handle_diamond_transport()` for the hardware side sending it.

## Scene Launch mode

Real feedback: "lets implemebt a new mode that triggers scenes in
ableton live... can we pull the colors of the scenes from ableton?"
The same `TILES/` script folder now also carries `scene_launch.py`
(imported by `TILES.py`), which fires clips/scenes on TILES's own
button presses and pushes real clip/scene colors and playing/queued
state back to the hardware -- see `shared/protocol/README.md`'s own
"Scene Launch" section for the wire format and
`firmware/src/services/op_mode.c`'s own "Scene Launch mode" section for
the hardware side.

**If you installed the Ableton script before this feature existed,
re-copy the `ableton/TILES/` folder** (step 1 above) to pick up the new
`scene_launch.py` file, then restart Ableton -- the existing Control
Surface slot picked in step 3 doesn't need reselecting, just a fresh
copy of the folder and a restart so Ableton reloads it.

**Master stop**: shift+diamond in Scene Launch mode sends Ableton's own
"stop all clips" action -- real feedback, "a master stop in this app
should be shift diamond." Distinct from the diamond's own plain click
(transport play/stop), which still works unchanged in this mode.

**Stop one clip**: pressing a playing clip's pad all the way down (not
just a normal touch) stops that one clip specifically -- real feedback,
"re pushing a playing clip pad all the way down or close to that stops
the individual clip."

**Session-ring outline**: Scene Launch mode now shows Ableton's own
built-in session-ring box in Session View, sized to the same 5-track x
4-scene window the hardware shows and following the same "-"/"+" pan --
real feedback, "i need that outline for tiles as well," after finding
out the box the user had seen previously was actually their other
(Novation) controller's own overlay, not anything TILES's script drew.

**Architecture change**: after several real-hardware rounds with no
confirmed successful delivery of master stop, fire, or individual stop
-- "master stop doesnt work at all, individual start and stop doesnt
work and hasent for the past few pushes. i need you to look at how a
lounchapd works or abletoun push works to pull the exxact same
standardizre behaviour" -- every TILES -> Ableton message (fire clip,
launch scene, stop all, stop one clip, track-offset sync) moved off a
custom SysEx sub-protocol onto plain Note-On/CC, matching real
Launchpad-family Remote Scripts (and this project's own already-
working transport CCs) exactly. Ableton -> TILES color feedback is
still SysEx, unchanged. See `scene_launch.py`'s own module docstring
and `shared/protocol/README.md`'s "Scene Launch" section for the full
wire format.

**Debugging**: real feedback found colors weren't showing on first
try -- root cause was `scene_launch.py` monkey-patching an attribute
directly onto Ableton's own native `Clip` object, which isn't
guaranteed to support that and could silently abort the whole script's
setup (fixed: it now tracks state in a plain dict it owns instead).
A second round found colors STILL not updating and clip fires not
reaching Ableton either -- two more real bugs, root-caused against
Ableton's own bundled Remote Script source rather than guessed at a
third time: `handle_sysex` doesn't actually receive the `0xF0`/`0xF7`
SysEx framing (Ableton's framework strips it first), and `ClipSlot` has
no `add_is_playing_listener` (the real listener is `add_playing_status_
listener`). A third round found master stop and individual stop STILL
not working with no further bug findable by inspection -- see the
architecture change above for how that direction was rebuilt entirely.
`scene_launch.py` logs every real action it takes (connecting, each
clip/scene color push, every grid touch/stop/master-stop it receives)
to Ableton's own log -- if something still isn't updating, check there
first:

- macOS: `~/Library/Preferences/Ableton/Live <version>/Log.txt`, or
  Ableton's own Help menu -> Show Log.
- Look for lines starting `[TILES scene_launch]`. No lines at all means
  the script never even loaded/connected (check step 3's Control
  Surface slot is actually set to TILES); a `failed to connect` line
  names the real error; `connected` with no further `clip_state`/
  `scene_state` lines after touching a clip means the listeners aren't
  firing (a genuinely open question against a real session, see
  `scene_launch.py`'s own module docstring).

## Other DAWs

This specific script is Ableton-only -- Logic, Cubase, Reaper, Bitwig,
and others each have their own, completely different control-surface/
scripting architectures, and a real "just works" integration for any of
them would need its own separate script written against that DAW's own
API, not something this Ableton script (or TILES's firmware) can cover
for free. Until/unless one of those gets built, TILES's firmware still
sends the same three CCs (102/103/104) regardless of which DAW is
listening, so the fallback for any other DAW is exactly what this
folder exists to avoid needing for Ableton specifically: a one-time
manual MIDI-Map/MIDI-Learn of each CC to that DAW's own Play/Stop/
Record, using whatever generic-mapping feature it provides.
