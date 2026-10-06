"""
Live side of TILES's Ableton mode (Scene Launch). Hardware side: the
"Scene Launch (Ableton) mode" section of firmware/src/services/op_mode.c.
Wire format: shared/protocol/README.md, "Scene Launch".

Everything travels on TILES's DAW port, which this script uses alone
(__init__.py's get_capabilities()).

TILES -> Live: CCs on TILES_MASTER_CHANNEL, bound as ButtonElements with
add_value_listener(), the same mechanism as TILES.py's transport. Pad
events are sent only on a pressure click; a bare touch is haptics-only
on the hardware.

    CC_GRID_TOUCH   (108) value = pad 1-24, then 0: fire that clip
                    (columns 1-5) or launch that scene (column 6); an
                    empty slot does nothing
    CC_STOP_TOUCH   (109) value = pad, then 0: stop that playing clip
    CC_DELETE_TOUCH (110) value = pad, then 0: delete that clip (circle
                    held + pad for 3 s; the firmware times the hold)
    CC_RECORD_TOUCH (111) value = pad, then 0: record a new clip into that
                    empty slot (circle held + pressure click;
                    _record_new_clip())
    CC_MASTER_STOP  (105) 127 then 0: stop all clips
    CC_TRACK_OFFSET (106) value = first visible track (the "-"/"+" pan)
    CC_END_CAPTURE  (107) 127 then 0: end the recording that
                    _record_new_clip() started

Live -> TILES: SysEx, manufacturer ID 0x7D (development ID), sub-ID 0x01:

    F0 7D 01 10 <track> <scene> <flags> <r7> <g7> <b7> F7   clip state
    F0 7D 01 11 <scene> <flags> <r7> <g7> <b7>         F7   scene state
    F0 7D 01 12                                        F7   open melodic mode
        (a MIDI track was just armed for a new recording; the firmware
        switches once every pad is released)

    flags: bit 0 = has_clip, bit 1 = is_playing (clip state only), bit 2
    = is_triggered (both). r7/g7/b7: Live's 8-bit color channels halved
    to 0-127 (_color_to_wire_rgb()).

Pad -> slot: column = (pad-1) % 6 + 1, scene = (pad-1) // 6, track =
column - 1 + the last CC_TRACK_OFFSET, as in the firmware's
handle_scene_launch_taps(). Only the first MAX_TRACKS tracks and
NUM_SCENES scenes are tracked (the firmware's table size); scenes don't
page.

Keep these as they are; each was a bug found in Live (history in
daw-integration/HISTORY.md):
  - TILES -> Live is CCs, one per action with the pad as the value, all
    in the MIDI spec's undefined 102-119 range. A custom SysEx version
    never worked reliably; Note-On reached instrument tracks as playable
    notes; per-pad CCs claimed sustain (64), expression (11) and slide
    (74).
  - The session ring is wired with set_highlighting_session_component().
    ControlSurface has no public register_components().
  - Per-slot state lives in dicts owned here, never in attributes set on
    Live's Clip objects.
  - A slot's playing state is ClipSlot.playing_status (there is no
    is_playing listener).
  - An exception in _connect() is caught and logged, so Ableton mode
    failing never takes TILES.py's transport down with it.

The session ring (Ableton's box around the grid's window in Session
View) comes from a _Framework.SessionComponent with no button matrix
bound, since this script sends its own RGB feedback. Not yet confirmed
that Live draws the ring in that setup.
"""

from _Framework.ButtonElement import ButtonElement
from _Framework.InputControlElement import MIDI_CC_TYPE
from _Framework.SessionComponent import SessionComponent

# 0 = MIDI channel 1, as in TILES.py (not imported from there: TILES.py
# imports this module).
TILES_MASTER_CHANNEL = 0

# Must match OP_SCENE_CC_* in op_mode.c. One CC per action, the pad (1-24)
# as the value (_make_pad_event_callback()); see the module docstring.
CC_MASTER_STOP = 105
CC_TRACK_OFFSET = 106
CC_END_CAPTURE = 107
CC_GRID_TOUCH = 108
CC_STOP_TOUCH = 109
CC_DELETE_TOUCH = 110
CC_RECORD_TOUCH = 111

