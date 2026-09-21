"""
Ableton-side half of TILES's Scene Launch mode (see
firmware/src/services/op_mode.c's own "Scene Launch mode" section for
the hardware side, and shared/protocol/README.md's "Scene Launch"
section for the full message catalog this implements).

Real feedback: "lets implemebt a new mode that triggers scenes in
ableton live keep it simple for now, push triggers it... can we pull
the colors of the scenes from ableton? and light behaviour to feel
intuitive?"

Architecture, rewritten twice after several real-hardware rounds with
no confirmed successful delivery of any TILES -> Ableton action (fire,
launch scene, stop all, stop one clip): "master stop doesnt work at
all, individual start and stop doesnt work and hasent for the past few
pushes. i need you to look at how a lounchapd works or abletoun push
works to pull the exxact same standardizre behaviour." The TILES ->
Ableton direction used to be a custom SysEx sub-protocol, handled by a
`handle_sysex()` override -- despite passing every review against
Ableton's own real Remote Script source, it never had one single
confirmed successful round-trip on real hardware.

First rewrite replaced it with plain Note-On, matching how a REAL
Launchpad sends its own grid (confirmed against Ableton's bundled
Launchpad.py: `ConfigurableButtonElement(is_momentary, MIDI_NOTE_TYPE,
0, ...)`). Real feedback caught the real flaw: "you fully broke how
clip lounching works now its just sending regular midi notes for me to
map. thats not how this feature operates ever in any device." A real
Launchpad is a DEDICATED grid controller that never sends musical note
content at all, so nobody ever enables that port's "Track" MIDI input
in Ableton. TILES is not that -- this exact same USB-MIDI port also
carries real musical Note-On for melodic/chord/guitar/sequencer play,
so the user's own instrument track almost certainly already has this
port's Track input enabled (typically listening on "All Channels,"
required for real MPE playback) -- meaning a Scene Launch "button"
Note-On, on ANY channel, is ALSO delivered to that track as ordinary
playable/recordable note content, on top of whatever this script's own
`ButtonElement` does with it. Being claimed by the Control Surface's
Remote path and ALSO reaching a Track's input are not mutually
exclusive in Ableton. A Control Change never has this problem --
Ableton never treats a CC as note/audio content for an instrument
regardless of Track/Remote routing, exactly why the transport CCs
below have always been safe on this same port. Second rewrite moved
grid-touch/stop-touch off Note-On entirely, onto CC, same as
everything else here already was -- see NOTE_GRID_BASE's own
replacement, CC_GRID_BASE, for the current wire format.

Both rewrites bind real `ButtonElement`s via `add_value_listener()` --
the same mechanism TILES.py's own transport buttons (play/stop/record)
already use, with actual confirmed delivery on this exact hardware/
Ableton/script combination. This file keeps its own fire/stop logic
here rather than handing it to `SessionComponent`/`ClipSlotComponent`
directly -- those components' own LED feedback is a small quantized
color palette, and keeping this file's own logic preserves real
per-pad RGB feedback (see below).

Real bug found from live testing after the CC rewrite: "no click is
triggering anything," even though colors had started updating
correctly. Root cause: `_connect()` used to call `self._control_
surface.register_components(self._session)` to wire up the session
ring below -- `ControlSurface` has no such public method (confirmed
directly in `_Framework/ControlSurface.py`'s own source: only a
private `_register_component`, exposed to real `ControlSurfaceComponent`
instances via dependency injection, not callable externally like
this). That raised an `AttributeError` immediately, aborting the rest
of `_connect()` -- everything before it (the clip/scene color
listeners) had already run, which is exactly why colors worked but no
button below that line ever got bound. Fixed with the real, public
API for this -- `set_highlighting_session_component()`, confirmed both
in `ControlSurface.py`'s own source and by Ableton's bundled
`Launchpad.py`, which calls this exact method on itself.

The Ableton -> TILES direction (clip/scene color + state) is
UNCHANGED, still plain SysEx -- that direction was never reported
broken, and a real RGB color feed has no equivalent in a single CC
value anyway.

Wire protocol summary:

    TILES -> Ableton (plain CC, TILES_MASTER_CHANNEL, no SysEx, no
    Note-On -- see above for why Note-On specifically doesn't work here):
        CC, controller = CC_GRID_BASE + pad (11-34), 127 then 0
            grid touch: fire that pad's clip (columns 1-5) or launch
            that pad's whole scene (column 6, OP_SCENE_LAUNCH_COL in
            op_mode.c)
        CC, controller = CC_STOP_BASE + pad (41-64), 127 then 0
            deep-press stop of that one clip (columns 1-5 only)
        CC CC_MASTER_STOP (105), value 127 then 0        stop all clips
        CC CC_TRACK_OFFSET (106), value = offset         visible track
            window changed (session ring + grid-touch track mapping)

    Ableton -> TILES (SysEx, manufacturer ID 0x7D = MMA-reserved
    "non-commercial/educational use", sub-ID 0x01):
        F0 7D 01 10 <track> <scene> <flags> <r7> <g7> <b7> F7   clip state
        F0 7D 01 11 <scene> <flags> <r7> <g7> <b7>         F7   scene state

    flags bit 0 = has_clip (clip state only), bit 1 = is_playing (clip
    state only), bit 2 = is_triggered (both). <r7>/<g7>/<b7> are each
    0-127 (Ableton's own 0-255 channel value halved, see
    _color_to_wire_rgb() below) -- this hardware doubles them back
    toward 8-bit on receipt (see op_mode.c's own scene_on_sysex()),
    losing the bottom bit, not the top.

A grid-touch/stop CC's pad number maps to (column, row) exactly like
op_mode.c's own handle_scene_launch_taps() does (column = (pad-1) % 6
+ 1, row/scene = (pad-1) // 6); the real track index is column-1
offset by whatever CC_TRACK_OFFSET last reported (see
_pad_to_col_track_scene() below) -- this script has to track that
itself now, the same thing op_mode.c's own s_scene_track_offset already
does firmware-side.

Also owns a plain _Framework.SessionComponent (see _connect()'s own
comment) purely for Ableton's own built-in session-ring overlay in
Session View -- real feedback: "the box was from my novation. i need
that outline for tiles as well tho." Sized to the same 5-track x
4-scene window op_mode.c's own grid shows, kept in sync via
CC_TRACK_OFFSET above, and wired to the control surface via
set_highlighting_session_component() -- the same real API Ableton's
own bundled Launchpad.py uses for its own ring. Still genuinely
unconfirmed whether Ableton draws the ring without any
ButtonMatrixElement ever bound to the component (this script keeps
driving its own SysEx-based color feedback instead of handing that job
to it) -- wrapped in the same try/except as the rest of _connect(), so
if that guess is wrong it logs and leaves everything else working
either way.

Only the first MAX_TRACKS tracks and NUM_SCENES scenes are ever pushed
or listened to for color feedback -- matches firmware/src/services/
op_mode.c's own fixed-size state table, and "keep it simple for now"
means no attempt yet to page scenes beyond the first 4 (real feedback's
own Q&A settled "-"/"+" as a TRACK pan, not a scene page, for this
version).
"""

