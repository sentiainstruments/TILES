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

**Touch vs. click**: a bare capacitive touch never does anything in
Ableton -- it only gives haptics (a strong "ready" click on a pad with a
clip, a continuous buzz while that clip is playing). A pressure click
(push the pad ~half way down, the same "push to select" feel as the
mode menu) is what acts: fire a clip, launch a scene (right-hand
column), or stop a clip that's already playing. Underglow flashes Sentia
purple for a scene, the clip's own color for a clip.

**Delete a clip**: hold shift and touch a clip's pad for 3 seconds (pad
and underglow go red); the clip is deleted in Ableton (Cmd+Z undoes
it). Releasing either early cancels. The right-hand scene column glows
Sentia purple whenever anything in that scene has a clip, on any track.

**Record a new clip**: a pressure click on an EMPTY slot arms that track
and starts recording into the slot; if the track takes MIDI, TILES then
opens melodic mode (once your fingers are off the pads) so you can play
straight into it. The script itself disarms every other track first (it
used to rely on Live's own Exclusive Arm preference, which it can't
see or guarantee is on -- see "Exclusive arm fixed" below). Shift+diamond
while in that melodic capture ends the recording (the clip starts
looping) and returns to Scene Launch mode -- it never enters Song mode
capture from this flow.

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
custom SysEx sub-protocol onto plain CC. A first attempt used Note-On
instead (matching how a real Launchpad sends its own grid), but real
feedback caught the actual problem: "you fully broke how clip
lounching works now its just sending regular midi notes for me to
map. thats not how this feature operates ever in any device." A real
Launchpad never sends musical notes at all, so nobody enables its
port's Track input in Ableton -- TILES's port ALSO carries real notes
for melodic play, so the user's own instrument track (listening on
"All Channels" for MPE) receives those "button" Note-Ons too, as
ordinary playable content. CC has no such conflict, matching this
project's own already-working transport CCs exactly. Ableton -> TILES
color feedback is still SysEx, unchanged. See `scene_launch.py`'s own
module docstring and `shared/protocol/README.md`'s "Scene Launch"
section for the full wire format.

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

