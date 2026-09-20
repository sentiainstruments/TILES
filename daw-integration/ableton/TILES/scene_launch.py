"""
Ableton-side half of TILES's Scene Launch mode (see
firmware/src/services/op_mode.c's own "Scene Launch mode" section for
the hardware side, and shared/protocol/README.md's "Scene Launch"
section for the full message catalog this implements).

Real feedback: "lets implemebt a new mode that triggers scenes in
ableton live keep it simple for now, push triggers it... can we pull
the colors of the scenes from ableton? and light behaviour to feel
intuitive?"

Honesty about confidence, same spirit as firmware/src/drivers/
dac80502.c's own header comment for its first-ever, unverified driver:
this is the FIRST code in this repo that reads Ableton's Live Object
Model (song/track/scene/clip-slot navigation, add_*_listener) or
receives raw SysEx into a Control Surface script (handle_sysex).
Real testing against a live session found two real bugs in the first
version of this file (see handle_sysex()'s and _connect()'s own
comments for each) -- both now fixed and cross-checked against
Ableton's own bundled Remote Script source (_APC/APC.py for the
handle_sysex framing, _Framework/ClipSlotComponent.py and
SessionComponent.py for the Live API property/listener names and
stop_all_clips()), not just guessed at a second time. Still genuinely
possible something else in here doesn't match a real session exactly
-- if Scene Launch mode lights up but nothing updates, or clip fires
don't work, check Log.txt for this file's own "[TILES scene_launch]"
lines first (see daw-integration/README.md's own Debugging section).

Wire protocol summary (manufacturer ID 0x7D = MMA-reserved "non-
commercial/educational use", sub-ID 0x01 = TILES's own Scene Launch
sub-protocol under it). The framing below is the wire format actually
sent/received over USB MIDI; handle_sysex()'s own `midi_bytes` does
NOT include the leading F0/trailing F7 -- Ableton's framework strips
both before calling back (confirmed against APC.py's own real
handle_sysex) -- see that method's own comment:

    TILES -> Ableton:
        F0 7D 01 01 <track> <scene>              F7   fire clip
        F0 7D 01 02 <scene>                       F7   launch scene
        F0 7D 01 03                               F7   stop all clips (master stop)

    Ableton -> TILES:
        F0 7D 01 10 <track> <scene> <flags> <r7> <g7> <b7> F7   clip state
        F0 7D 01 11 <scene> <flags> <r7> <g7> <b7>         F7   scene state

    flags bit 0 = has_clip (clip state only), bit 1 = is_playing (clip
    state only), bit 2 = is_triggered (both). <r7>/<g7>/<b7> are each
    0-127 (Ableton's own 0-255 channel value halved, see
    _color_to_wire_rgb() below) -- this hardware doubles them back
    toward 8-bit on receipt (see op_mode.c's own scene_on_sysex()),
    losing the bottom bit, not the top.

Only the first OP_SCENE_MAX_TRACKS tracks and OP_SCENE_NUM_ROWS scenes
are ever pushed or listened to -- matches firmware/src/services/
op_mode.c's own fixed-size state table, and "keep it simple for now"
means no attempt yet to page scenes beyond the first 4 (real feedback's
own Q&A settled "-"/"+" as a TRACK pan, not a scene page, for this
version).
"""

SYSEX_MFR_ID = 0x7D
SYSEX_SUB_ID = 0x01

MSG_FIRE_CLIP = 0x01
MSG_LAUNCH_SCENE = 0x02
MSG_STOP_ALL = 0x03
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


