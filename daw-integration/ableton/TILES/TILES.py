"""
Ableton Live Control Surface script for SENTIA TILES's transport remote
(diamond button -- see firmware/src/services/op_mode.c's own
handle_diamond_transport()).

Real feedback, in order: "the diamond for now will play and stop in
ableton like a toggle... if we hold it for 2 sec it arms record;" then,
once plain MIDI Start/Stop bytes turned out not to do anything without a
full Sync/Ext external-clock setup: "look online for how other things do
that like the novation lounch key" -- Launchkey's own transport buttons
send plain MIDI CCs that Ableton recognizes because Ableton BUNDLES a
Launchkey-specific Control Surface script that translates those CCs into
direct Live API calls; then, once told that TILES sending the identical
kind of CC still needed a one-time manual MIDI-Map step per button: "i
dont want to map manually, this should just work like it does for
launchkey out of the box."

Honest limitation this script cannot get around: Ableton only auto-loads
a BUNDLED script for controllers it ships support for -- there is no way
for third-party/DIY hardware to make Ableton auto-select an unbundled
script with literally zero user action, any more than a brand-new
Launchkey model works before Ableton has been updated to include it.
What THIS script gets to, which is the closest real equivalent: ONE
manual step, done ONCE ever (copy this folder in, pick "TILES" from a
dropdown) -- not per-button MIDI Learn, and not something that needs
repeating after restarts, DAW updates, or new projects. See
daw-integration/README.md for that one-time install step.

Once installed and selected, this script listens on TILES's MPE Zone
Master Channel (MIDI channel 1) for the three momentary CC triggers
op_mode.c's handle_diamond_transport() already sends -- CC 102 (Play),
103 (Stop), 104 (Record) -- and calls Live's own transport API directly,
the same kind of translation Ableton's bundled Launchkey script does
internally for ITS transport buttons. No MIDI Map Mode involved at all.

Each trigger arrives as TWO CC messages back to back (value 127, then
immediately 0 -- see OP_TRANSPORT_PLAY_CC's own comment in op_mode.c for
why: a clean on/off pair, not a value left dangling at 127). Only the
value > 0 message should act; the 0 that follows is just that trigger's
own release and must be ignored, not treated as a second event.

Also owns Scene Launch mode's own Ableton-side half (real feedback:
"lets implemebt a new mode that triggers scenes in ableton live...
can we pull the colors of the scenes from ableton?") -- see
scene_launch.py's own module docstring for that protocol and this
class's disconnect() for how it's wired in here. Scene Launch's own
TILES -> Ableton messages (fire/launch/stop-all/stop-clip) arrive as
plain Note-On/CC on this same MPE Zone Master Channel, exactly like
the three transport CCs above -- SceneLaunch binds its own
ButtonElements directly (see that file's own _connect()) rather than
this class needing a handle_sysex() override the way an earlier,
custom-SysEx version of that protocol once did (see scene_launch.py's
own module docstring for why that was replaced).
"""

from _Framework.ControlSurface import ControlSurface
from _Framework.ButtonElement import ButtonElement
from _Framework.InputControlElement import MIDI_CC_TYPE

from .scene_launch import SceneLaunch

# Wire values, matching firmware/src/midi/midi_out.h's own
# TILES_MIDI_MPE_MASTER_CHANNEL (0 = MIDI channel 1, the status-byte
# nibble) and firmware/src/services/op_mode.c's OP_TRANSPORT_PLAY_CC/
# _STOP_CC/_RECORD_CC. Keep these three in sync with that file if they
# ever change there.
TILES_MASTER_CHANNEL = 0
PLAY_CC = 102
STOP_CC = 103
RECORD_CC = 104


class TILES(ControlSurface):
    def __init__(self, c_instance):
        super(TILES, self).__init__(c_instance)
        with self.component_guard():
            self._play_button = ButtonElement(True, MIDI_CC_TYPE, TILES_MASTER_CHANNEL, PLAY_CC)
            self._stop_button = ButtonElement(True, MIDI_CC_TYPE, TILES_MASTER_CHANNEL, STOP_CC)
            self._record_button = ButtonElement(True, MIDI_CC_TYPE, TILES_MASTER_CHANNEL, RECORD_CC)
            self._play_button.add_value_listener(self._on_play)
            self._stop_button.add_value_listener(self._on_stop)
            self._record_button.add_value_listener(self._on_record)
            # Real feedback: "lets implemebt a new mode that triggers
            # scenes in ableton live... can we pull the colors of the
            # scenes from ableton?" -- see scene_launch.py's own module
            # docstring for the full protocol and this addition's
            # confidence level (newer/less-verified Live API surface
            # than the transport buttons above).
            self._scene_launch = SceneLaunch(self)

    def _on_play(self, value):
        if value > 0:
            self.song().start_playing()

    def _on_stop(self, value):
        if value > 0:
            # Real Ableton transport behavior: hitting Stop while
            # recording stops the recording too, not just playback --
            # record_mode doesn't clear on its own just because playback
            # did, so it's set explicitly here rather than assumed.
            self.song().record_mode = False
            self.song().stop_playing()

    def _on_record(self, value):
        if value > 0:
            # Absolute set, not a toggle -- op_mode.c only ever sends
            # this CC when the hardware is ARMING a new recording (see
            # handle_diamond_transport()'s own comment), never to mean
            # "stop recording" -- that's the Stop CC/button above
            # instead. Matches Ableton's own Count-In preference
            # (Preferences -> Record/Warp/Launch), which fires
            # automatically on record_mode becoming True with no
            # further action needed here.
            self.song().record_mode = True

    def disconnect(self):
        self._play_button.remove_value_listener(self._on_play)
        self._stop_button.remove_value_listener(self._on_stop)
        self._record_button.remove_value_listener(self._on_record)
        self._scene_launch.disconnect()
        super(TILES, self).disconnect()