from _Framework.ButtonElement import ButtonElement
from _Framework.InputControlElement import MIDI_CC_TYPE
from _Framework.SessionComponent import SessionComponent

# Must match op_mode.c's own TILES_MIDI_MPE_MASTER_CHANNEL (0 = MIDI
# channel 1) -- same channel TILES.py's own transport buttons already
# use; not imported from TILES.py to avoid a circular import (TILES.py
# imports this module).
TILES_MASTER_CHANNEL = 0

# Must match op_mode.c's own OP_SCENE_CC_GRID_BASE/_STOP_BASE/
# _MASTER_STOP/_TRACK_OFFSET -- keep all four in sync with that file
# if they ever change there. All CC, not Note-On -- see this module's
# own docstring for why Note-On specifically doesn't work on this
# port.
CC_GRID_BASE = 10
CC_STOP_BASE = 40
CC_MASTER_STOP = 105
CC_TRACK_OFFSET = 106

# Must match TILES_NUM_PADS (board_layout.h) -- every pad on the grid,
# used to build one grid-touch and one stop-touch ButtonElement per
# pad (the stop ones for column-6 pads are simply never triggered,
# op_mode.c only ever sends that CC for track columns 1-5).
NUM_GRID_PADS = 24

SYSEX_MFR_ID = 0x7D
SYSEX_SUB_ID = 0x01