**Exclusive arm fixed.** Real feedback: "automation arm is not
switching exclusively to the track thats going to get the new clip."
`_record_new_clip()` (fired on a pressure click into an empty slot,
per "if were recording a new clip make it open melodic mode
automatically and arm that channel") used to arm only the target
track and rely on Live's own Exclusive Arm preference to disarm every
other track -- a per-user Live setting this script has no way to see
or guarantee is on. With it off, every previously-armed track stayed
armed too, so the new recording wasn't landing on just the one track
the player picked. Fixed by having `_record_new_clip()` explicitly
disarm every other currently-armed track itself before arming the
target, independent of that Live preference.

**New tracks now get tracked too.** Real feedback: "when a pattern is
edited within ableton without the instrument it doesnt register that
it happened and acts like its not there. it tryes to recoed but it
dosnt because theres soemthing so it shouldnt." Root cause: `_connect()`
used to enumerate `self._song.tracks` exactly once, at script load --
a track created afterward (a fresh, not-yet-instrumented one is the
obvious way to get one) never had its clip slots' `has_clip`/
`playing_status`/`is_triggered` listeners wired up at all, so a clip
added there (editing directly in Ableton, same as any other way) never
sent a `clip_state` SysEx message and the pad for that slot kept
showing empty on the hardware. Confirmed this wasn't actually a
record-vs-playback bug: `_on_grid_touch()` checks `clip_slot.has_clip`
LIVE off Ableton at touch time, so it always correctly fired the
existing clip rather than trying to record over it -- the reported
"tries to record but doesn't" was the pad's stale, never-updated LED
lying about the slot being empty, not a wrong action being taken.
Fixed with `Song.add_tracks_listener()` (fires on any track added,
removed, or reordered), which now tears down and rebuilds every
per-track/per-slot listener against the current track list whenever
tracks change -- the connect-time setup loop was extracted into
`_connect_track_clip_listeners()`/`_disconnect_track_clip_listeners()`
so both the initial connect and this resync share the exact same code.

## Melodic mode: echoing a track's melody (TILES DISPLAY)

Real feedback: "in midi melodic mode is there any way we could read the
playing melody of the armed track and display it back on tiles?" -- then,
once the first version's instructions met a real Ableton setup: "ableton
instruments send either midi or audio after the vst... we need to build
a max for live device that slots in between... we can call it VIEW...
the plugin is called TILES DISPLAY."

**Why a device, not just routing.** The first version of this section
told you to point the armed track's MIDI *output* at TILES. That only
works for a track with NO instrument on it -- the moment a track has a
VST/instrument, everything after that instrument is AUDIO, so there's no
MIDI output left to route. And a Remote Script (`TILES.py`/
`scene_launch.py`) can't fill the gap: it only ever sees its own MIDI
port, not a track's MIDI. A Max for Live **MIDI Effect placed before the
instrument** sees every note that actually reaches it -- clip playback,
live input, anything an earlier arp/chord/scale device produced -- and
passes all of it through untouched, so it changes nothing about how the
track sounds.

**How it reaches TILES.** The device calls the Live Object Model's
`ControlSurface.send_midi` on the TILES control surface, which writes
straight to that script's MIDI *output* port -- the same port
`scene_launch.py` already uses for its SysEx feedback (confirmed against
Cycling '74's LOM docs and forum reports of the same technique lighting
Push pads). No network layer, no extra Remote Script code, negligible
latency. Firmware side is unchanged from before: TILES lights the pad
that note maps to (bright green) while melodic mode is active, and
clears it on the Note-Off.

### Install (one time)

1. Copy `ableton/TILES_DISPLAY/TILES DISPLAY.amxd` into your User
   Library, e.g. `~/Music/Ableton/User Library/Presets/MIDI Effects/Max
   MIDI Effect/` (Live's browser: *Max for Live > Max MIDI Effect >
   User Library*), or just drag it from Finder onto a track. Needs Max
   for Live (included in Live Suite).
2. Put it on the MIDI track **before the instrument** (MIDI effects sit
   to the left of the instrument in the device chain).

### Use

- **VIEW** -- the arm toggle. Sentia pink when armed: that track's notes
  show on TILES (green pads). **Two TILES DISPLAYs can be armed at once**
  (real feedback: "make the device work on 2 channels at once, if 2
  devices are on then the secondary does color red"): the first one armed
  is the primary -- pink button, green pads, MIDI channel 1 -- and the
  second is the secondary -- **red** button, **red** pads, MIDI channel 2.
  Arming a third replaces the secondary (the primary is never bumped).
  Turning the primary off promotes the secondary to primary (its button
  goes pink, its pads green) so a lone armed device is never left red.
  The instances coordinate through Max's global name space, so no
  configuration is needed. Turning VIEW off clears any pad still lit.
  VIEW is a normal Live parameter, so it's saved with the set and can be
  MIDI/key mapped (it always loads off).
  **Needs the matching firmware** -- an older board would show the
  secondary's notes in green too (it doesn't know channel 2 is special).
  **Replace any device already in a set** with this version (delete it and
  drag the updated one in): a saved instance keeps its old patcher, and an
  old and a new instance don't talk to each other.
- **SURFACE** -- which control surface TILES is, 1-7. Set once, saved
  with the set. This exists because the Live Object Model gives a device
  no way to ask a control surface what script it is. **It is NOT the
  Preferences slot number**: Live's own device bridge (`_MxDCore/
  LomTypes.py`, `get_control_surfaces()`) is `tuple(filter(lambda c: c is
  not None, application.control_surfaces))` -- empty slots are skipped,
  so this counts *loaded* scripts in slot order. (The first version of
  this doc told you to enter the slot number; that was wrong, and Live's
  own Log.txt showed why: every send was rejected with "no valid object
  set".) To find the right number, **just step SURFACE from 1 upward:
  whenever it changes, and whenever VIEW is armed, the device flashes a
  run of pads on TILES for about a third of a second** -- the number that
  makes pads flash green (TILES in melodic mode) is TILES. A wrong number
  is harmless if the script at that position has no MIDI output port, but
  would send notes to another controller's hardware if it does, which is
  why this isn't guessed automatically.

TILES must be in melodic mode, and a note outside the currently selected
scale/octave/key has no pad to light (same accepted tradeoffs as the
firmware entry in `firmware/src/services/README.md`).

### MPE / expression pass-through

Real feedback: "the plugin is killing mpe behaviour can we make it even
more pass through so theres no mpe or expression loss." The device's MIDI
thru was already a single direct `midiin` -> `midiout` patchline with
nothing parsed or filtered on it, so the wiring wasn't the problem. The
cause was that a Max for Live device has to **declare** MPE support -- the
patcher's `is_mpe` property ("Patch Supports MPE" in Max's patcher
inspector) -- or Live doesn't hand it the per-note MPE stream (per-note
pitch bend, slide, channel pressure on member channels 2-16) at all; the
generator simply never set it, so it defaulted to 0. Max's own help text
says so ("To receive MPE, make sure the 'Patch Supports MPE' (is_mpe)
attribute is set to 1 for the device"), as does a Cycling '74 forum
thread on passing MPE through a MIDI effect, and real devices serialize
it next to `"latency"`. The generator now sets `is_mpe: 1`. Nothing else
about the thru changed -- deliberately no `midiparse`/`midiformat`/
`mpeparse` on it (the first two are documented to truncate per-note pitch
bend to semitones), and everything the device does beyond passing MIDI
through (the note tap, the route flash, the disarm flush) runs off to the
side and never writes back into the chain.

**Replace any device already in a set:** a saved instance keeps the old
patcher, so delete it and drag the updated one in (or re-drag from the
browser). Then check MPE end to end: play with pitch bend/pressure on a
track whose MIDI input has MPE enabled, with the device armed and
disarmed -- expression should be identical either way. Not tested in Live
from here; if it still flattens, tell me exactly what's lost (bend, slide,
pressure) and whether the instrument after it has MPE turned on.

### The device's source

`ableton/TILES_DISPLAY/build_tiles_display.py` generates the `.amxd`
(same container format Ableton's own factory "Max MIDI Effect" template
uses) -- edit the device there and re-run `python3 build_tiles_display.py`
rather than hand-editing the binary-wrapped file. The generator checks
every connection points at a real inlet/outlet before writing.

### Verification status -- read this first time

**Written and structurally validated, never opened in Live yet** (no way
to drive Live's UI from where this was written). First-run checklist:

1. Drop the device on a MIDI track before an instrument; it should load
   with no red/errors in Max's console and show the dark panel, a pink
   underline, the **VIEW** button, and **SURFACE**.
2. TILES in melodic mode: step **SURFACE** from 1 upward until pads
   flash green on TILES, then click **VIEW** -- button turns Sentia
   pink (pads flash once more); play a note on that track and the
   matching pad should light green, then go dark on release.
3. Add a second instance on another track and arm it -- both VIEW
   buttons stay on: the first pink, the second **red**, and the second
   track's notes light **red** pads while the first's stay green (pads
   also flash red when the second one arms).
4. Arm a third instance -- the red one switches itself off and its pads
   clear; the pink one is untouched. Then turn the pink one off -- the
   remaining device turns pink and its notes go green.
5. Stop transport / disarm mid-note -- no pad should stay lit.

If step 2 fails but 1 loads clean, the likely culprits, in order:
SURFACE number (above), the TILES script not selected in a Control
Surface slot with its **Output** port set, or TILES not in melodic mode.
Live's own log is the fastest way to see which:
`~/Library/Preferences/Ableton/Live <version>/Log.txt`. A line like
`call send_midi 144 60 100: no valid object set` means the device is
tapping notes fine but SURFACE points at nothing; no such lines while
nothing lights means the send is reaching a control surface and the
problem is that surface's output port or TILES's own mode.

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