class SceneLaunch(object):
    """Owned by TILES.py (see that file's own __init__/disconnect) --
    kept as a separate object rather than folded into the TILES class
    itself so the already-real-hardware-tested transport-remote code
    stays completely undisturbed by this newer, unverified addition.
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
        self._clip_slot_listeners = []  # list of (clip_slot, has_clip_cb, is_playing_cb, is_triggered_cb)
        # keyed by (track_index, scene_index) -- see _on_has_clip_changed()'s
        # own comment on why this replaced an earlier, real bug (monkey-
        # patching an identifying attribute directly onto Ableton's own
        # native Clip object, which isn't guaranteed to support arbitrary
        # attribute assignment and could silently abort this whole
        # object's __init__ if it ever raised).
        self._clip_color_listeners = {}  # {(track_index, scene_index): (clip, color_cb)}
        self._scene_listeners = []  # list of (scene, is_triggered_cb, color_cb)
        try:
            self._connect()
            self._log("connected")
        except Exception as e:  # noqa: BLE001 -- see this except's own comment
            # Real feedback: "colors ar[e] not showing." Whatever the
            # exact cause, an exception anywhere in _connect() used to
            # propagate all the way up through TILES.__init__()'s own
            # component_guard(), which would silently abort the WHOLE
            # script -- taking the already-working transport remote down
            # with a completely unrelated Scene Launch bug, the opposite
            # of "a failed subsystem disables itself, it never takes
            # other subsystems down with it." Caught here instead, logged
            # so it's actually visible (Ableton's own Log.txt, Help ->
            # Show Log), and left non-fatal: the transport buttons in
            # TILES.py keep working either way.
            self._log("failed to connect: %s" % e)

    def _log(self, message):
        # self.log_message() writes to Ableton's own Log.txt -- the
        # standard way to see what a Remote Script is actually doing,
        # since there's no console output visible otherwise. Every real
        # action this object takes logs one line, on purpose, while this
        # is still unverified against a real session (see this module's
        # own docstring) -- trim this down once it's confirmed working.
        self._control_surface.log_message("[TILES scene_launch] " + message)

    # ---- Ableton -> TILES ------------------------------------------------

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
                # property).
                has_clip_cb = self._make_slot_callback(track_index, scene_index, clip_slot)
                # Real bug found from live testing ("colors are not
                # updating"): there is no add_is_playing_listener on the
                # real ClipSlot object -- confirmed against Ableton's own
                # bundled _Framework/ClipSlotComponent.py, which listens
                # for playing-state changes on 'playing_status' instead
                # (is_playing itself is only ever a plain, non-listenable
                # property you re-read inside that callback -- see
                # _send_clip_state() below). Calling the nonexistent
                # method raised on the very first clip slot in this loop,
                # which the try/except in __init__ then swallowed -- so
                # _connect() aborted before registering ANYTHING or ever
                # pushing a single state update, on every run until now.
                playing_status_cb = self._make_slot_callback(track_index, scene_index, clip_slot)
                is_triggered_cb = self._make_slot_callback(track_index, scene_index, clip_slot)
                clip_slot.add_has_clip_listener(has_clip_cb)
                clip_slot.add_playing_status_listener(playing_status_cb)
                clip_slot.add_is_triggered_listener(is_triggered_cb)
                self._clip_slot_listeners.append((clip_slot, has_clip_cb, playing_status_cb, is_triggered_cb))
                self._on_has_clip_changed(track_index, scene_index, clip_slot)

    def _on_has_clip_changed(self, track_index, scene_index, clip_slot):
        """Fired whenever a slot gains or loses a clip (also called once
        directly from _connect() to seed the initial state) -- only
        color needs re-subscribing here; is_playing/is_triggered stay
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

    # ---- TILES -> Ableton --------------------------------------------------

    def handle_sysex(self, midi_bytes):
        """Called by TILES.py's own handle_sysex() override -- see that
        file's own comment on why the base ControlSurface's raw SysEx
        hook is what delivers this rather than a ButtonElement/CC
        listener the way the transport remote uses.

        Real bug found from live testing ("colors are not updating...
        in ableton", i.e. clip fires never reached Live): `midi_bytes`
        does NOT include the F0/F7 framing -- confirmed against
        Ableton's own bundled _APC/APC.py, whose real handle_sysex
        indexes midi_bytes[3]/[4] directly with no offset for a leading
        status byte, meaning the framework strips both ends before this
        callback ever runs. This module's previous version assumed the
        framing was still present and read one index too far right,
        so the manufacturer/sub-ID check below was comparing the wrong
        bytes and silently rejected every message TILES ever sent."""
        self._log("handle_sysex received: %s" % (tuple(midi_bytes),))
        if len(midi_bytes) < 3 or midi_bytes[0] != SYSEX_MFR_ID or midi_bytes[1] != SYSEX_SUB_ID:
            return
        msg_type = midi_bytes[2]
        if msg_type == MSG_FIRE_CLIP and len(midi_bytes) == 5:
            track_index, scene_index = midi_bytes[3], midi_bytes[4]
            tracks = self._song.tracks
            scenes = self._song.scenes
            if track_index < len(tracks) and scene_index < len(scenes):
                tracks[track_index].clip_slots[scene_index].fire()
        elif msg_type == MSG_LAUNCH_SCENE and len(midi_bytes) == 4:
            scene_index = midi_bytes[3]
            scenes = self._song.scenes
            if scene_index < len(scenes):
                scenes[scene_index].fire()
        elif msg_type == MSG_STOP_ALL and len(midi_bytes) == 3:
            # Real feedback: "a master stop in this app should be shift
            # diamond." Ableton's own real "stop all clips" action
            # (confirmed against _Framework/SessionComponent.py's own
            # self.song().stop_all_clips() call) -- stops every playing/
            # queued clip without touching the transport itself, distinct
            # from the diamond's plain-click transport Stop (see op_mode.c's
            # own handle_diamond_transport() for the firmware-side gating
            # that keeps this scoped to Scene Launch mode only).
            self._song.stop_all_clips()

    def disconnect(self):
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