# Must match TILES_NUM_PADS (firmware/src/board/board_pins.h). Pad values
# outside 1..NUM_GRID_PADS are ignored.
NUM_GRID_PADS = 24

# Must match TILES_SYSEX_MANUFACTURER_ID in
# firmware/src/midi/product_identity.h (0x7D, the development ID, until
# SENTIA registers its own; a registered ID is 3 bytes, which changes the
# frame layout).
SYSEX_MFR_ID = 0x7D
SYSEX_SUB_ID = 0x01

MSG_CLIP_STATE = 0x10
MSG_SCENE_STATE = 0x11
# No payload -- tells the firmware to open melodic mode (once the pads
# are released) because a track was just armed for a new recording. See
# _record_new_clip().
MSG_OPEN_MELODIC = 0x12

FLAG_HAS_CLIP = 0x01
FLAG_IS_PLAYING = 0x02
FLAG_IS_TRIGGERED = 0x04

# Must match OP_SCENE_MAX_TRACKS / OP_SCENE_NUM_ROWS in op_mode.c.
MAX_TRACKS = 64
NUM_SCENES = 4

# Must match OP_SCENE_TRACK_COL_MAX - OP_SCENE_TRACK_COL_MIN + 1 in
# op_mode.c: the track columns shown at once. Sizes the session ring only;
# the clip/scene state covers every MAX_TRACKS track.
NUM_VISIBLE_TRACKS = 5



def _color_to_wire_rgb(color_int):
    """Live's packed 0xRRGGBB color -> three 7-bit bytes, each channel
    halved. The firmware doubles them back (scene_on_sysex()); the lost
    low bit doesn't matter for the LEDs."""
    r = (color_int >> 16) & 0xFF
    g = (color_int >> 8) & 0xFF
    b = color_int & 0xFF
    return r >> 1, g >> 1, b >> 1


def _pad_to_col_row(pad):
    """Pad 1-24 -> (column 1-6, row 0-3), as the firmware's
    handle_scene_launch_taps() maps it."""
    col = (pad - 1) % 6 + 1
    row = (pad - 1) // 6
    return col, row


def _try_remove(live_object, method_name, callback):
    """Removes one Live listener, ignoring failure. A deleted Live object
    can raise any of several types (a slot of a deleted track raises
    Boost.Python's ArgumentError, not RuntimeError), and one failed
    removal must not skip the others."""
    try:
        getattr(live_object, method_name)(callback)
    except Exception:  # noqa: BLE001 -- cleanup only
        pass


def _input_name(track):
    try:
        return track.input_routing_type.display_name
    except Exception:  # noqa: BLE001 -- not every track has a MIDI input
        return ""


def _played_by_other_tiles(track, other):
    """Whether `track` and `other` take their input from two different
    TILES units (each unit has its own port name, "SENTIA TILES 2 (MIDI)").
    Then disarming `other` would silence the other player, so it stays
    armed. Any other routing (All Ins, a keyboard) is disarmed."""
    mine, theirs = _input_name(track), _input_name(other)
    return "SENTIA TILES" in mine and "SENTIA TILES" in theirs and mine != theirs