MSG_CLIP_STATE = 0x10
MSG_SCENE_STATE = 0x11

FLAG_HAS_CLIP = 0x01
FLAG_IS_PLAYING = 0x02
FLAG_IS_TRIGGERED = 0x04

# Must match firmware/src/services/op_mode.c's own OP_SCENE_MAX_TRACKS/
# OP_SCENE_NUM_ROWS -- keep these three in sync with that file if they
# ever change there.
MAX_TRACKS = 64
NUM_SCENES = 4

# Must match op_mode.c's own OP_SCENE_TRACK_COL_MAX - OP_SCENE_TRACK_COL_MIN
# + 1 -- the number of track columns actually shown on the hardware at
# once, used only to size the SessionComponent ring, not the clip/scene
# cache above (which still tracks every MAX_TRACKS regardless of what's
# currently scrolled into view).
NUM_VISIBLE_TRACKS = 5


def _color_to_wire_rgb(color_int):
    """Ableton clip/scene `.color` is a packed 0xRRGGBB int. Halves each
    8-bit channel down to a 7-bit MIDI data byte (0-127) -- lossy, and
    deliberately so: matches op_mode.c's own scene_on_sysex() comment on
    why this hardware's LEDs don't need the missing bit of precision
    back badly enough to justify a more expensive/complex lossless
    encoding for this first version."""
    r = (color_int >> 16) & 0xFF
    g = (color_int >> 8) & 0xFF
    b = color_int & 0xFF
    return r >> 1, g >> 1, b >> 1


def _pad_to_col_row(pad):
    """Inverse of op_mode.c's own board_pad_for_row_col() as used by
    handle_scene_launch_taps() -- pad is 1-based (1..NUM_GRID_PADS)."""
    col = (pad - 1) % 6 + 1
    row = (pad - 1) // 6
    return col, row


