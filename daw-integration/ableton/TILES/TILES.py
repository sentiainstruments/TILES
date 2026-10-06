"""
Ableton Live Control Surface script for SENTIA TILES.

Transport: listens on TILES's DAW port, channel 1, for the three CCs the
diamond sends (handle_diamond_transport() in
firmware/src/services/op_mode.c): 102 Play, 103 Stop, 104 Record. Each
calls Live's transport API directly, the way Ableton's bundled Launchkey
script handles its transport buttons, so nothing needs MIDI mapping.
Each trigger arrives as 127 then 0; only the 127 acts.

The DAW port is used by this script alone (__init__.py's
get_capabilities(), firmware/src/midi/midi_ports.h), so these CCs never
reach a track.

Ableton mode (Scene Launch) is scene_launch.py, owned by this class.
"""

from _Framework.ControlSurface import ControlSurface
from _Framework.ButtonElement import ButtonElement
from _Framework.InputControlElement import MIDI_CC_TYPE

from .scene_launch import SceneLaunch

# Must match TILES_MIDI_MPE_MASTER_CHANNEL (firmware/src/midi/midi_out.h;
# 0 = MIDI channel 1) and OP_TRANSPORT_PLAY_CC/_STOP_CC/_RECORD_CC in
# firmware/src/services/op_mode.c.
TILES_MASTER_CHANNEL = 0
PLAY_CC = 102
STOP_CC = 103
RECORD_CC = 104

# Matches OP_TRANSPORT_SHIFT_STOP in the firmware's op_mode.c (the
# performance layout: diamond plays, hold records, circle + diamond stops).
# While Live plays, Play and Record do nothing: no restart from the start
# marker, no punch-in. Checked against Live's own state, so it holds when
# another TILES or the mouse started the transport. Play is ignored while
# playing either way (a restart is never what a Play button means).
DIAMOND_IGNORED_WHILE_PLAYING = True


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
            # Ableton mode. SceneLaunch catches its own setup errors, so
            # the transport above keeps working if it fails.
            self._scene_launch = SceneLaunch(self)

    def _on_play(self, value):
        if value > 0 and not self.song().is_playing:
            self.song().start_playing()

    def _on_stop(self, value):
        if value > 0:
            # Stop also ends a recording: record_mode doesn't clear by
            # itself when playback stops.
            self.song().record_mode = False
            self.song().stop_playing()

    def _on_record(self, value):
        if value > 0:
            if DIAMOND_IGNORED_WHILE_PLAYING and self.song().is_playing:
                return
            # Set, not toggled: the firmware sends Record only to start a
            # recording (Stop ends one). Live's count-in preference
            # (Record/Warp/Launch) applies by itself.
            self.song().record_mode = True

    def disconnect(self):
        self._play_button.remove_value_listener(self._on_play)
        self._stop_button.remove_value_listener(self._on_stop)
        self._record_button.remove_value_listener(self._on_record)
        self._scene_launch.disconnect()
        super(TILES, self).disconnect()