class SceneLaunch(object):
    """Ableton mode's Live side. Owned by TILES (TILES.py); a separate
    object so the transport code doesn't depend on it.
    """

    def __init__(self, control_surface):
        self._control_surface = control_surface
        self._song = control_surface.song()
        # Listener callables are stored: Live's remove_*_listener needs
        # the exact callable that was added, not an equivalent one.
        self._clip_slot_listeners = []  # list of (clip_slot, has_clip_cb, playing_status_cb, is_triggered_cb)
        # Keyed by (track_index, scene_index) here, never by an attribute
        # set on Live's Clip (not guaranteed to be allowed).
        self._clip_color_listeners = {}  # {(track_index, scene_index): (clip, clip_cb)}, color + playing_status
        self._scene_listeners = []  # list of (scene, is_triggered_cb, color_cb)
        self._track_listeners = []  # list of (track, refresh_cb) -- see _refresh_track()
        self._session = None  # SessionComponent (the session ring), created in _connect()
        self._grid_button_listeners = []  # list of (button, callback), pad 1..NUM_GRID_PADS
        self._stop_button_listeners = []  # list of (button, callback), pad 1..NUM_GRID_PADS
        self._master_stop_button = None
        self._track_offset_button = None
        self._end_capture_button = None
        self._delete_button_listeners = []  # list of (button, callback), pad 1..NUM_GRID_PADS
        self._record_button_listeners = []  # list of (button, callback), pad 1..NUM_GRID_PADS
        # The slot _record_new_clip() last armed and started recording
        # into -- what CC_END_CAPTURE ends. None when nothing's recording.
        self._capture_slot = None
        # Mirrors the firmware's s_scene_track_offset: pad CCs carry only
        # a pad number.
        self._track_offset = 0
        try:
            self._connect()
            self._log("connected")
        except Exception as e:  # noqa: BLE001 -- see below
            # Catch everything: an exception here would propagate through
            # TILES.__init__()'s component_guard() and take the whole
            # script, transport included, down. Logged to Live's Log.txt.
            self._log("failed to connect: %s" % e)

    def _log(self, message):
        # Writes to Live's Log.txt (Help -> Show Log), the only place a
        # Remote Script's output shows. One line per action, while this is
        # still being confirmed in real sessions.
        self._control_surface.log_message("[TILES scene_launch] " + message)

    # ---- Live -> TILES (SysEx) ----------------------------------------------

    def _send_clip_state(self, track_index, scene_index, clip_slot):
        # Playing/triggered come from the Clip when there is one, else the
        # slot, as Live's own ClipSlotComponent does.
        has_clip = clip_slot.has_clip
        clip = clip_slot.clip if has_clip else None
        is_playing = bool(clip is not None and clip.is_playing)
        is_triggered = bool(clip.is_triggered if clip is not None else clip_slot.is_triggered)
        color = clip.color if clip is not None else 0
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

    def _make_has_clip_callback(self, track_index, scene_index, clip_slot):
        # has_clip also re-subscribes the Clip-level listeners (a clip was
        # just recorded into or deleted from this slot).
        return lambda: self._on_has_clip_changed(track_index, scene_index, clip_slot)

    def _refresh_track(self, track_index):
        """Re-sends every tracked slot on one track. Launching clip B stops
        clip A as a side effect, and A's own listeners aren't guaranteed to
        fire, which left A showing as playing. A track's playing/fired slot
        index always changes with its playing clip (Live's SessionComponent
        listens to the same two), so refreshing the whole track on those
        catches it."""
        tracks = self._song.tracks
        if track_index >= len(tracks):
            return
        slots = tracks[track_index].clip_slots
        for scene_index in range(min(len(slots), NUM_SCENES)):
            self._send_clip_state(track_index, scene_index, slots[scene_index])

    def _make_track_refresh_callback(self, track_index):
        return lambda: self._refresh_track(track_index)

    def _connect_track_clip_listeners(self):
        """Adds the has_clip/playing_status/is_triggered listeners for every
        clip slot and the playing/fired slot index listeners for every
        track, up to MAX_TRACKS x NUM_SCENES. Appends; call
        _disconnect_track_clip_listeners() first for a clean slate."""
        tracks = self._song.tracks
        num_tracks = min(len(tracks), MAX_TRACKS)
        num_scenes = min(len(self._song.scenes), NUM_SCENES)
        for track_index in range(num_tracks):
            track = tracks[track_index]
            for scene_index in range(num_scenes):
                clip_slot = track.clip_slots[scene_index]
                # On the ClipSlot, which is stable for its (track, scene)
                # position, so these never need re-registering; color is
                # per Clip (_on_has_clip_changed()). The slot has
                # playing_status, not an is_playing listener (as in Live's
                # _Framework/ClipSlotComponent.py).
                has_clip_cb = self._make_has_clip_callback(track_index, scene_index, clip_slot)
                playing_status_cb = self._make_slot_callback(track_index, scene_index, clip_slot)
                is_triggered_cb = self._make_slot_callback(track_index, scene_index, clip_slot)
                clip_slot.add_has_clip_listener(has_clip_cb)
                clip_slot.add_playing_status_listener(playing_status_cb)
                clip_slot.add_is_triggered_listener(is_triggered_cb)
                self._clip_slot_listeners.append((clip_slot, has_clip_cb, playing_status_cb, is_triggered_cb))
                self._on_has_clip_changed(track_index, scene_index, clip_slot)

            # playing_slot_index/fired_slot_index -- see _refresh_track().
            refresh_cb = self._make_track_refresh_callback(track_index)
            track.add_playing_slot_index_listener(refresh_cb)
            track.add_fired_slot_index_listener(refresh_cb)
            self._track_listeners.append((track, refresh_cb))

    def _disconnect_track_clip_listeners(self):
        """Removes everything _connect_track_clip_listeners() and
        _on_has_clip_changed() added and empties their lists. Also used by
        disconnect()."""
        for clip_slot, has_clip_cb, playing_status_cb, is_triggered_cb in self._clip_slot_listeners:
            _try_remove(clip_slot, "remove_has_clip_listener", has_clip_cb)
            _try_remove(clip_slot, "remove_playing_status_listener", playing_status_cb)
            _try_remove(clip_slot, "remove_is_triggered_listener", is_triggered_cb)
        self._clip_slot_listeners = []
        for track, refresh_cb in self._track_listeners:
            _try_remove(track, "remove_playing_slot_index_listener", refresh_cb)
            _try_remove(track, "remove_fired_slot_index_listener", refresh_cb)
        self._track_listeners = []
        for clip, clip_cb in self._clip_color_listeners.values():
            self._remove_clip_listeners(clip, clip_cb)
        self._clip_color_listeners = {}

    def _on_tracks_changed(self):
        """Song tracks listener: a track was added, removed or reordered.
        Indices can all shift, so every per-track and per-slot listener is
        rebuilt rather than diffed. Without this, a track created after
        the script loaded never reported its clips and its pads stayed
        dark."""
        self._log("tracks changed -- resyncing clip-slot listeners")
        self._disconnect_track_clip_listeners()
        self._connect_track_clip_listeners()

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

        self._connect_track_clip_listeners()
        # Tracks created later: _on_tracks_changed().
        self._song.add_tracks_listener(self._on_tracks_changed)

        # Ableton's session-ring box in Session View, sized to the
        # hardware's 5 x 4 window and moved by set_track_offset(). No
        # button matrix is bound: this script sends its own RGB feedback.
        self._session = SessionComponent(NUM_VISIBLE_TRACKS, num_scenes)
        self._session.set_offsets(0, 0)
        # The public API for the ring, as Live's bundled Launchpad.py uses
        # it. (ControlSurface has no public register_components(); calling
        # it raised here and left every button below unbound.)
        self._control_surface.set_highlighting_session_component(self._session)

        # ---- TILES -> Live: one CC per action, the pad as the value,
        # bound like TILES.py's transport (ButtonElement +
        # add_value_listener). ----
        for cc, handler, listeners in (
            (CC_GRID_TOUCH, self._on_grid_touch, self._grid_button_listeners),
            (CC_STOP_TOUCH, self._on_stop_touch, self._stop_button_listeners),
            (CC_DELETE_TOUCH, self._on_delete_touch, self._delete_button_listeners),
            (CC_RECORD_TOUCH, self._on_record_touch, self._record_button_listeners),
        ):
            button = ButtonElement(True, MIDI_CC_TYPE, TILES_MASTER_CHANNEL, cc)
            callback = self._make_pad_event_callback(handler)
            button.add_value_listener(callback)
            listeners.append((button, callback))

        self._master_stop_button = ButtonElement(True, MIDI_CC_TYPE, TILES_MASTER_CHANNEL, CC_MASTER_STOP)
        self._master_stop_button.add_value_listener(self._on_master_stop)

        self._track_offset_button = ButtonElement(True, MIDI_CC_TYPE, TILES_MASTER_CHANNEL, CC_TRACK_OFFSET)
        self._track_offset_button.add_value_listener(self._on_track_offset_cc)

        self._end_capture_button = ButtonElement(True, MIDI_CC_TYPE, TILES_MASTER_CHANNEL, CC_END_CAPTURE)
        self._end_capture_button.add_value_listener(self._on_end_capture)

    def set_track_offset(self, offset):
        """Sets which five-track window is visible: moves the session ring
        and the offset _pad_to_col_track_scene() applies. The firmware
        sends CC_TRACK_OFFSET on entering Ableton mode and on every
        "-"/"+"."""
        self._track_offset = offset
        if self._session is not None:
            self._session.set_offsets(offset, 0)

    def _on_has_clip_changed(self, track_index, scene_index, clip_slot):
        """A slot gained or lost a clip (also called once per slot at
        connect). Re-subscribes the Clip's color and playing_status
        listeners; the slot-level listeners stay registered for the slot's
        lifetime."""
        key = (track_index, scene_index)
        old = self._clip_color_listeners.pop(key, None)
        if old is not None:
            self._remove_clip_listeners(*old)

        if clip_slot.has_clip:
            clip = clip_slot.clip
            # The Clip's playing_status as well as the slot's (Live's
            # ClipSlotComponent listens to both); with _refresh_track(),
            # this keeps a stopped clip from showing as playing. One
            # callable for both is fine: each listener list is separate.
            clip_cb = self._make_slot_callback(track_index, scene_index, clip_slot)
            clip.add_color_listener(clip_cb)
            clip.add_playing_status_listener(clip_cb)
            self._clip_color_listeners[key] = (clip, clip_cb)

        self._send_clip_state(track_index, scene_index, clip_slot)

    @staticmethod
    def _remove_clip_listeners(clip, clip_cb):
        _try_remove(clip, "remove_color_listener", clip_cb)
        _try_remove(clip, "remove_playing_status_listener", clip_cb)

    # ---- TILES -> Live: pad clicks, master stop, track offset, end capture --

    def _pad_to_col_track_scene(self, pad):
        col, row = _pad_to_col_row(pad)
        track_index = self._track_offset + (col - 1)
        return col, track_index, row

    def _make_pad_event_callback(self, handler):
        """Adapts a pad CC (value = pad 1..NUM_GRID_PADS, then 0) to
        handler(pad, 127). The trailing 0 and out-of-range values are
        ignored."""
        def callback(value):
            if 1 <= value <= NUM_GRID_PADS:
                handler(value, 127)
        return callback

    def _on_grid_touch(self, pad, value):
        # Only the press acts. Sent on a pressure click only (a touch is
        # haptics-only on the hardware).
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
                track = tracks[track_index]
                clip_slot = track.clip_slots[scene_index]
                if clip_slot.has_clip:
                    clip_slot.fire()

    def _on_record_touch(self, pad, value):
        """Circle held + a pressure click on an empty slot: record a new
        clip there. Checked against Live's own state, so a slot that has
        a clip by now (the firmware's copy can lag) is left alone."""
        if value <= 0:
            return
        col, track_index, scene_index = self._pad_to_col_track_scene(pad)
        if col == 6:
            return
        tracks = self._song.tracks
        scenes = self._song.scenes
        if track_index < len(tracks) and scene_index < len(scenes):
            track = tracks[track_index]
            clip_slot = track.clip_slots[scene_index]
            self._log("record_touch pad=%d track=%d scene=%d has_clip=%d" % (pad, track_index, scene_index, clip_slot.has_clip))
            if not clip_slot.has_clip:
                self._record_new_clip(track, clip_slot, track_index, scene_index)

    def _record_new_clip(self, track, clip_slot, track_index, scene_index):
        """Circle + a click on an empty slot. Disarms the other armed
        tracks (Live's Exclusive Arm preference can't be seen or relied
        on) except one played by a different TILES
        (_played_by_other_tiles()), arms this one if it can be armed
        (group tracks can't), and fires the slot, which records a new
        clip into it. If the track takes MIDI, tells the firmware to open
        melodic mode so the player can play into it; audio tracks still
        arm and record. (can_be_armed, arm and has_midi_input as in
        AbletonOSC's track.py.)"""
        armed = False
        if track.can_be_armed:
            for other in self._song.tracks:
                if (
                    other is not track
                    and other.can_be_armed
                    and other.arm
                    and not _played_by_other_tiles(track, other)
                ):
                    other.arm = False
            track.arm = True
            armed = True
        self._log(
            "record_new_clip track=%d scene=%d armed=%d midi_input=%d"
            % (track_index, scene_index, armed, track.has_midi_input)
        )
        clip_slot.fire()
        self._capture_slot = clip_slot
        if armed and track.has_midi_input:
            self._control_surface._send_midi((0xF0, SYSEX_MFR_ID, SYSEX_SUB_ID, MSG_OPEN_MELODIC, 0xF7))

    def _on_stop_touch(self, pad, value):
        if value <= 0:
            return
        col, track_index, scene_index = self._pad_to_col_track_scene(pad)
        if col == 6:
            # The firmware never sends this for column 6.
            return
        tracks = self._song.tracks
        scenes = self._song.scenes
        if track_index < len(tracks) and scene_index < len(scenes):
            clip_slot = tracks[track_index].clip_slots[scene_index]
            self._log("stop_touch pad=%d track=%d scene=%d has_clip=%d" % (pad, track_index, scene_index, clip_slot.has_clip))
            # The firmware picks fire vs. stop from the state Live last
            # reported, so one click toggles. Clip.stop() stops just this
            # clip (ClipSlot.fire() would retrigger it); as in AbletonOSC's
            # clip.py.
            if clip_slot.has_clip:
                clip_slot.clip.stop()

    def _on_master_stop(self, value):
        if value <= 0:
            return
        # Live's stop-all-clips (as _Framework/SessionComponent.py calls
        # it): stops playing and queued clips, leaves the transport
        # running. The firmware sends this only in Ableton mode.
        self._log("stop_all_clips")
        self._song.stop_all_clips()

    def _on_delete_touch(self, pad, value):
        """Circle held + pad for 3 s (the firmware times the hold). Guarded
        by has_clip like Live's ClipSlotComponent._do_delete_clip(). Live's
        undo covers a mistake; the has_clip listener then clears the pad."""
        if value <= 0:
            return
        col, track_index, scene_index = self._pad_to_col_track_scene(pad)
        if col == 6:
            return
        tracks = self._song.tracks
        scenes = self._song.scenes
        if track_index >= len(tracks) or scene_index >= len(scenes):
            return
        clip_slot = tracks[track_index].clip_slots[scene_index]
        self._log("delete_clip pad=%d track=%d scene=%d has_clip=%d" % (pad, track_index, scene_index, clip_slot.has_clip))
        if clip_slot.has_clip:
            if clip_slot is self._capture_slot:
                self._capture_slot = None
            clip_slot.delete_clip()

    def _on_end_capture(self, value):
        """Circle + diamond during the melodic-mode capture that
        _record_new_clip() opened. Fires the same slot again, which ends
        the recording and starts the clip looping (Live's own launch-button
        behavior). Doesn't disarm the track. The firmware returns to
        Ableton mode by itself. (Clip.is_recording as in AbletonOSC's
        clip.py.)"""
        if value <= 0:
            return
        slot = self._capture_slot
        self._capture_slot = None
        if slot is None:
            self._log("end_capture: nothing was being captured")
            return
        try:
            if slot.has_clip and slot.clip.is_recording:
                self._log("end_capture: ending recording")
                slot.fire()
            else:
                self._log("end_capture: slot is no longer recording")
        except RuntimeError as e:
            self._log("end_capture: slot is gone (%s)" % e)

    def _on_track_offset_cc(self, value):
        self._log("track_offset -> %d" % value)
        self.set_track_offset(value)

    def disconnect(self):
        try:
            self._song.remove_tracks_listener(self._on_tracks_changed)
        except RuntimeError:
            pass
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
        if self._end_capture_button is not None:
            try:
                self._end_capture_button.remove_value_listener(self._on_end_capture)
            except RuntimeError:
                pass
        for button, callback in self._delete_button_listeners + self._record_button_listeners:
            try:
                button.remove_value_listener(callback)
            except RuntimeError:
                pass
        for scene, is_triggered_cb, color_cb in self._scene_listeners:
            try:
                scene.remove_is_triggered_listener(is_triggered_cb)
                scene.remove_color_listener(color_cb)
            except RuntimeError:
                pass
        self._disconnect_track_clip_listeners()