class SceneLaunch(object):
    """Owned by TILES.py (see that file's own __init__/disconnect) --
    kept as a separate object rather than folded into the TILES class
    itself so the already-real-hardware-tested transport-remote code
    stays completely undisturbed by this newer addition.
    """

    def __init__(self, control_surface):
        self._control_surface = control_surface
        self._song = control_surface.song()
        # One listener closure per (track, scene) clip slot and per
        # scene, kept alive for as long as this object exists so they
        # can be individually removed in disconnect() -- Ableton's own
        # add_*_listener/remove_*_listener pattern needs the EXACT same
        # callable passed to both, not just an equivalent one, so these
        # have to be stored, not recreated on disconnect.
        self._clip_slot_listeners = []  # list of (clip_slot, has_clip_cb, playing_status_cb, is_triggered_cb)
        # keyed by (track_index, scene_index) -- see _on_has_clip_changed()'s
        # own comment on why this replaced an earlier, real bug (monkey-
        # patching an identifying attribute directly onto Ableton's own
        # native Clip object, which isn't guaranteed to support arbitrary
        # attribute assignment and could silently abort this whole
        # object's __init__ if it ever raised).
        self._clip_color_listeners = {}  # {(track_index, scene_index): (clip, color_cb)}
        self._scene_listeners = []  # list of (scene, is_triggered_cb, color_cb)
        self._session = None  # SessionComponent, created in _connect() -- see that method's own comment
        self._grid_button_listeners = []  # list of (button, callback), pad 1..NUM_GRID_PADS
        self._stop_button_listeners = []  # list of (button, callback), pad 1..NUM_GRID_PADS
        self._master_stop_button = None
        self._track_offset_button = None
        # Mirrors op_mode.c's own s_scene_track_offset -- this script
        # has to track it independently now that grid-touch/stop
        # messages carry only a pad number, not a track index (see
        # CC_TRACK_OFFSET's own comment above).
        self._track_offset = 0
        try:
            self._connect()
            self._log("connected")
        except Exception as e:  # noqa: BLE001 -- see this except's own comment
            # Whatever the exact cause, an exception anywhere in
            # _connect() used to propagate all the way up through
            # TILES.__init__()'s own component_guard(), which would
            # silently abort the WHOLE script -- taking the already-
            # working transport remote down with a completely
            # unrelated Scene Launch bug, the opposite of "a failed
            # subsystem disables itself, it never takes other
            # subsystems down with it." Caught here instead, logged so
            # it's actually visible (Ableton's own Log.txt, Help ->
            # Show Log), and left non-fatal: the transport buttons in
            # TILES.py keep working either way.
            self._log("failed to connect: %s" % e)

    def _log(self, message):
        # self.log_message() writes to Ableton's own Log.txt -- the
        # standard way to see what a Remote Script is actually doing,
        # since there's no console output visible otherwise. Every real
        # action this object takes logs one line, on purpose, while this
        # is still being confirmed against a real session -- trim this
        # down once it's confirmed working end to end.
        self._control_surface.log_message("[TILES scene_launch] " + message)

    # ---- Ableton -> TILES (SysEx, unchanged) --------------------------------

    def _send_clip_state(self, track_index, scene_index, clip_slot):
        has_clip = clip_slot.has_clip
        is_playing = has_clip and clip_slot.is_playing
        is_triggered = clip_slot.is_triggered
        color = clip_slot.clip.color if has_clip else 0
        r7, g7, b7 = _color_to_wire_rgb(color)
        flags = 0
        if has_clip:
            flags |= FLAG_HAS_CLIP
        if is_playing:
            flags |= FLAG_IS_PLAYING
        if is_triggered:
            flags |= FLAG_IS_TRIGGERED
        self._log(
            "clip_state track=%d scene=%d has_clip=%d playing=%d triggered=%d rgb=(%d,%d,%d)"
            % (track_index, scene_index, has_clip, is_playing, is_triggered, r7, g7, b7)
        )
        self._control_surface._send_midi(
            (
                0xF0,
                SYSEX_MFR_ID,
                SYSEX_SUB_ID,
                MSG_CLIP_STATE,
                track_index,
                scene_index,
                flags,
                r7,
                g7,
                b7,
                0xF7,
            )
        )

    def _send_scene_state(self, scene_index, scene):
        r7, g7, b7 = _color_to_wire_rgb(scene.color)
        flags = FLAG_IS_TRIGGERED if scene.is_triggered else 0
        self._log("scene_state scene=%d triggered=%d rgb=(%d,%d,%d)" % (scene_index, scene.is_triggered, r7, g7, b7))
        self._control_surface._send_midi(
            (0xF0, SYSEX_MFR_ID, SYSEX_SUB_ID, MSG_SCENE_STATE, scene_index, flags, r7, g7, b7, 0xF7)
        )

    def _make_scene_callback(self, scene_index, scene):
        return lambda: self._send_scene_state(scene_index, scene)

    def _make_slot_callback(self, track_index, scene_index, clip_slot):
        return lambda: self._send_clip_state(track_index, scene_index, clip_slot)

    def _connect(self):
        tracks = self._song.tracks
        scenes = self._song.scenes
        num_tracks = min(len(tracks), MAX_TRACKS)
        num_scenes = min(len(scenes), NUM_SCENES)
        self._log("connecting: %d track(s), %d scene(s) tracked" % (num_tracks, num_scenes))

        for scene_index in range(num_scenes):
            scene = scenes[scene_index]
            is_triggered_cb = self._make_scene_callback(scene_index, scene)
            color_cb = self._make_scene_callback(scene_index, scene)
            scene.add_is_triggered_listener(is_triggered_cb)
            scene.add_color_listener(color_cb)
            self._scene_listeners.append((scene, is_triggered_cb, color_cb))
            self._send_scene_state(scene_index, scene)

        for track_index in range(num_tracks):
            track = tracks[track_index]
            for scene_index in range(num_scenes):
                clip_slot = track.clip_slots[scene_index]
                # is_playing/is_triggered are listened on the ClipSlot
                # itself, not the Clip inside it -- the slot is stable
                # for the lifetime of the (track, scene) position, so
                # these never need re-registering when a clip is added/
                # removed/replaced, unlike color below (a Clip-only
                # property). playing_status (NOT is_playing, which has
                # no listener of its own) confirmed against Ableton's
                # bundled _Framework/ClipSlotComponent.py.
                has_clip_cb = self._make_slot_callback(track_index, scene_index, clip_slot)
                playing_status_cb = self._make_slot_callback(track_index, scene_index, clip_slot)
                is_triggered_cb = self._make_slot_callback(track_index, scene_index, clip_slot)
                clip_slot.add_has_clip_listener(has_clip_cb)
                clip_slot.add_playing_status_listener(playing_status_cb)
                clip_slot.add_is_triggered_listener(is_triggered_cb)
                self._clip_slot_listeners.append((clip_slot, has_clip_cb, playing_status_cb, is_triggered_cb))
                self._on_has_clip_changed(track_index, scene_index, clip_slot)

        # Real feedback: "the box was from my novation. i need that
        # outline for tiles as well tho" -- Ableton's own built-in
        # session-ring overlay in Session View, which SessionComponent
        # (Ableton's own framework class for exactly this) draws once
        # it's given a size/offset and hooked up as the control
        # surface's highlighting source. Not wired to any
        # ButtonMatrixElement -- this script keeps driving LED feedback
        # itself over the existing SysEx protocol, so this component's
        # only job is the visual ring.
        self._session = SessionComponent(NUM_VISIBLE_TRACKS, num_scenes)
        self._session.set_offsets(0, 0)
        # Real bug found from live testing ("no click is triggering
        # anything," after colors started working): `ControlSurface`
        # has NO public `register_components()`/`register_component()`
        # method -- those names are dependency-injected onto actual
        # `ControlSurfaceComponent` instances (confirmed directly in
        # `_Framework/ControlSurface.py`'s own source: the base class
        # only defines a private `_register_component`, exposed to
        # components via `inject(...).everywhere()`), not something a
        # plain helper object like this one can call on the control
        # surface directly. Calling it raised an AttributeError right
        # here, silently aborting the rest of _connect() -- everything
        # BEFORE this line (the clip/scene color listeners above) kept
        # working, which is exactly why colors updated but no button
        # below this point ever got bound. The real, public API for
        # wiring a SessionComponent's session-ring overlay is
        # set_highlighting_session_component() -- confirmed both in
        # ControlSurface.py's own source and by Ableton's bundled
        # Launchpad.py, which calls this exact method on itself.
        self._control_surface.set_highlighting_session_component(self._session)

        # ---- TILES -> Ableton: plain CC, see this module's own docstring
        # for why this replaced a custom SysEx sub-protocol (never had
        # one confirmed successful delivery) and then a Note-On version
        # of this same migration (leaked through as playable/recordable
        # note content on any track with this port's Track input
        # enabled). Mirrors TILES.py's own transport-button setup
        # exactly (ButtonElement + add_value_listener), the one
        # reception mechanism with actual confirmed real-hardware
        # delivery on this project. ----
        for pad in range(1, NUM_GRID_PADS + 1):
            grid_button = ButtonElement(True, MIDI_CC_TYPE, TILES_MASTER_CHANNEL, CC_GRID_BASE + pad)
            grid_cb = self._make_grid_touch_callback(pad)
            grid_button.add_value_listener(grid_cb)
            self._grid_button_listeners.append((grid_button, grid_cb))

            stop_button = ButtonElement(True, MIDI_CC_TYPE, TILES_MASTER_CHANNEL, CC_STOP_BASE + pad)
            stop_cb = self._make_stop_touch_callback(pad)
            stop_button.add_value_listener(stop_cb)
            self._stop_button_listeners.append((stop_button, stop_cb))

        self._master_stop_button = ButtonElement(True, MIDI_CC_TYPE, TILES_MASTER_CHANNEL, CC_MASTER_STOP)
        self._master_stop_button.add_value_listener(self._on_master_stop)

        self._track_offset_button = ButtonElement(True, MIDI_CC_TYPE, TILES_MASTER_CHANNEL, CC_TRACK_OFFSET)
        self._track_offset_button.add_value_listener(self._on_track_offset_cc)

    def set_track_offset(self, offset):
        """The single source of truth for "which 5-track window is
        currently visible" -- updates both the session-ring overlay
        and the value _pad_to_col_track_scene() uses to translate an
        incoming grid-touch/stop pad number into a real track index.
        Called from _on_track_offset_cc() below whenever op_mode.c's
        own "-"/"+" changes it, and once on Scene Launch mode entry."""
        self._track_offset = offset
        if self._session is not None:
            self._session.set_offsets(offset, 0)

    def _on_has_clip_changed(self, track_index, scene_index, clip_slot):
        """Fired whenever a slot gains or loses a clip (also called once
        directly from _connect() to seed the initial state) -- only
        color needs re-subscribing here; playing_status/is_triggered stay
        registered on the ClipSlot itself for its whole lifetime (see
        _connect() above). Keyed by (track_index, scene_index) in a
        plain dict -- NOT by tagging an attribute onto the Clip object
        itself, which real feedback ("colors ar[e] not showing") traced
        back to: Ableton's own Clip objects aren't guaranteed to support
        arbitrary attribute assignment, and a raised AttributeError
        there would abort this whole object's __init__ (see that
        try/except's own comment)."""
        key = (track_index, scene_index)
        old = self._clip_color_listeners.pop(key, None)
        if old is not None:
            old_clip, old_color_cb = old
            try:
                old_clip.remove_color_listener(old_color_cb)
            except RuntimeError:
                pass  # old clip already gone -- nothing to remove

        if clip_slot.has_clip:
            clip = clip_slot.clip
            color_cb = self._make_slot_callback(track_index, scene_index, clip_slot)
            clip.add_color_listener(color_cb)
            self._clip_color_listeners[key] = (clip, color_cb)

        self._send_clip_state(track_index, scene_index, clip_slot)

    # ---- TILES -> Ableton: grid touch, stop, master stop, track offset -----

    def _pad_to_col_track_scene(self, pad):
        col, row = _pad_to_col_row(pad)
        track_index = self._track_offset + (col - 1)
        return col, track_index, row

    def _make_grid_touch_callback(self, pad):
        return lambda value: self._on_grid_touch(pad, value)

    def _make_stop_touch_callback(self, pad):
        return lambda value: self._on_stop_touch(pad, value)

    def _on_grid_touch(self, pad, value):
        # CC value 127 then immediately 0, same on/off pair convention
        # the transport CCs already use -- only the press (value > 0)
        # is a real action, the release that follows is just that
        # trigger's own tail end.
        if value <= 0:
            return
        col, track_index, scene_index = self._pad_to_col_track_scene(pad)
        self._log("grid_touch pad=%d col=%d track=%d scene=%d" % (pad, col, track_index, scene_index))
        scenes = self._song.scenes
        if col == 6:
            if scene_index < len(scenes):
                scenes[scene_index].fire()
        else:
            tracks = self._song.tracks
            if track_index < len(tracks) and scene_index < len(scenes):
                tracks[track_index].clip_slots[scene_index].fire()

    def _on_stop_touch(self, pad, value):
        if value <= 0:
            return
        col, track_index, scene_index = self._pad_to_col_track_scene(pad)
        if col == 6:
            # op_mode.c never actually sends this note for column 6 --
            # defensive only, there's no per-scene "stop" concept.
            return
        tracks = self._song.tracks
        scenes = self._song.scenes
        if track_index < len(tracks) and scene_index < len(scenes):
            clip_slot = tracks[track_index].clip_slots[scene_index]
            self._log("stop_touch pad=%d track=%d scene=%d has_clip=%d" % (pad, track_index, scene_index, clip_slot.has_clip))
            # Real feedback: "re pushing a playing clip pad all the way
            # down or close to that stops the individual clip."
            # Clip.stop() is the real per-clip stop (confirmed against
            # AbletonOSC's own clip.py, which wires the same method to
            # its own "/live/clip/stop" handler) -- distinct from
            # ClipSlot.fire(), which retriggers rather than stops an
            # already-playing clip.
            if clip_slot.has_clip:
                clip_slot.clip.stop()

    def _on_master_stop(self, value):
        if value <= 0:
            return
        # Real feedback: "a master stop in this app should be shift
        # diamond." Ableton's own real "stop all clips" action
        # (confirmed against _Framework/SessionComponent.py's own
        # self.song().stop_all_clips() call) -- stops every playing/
        # queued clip without touching the transport itself, distinct
        # from the diamond's plain-click transport Stop (see op_mode.c's
        # own handle_diamond_transport() for the firmware-side gating
        # that keeps this scoped to Scene Launch mode only).
        self._log("stop_all_clips")
        self._song.stop_all_clips()

    def _on_track_offset_cc(self, value):
        self._log("track_offset -> %d" % value)
        self.set_track_offset(value)

    def disconnect(self):
        if self._session is not None:
            try:
                self._control_surface.set_highlighting_session_component(None)
            except RuntimeError:
                pass
            try:
                self._session.disconnect()
            except RuntimeError:
                pass
        for button, callback in self._grid_button_listeners:
            try:
                button.remove_value_listener(callback)
            except RuntimeError:
                pass
        for button, callback in self._stop_button_listeners:
            try:
                button.remove_value_listener(callback)
            except RuntimeError:
                pass
        if self._master_stop_button is not None:
            try:
                self._master_stop_button.remove_value_listener(self._on_master_stop)
            except RuntimeError:
                pass
        if self._track_offset_button is not None:
            try:
                self._track_offset_button.remove_value_listener(self._on_track_offset_cc)
            except RuntimeError:
                pass
        for scene, is_triggered_cb, color_cb in self._scene_listeners:
            try:
                scene.remove_is_triggered_listener(is_triggered_cb)
                scene.remove_color_listener(color_cb)
            except RuntimeError:
                pass
        for clip_slot, has_clip_cb, playing_status_cb, is_triggered_cb in self._clip_slot_listeners:
            try:
                clip_slot.remove_has_clip_listener(has_clip_cb)
                clip_slot.remove_playing_status_listener(playing_status_cb)
                clip_slot.remove_is_triggered_listener(is_triggered_cb)
            except RuntimeError:
                pass
        for clip, color_cb in self._clip_color_listeners.values():
            try:
                clip.remove_color_listener(color_cb)
            except RuntimeError:
                pass
