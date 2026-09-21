# protocol/

Real feedback: "keep cv gate implemented but off rn. we need the control
software." This is the **first, deliberately narrow version** of the USB
vendor protocol -- a plain-text settings GET/SET, covering only the
runtime toggles the firmware already built with a companion-app hook in
mind. It is **not** the fuller protocol `docs/protocol/README.md`'s own
design notes describe (pad remap, guided per-pad calibration, live 24-pad
XYZ streaming at ~120Hz, profile read/write, firmware update) -- those
are real, still open, and this version deliberately doesn't try to
answer their harder questions (binary framing, sequence IDs/streaming,
versioning, schema-based encoding). Scoped and confirmed this way on
purpose: prove the USB vendor interface and a real settings round-trip
work at all, over the simplest protocol that could possibly work, before
investing in the bigger design.

## Transport

A dedicated USB vendor-class interface (VID `0x2E8A`, PID `0x100A`,
interface string "SENTIA TILES Control"), separate from both the CDC
diagnostics console and the MIDI interface -- implemented in
`../../firmware/src/usb_vendor/usb_vendor.c`. Full-speed bulk endpoints,
64-byte packets.

## Wire format

Plain ASCII text, one command per line, newline-terminated (`\n`; `\r`
is also accepted and treated the same as `\n`, so a naive line-based
terminal on either line-ending convention works unmodified):

```
GET <key>\n
SET <key> <value>\n
LIST\n
```

Responses, one line each:

- `GET`: the current value as text, or `ERR unknown-key`.
- `SET`: `OK`, or `ERR unknown-key` / `ERR bad-value`.
- `LIST`: one `<key>=<value>` line per known key, then a final `OK`.

No sequence IDs, no binary framing, no capability negotiation -- a
command is answered by exactly the response(s) it produces, in order,
before the next command is read. Good enough for occasional, human- or
script-driven settings changes; genuinely insufficient for high-rate
telemetry (live sensor streaming) or anything needing to stay correct
under request/response interleaving -- both explicitly deferred to
whatever protocol version eventually covers that.

## Key catalog

Booleans are the literal text `0` or `1`. Floats round-trip through
`%.6f`/`strtof` (six decimal digits -- comfortably more precision than
any of these values are actually calibrated to yet).

| Key | Type | Values | Default |
|---|---|---|---|
| `pedal.mode` | enum | `sustain`, `expression` | `sustain` |
| `pedal.polarity` | enum | `normally_open`, `normally_closed` | `normally_open` |
| `expression.mpe_enabled` | bool | `0`, `1` | `1` |
| `expression.pitch_bend_sensitivity` | float | max cosine deviation | `0.065` |
| `expression.aftertouch_sensitivity` | uint | full-scale depth | `1450` |
| `cv_gate.enabled` | bool | `0`, `1` | `0` |
| `cv_gate.pitch.volts_per_semitone` | float | volts/semitone | `0.0833333` (1V/oct) |
| `cv_gate.pitch.reference_note` | int | MIDI note at 0V | `0` |
| `cv_gate.pitch.zero_trim_volts` | float | additive trim at the jack | `0.0` |
| `cv_gate.pitch.gain_trim` | float | multiplicative trim | `1.0` |
| `cv_gate.pressure.full_scale_volts` | float | volts at pressure=127 | `10.0` |
| `cv_gate.pressure.zero_trim_volts` | float | additive trim at the jack | `0.0` |
| `cv_gate.pressure.gain_trim` | float | multiplicative trim | `1.0` |

See each setting's own real definition (`firmware/src/services/pedal.h`,
`expression.h`, `cv_gate.h`) for what it actually controls -- this table
is the wire-format contract, not a re-explanation of the feature.

`cv_gate.enabled` being true here does **not** mean CV/gate is actually
driving anything -- `services/power.h`'s own external-power confirmation
is a separate, hardware-enforced gate this protocol has no ability to
override, by design (see `services/cv_gate.h`'s own header comment).

## Persistence

None of these settings are saved to flash yet -- every one resets to its
default on reboot, same as before this protocol existed. Real
persistence belongs in a `storage/` module that doesn't exist in this
codebase yet (see `docs/hardware/SENTIA_TILES_FIRMWARE_HANDOFF.md`'s own
recommended module boundary); a future version of this protocol, once
that exists, should write through to it rather than this document
inventing its own separate persistence scheme.

## Testing

`../../tools/tiles_control.py` -- a plain Python script (`pyusb`) that
connects to the vendor interface and exercises `LIST`/`GET`/`SET`
end-to-end. See that script's own header for setup (on macOS, `pyusb`
needs `libusb` installed, and vendor-class devices sometimes need the
terminal running it to have been granted the OS's own USB-device
permission prompt the first time).

## Scene Launch (Ableton Live)

A second, separate protocol over the SAME USB-MIDI port TILES already
uses for notes/CC/clock -- NOT the vendor-interface settings protocol
above (this one talks to Ableton Live's own Remote Script, not a host
control app). Real feedback: "lets implemebt a new mode that triggers
scenes in ableton live keep it simple for now... can we pull the colors
of the scenes from ableton?"

**Architecture, rewritten twice after several real-hardware rounds
with no confirmed successful delivery**: "master stop doesnt work at
all, individual start and stop doesnt work and hasent for the past few
pushes. i need you to look at how a lounchapd works or abletoun push
works to pull the exxact same standardizre behaviour." The TILES ->
Ableton direction used to be a custom SysEx sub-protocol; despite
passing review against Ableton's own real Remote Script source
multiple times, it never had one confirmed successful round-trip on
real hardware.

First rewrite replaced it with plain Note-On, matching how a real
Launchpad sends its own grid (confirmed against Ableton's bundled
`Launchpad.py`). Real feedback caught the real flaw: "you fully broke
how clip lounching works now its just sending regular midi notes for
me to map. thats not how this feature operates ever in any device." A
real Launchpad is a dedicated grid controller that never sends musical
note content at all, so nobody ever enables that port's "Track" MIDI
input in Ableton. TILES is not that -- this exact same USB-MIDI port
also carries real musical Note-On for melodic/chord/guitar/sequencer
play, so the user's own instrument track almost certainly already has
this port's Track input enabled (typically "All Channels," required
for real MPE playback), meaning a Scene Launch "button" Note-On is
ALSO delivered to that track as ordinary playable/recordable content
on top of whatever the Remote Script's own `ButtonElement` does with
it -- being claimed by the Control Surface's Remote path and reaching
a Track's input are not mutually exclusive in Ableton. A CC never has
this problem: Ableton never treats a CC as note/audio content for an
instrument regardless of Track/Remote routing, exactly why the
transport CCs have always been safe on this same port. Second rewrite
moved grid-touch/stop-touch off Note-On entirely, onto CC, matching
everything else in this protocol.

Both rewrites bind real `ButtonElement`s via `add_value_listener()` --
the same mechanism this project's own transport-remote CCs
(`OP_TRANSPORT_PLAY_CC` etc.) already use, with actual confirmed
real-hardware delivery.

| Direction | Transport | Meaning |
|---|---|---|
| TILES -> Ableton | CC, controller = `CC_GRID_BASE` (10) + pad, 127 then 0 | Pressure click: fire that pad's clip (track columns 1-5), launch that pad's whole scene (column 6), or -- on an EMPTY slot -- arm the track and record into it |
| TILES -> Ableton | CC, controller = `CC_STOP_BASE` (40) + pad, 127 then 0 | Pressure click on a clip that's already playing: stop that one clip (track columns 1-5 only) |
| TILES -> Ableton | CC `CC_MASTER_STOP` (105), 127 then 0 | Stop all clips (master stop) -- shift+diamond in Scene Launch mode |
| TILES -> Ableton | CC `CC_TRACK_OFFSET` (106), value = offset | Visible track window changed -- keeps the session-ring overlay and the pad-to-track mapping in sync |
| Ableton -> TILES | SysEx `F0 7D 01 10 track scene flags r7 g7 b7 F7` | One clip slot's current state |
| Ableton -> TILES | SysEx `F0 7D 01 11 scene flags r7 g7 b7 F7` | One scene's current state |
| Ableton -> TILES | SysEx `F0 7D 01 12 F7` | A track was just armed for a new recording -- open melodic mode (firmware waits until every pad is released) |

A bare capacitive touch sends NOTHING to Ableton -- it's haptics-only on the hardware side (a strong "ready" click on a pad with a clip, a continuous buzz while that clip is playing). Only a pressure click (Hall depth past the same 50% "push to select" threshold the mode menu uses) sends the CCs below.

All TILES -> Ableton messages are plain CC on
`TILES_MIDI_MPE_MASTER_CHANNEL` (channel 1) -- the same channel the
transport CCs use, deliberately never Note-On (see above). `pad` is
1-24 (`TILES_NUM_PADS`); Ableton-side, `scene_launch.py` derives
`(column, row)` from `pad` exactly like `op_mode.c`'s own
`handle_scene_launch_taps()` does, and derives the real track index
from `column` plus whatever `CC_TRACK_OFFSET` last reported (it has to
track that itself now, mirroring `op_mode.c`'s own
`s_scene_track_offset`).

The Ableton -> TILES SysEx frames are unchanged: manufacturer ID
`0x7D` (MMA-reserved "non-commercial/educational use"), sub-ID `0x01`,
parsed firmware-side by `firmware/src/midi/midi_in.c` and handled by
`op_mode.c`'s own "Scene Launch mode" section. `track` is 0-based, up
to 63 (`OP_SCENE_MAX_TRACKS`/`MAX_TRACKS`, firmware/Python
respectively); `scene` is 0-based, up to 3 (only Ableton's first 4
scenes are ever tracked -- no scene paging in this version). `flags`
bit 0 = has_clip (clip state only), bit 1 = is_playing (clip state
only), bit 2 = is_triggered (both). `r7`/`g7`/`b7` are each 0-127 --
Ableton's own 0-255 color channel halved; the firmware doubles it back
toward 8-bit on receipt, losing the bottom bit, not the top.

Ableton pushes state for every tracked cell once on script connect
(not just whatever's currently scrolled into view on the hardware),
then again on every real change via Live API listeners.

**Confidence**: the Ableton -> TILES SysEx wire format is exact and
firmware-verified. The CC reception mechanism for the other direction
matches this project's own already-confirmed-working transport CCs
exactly, and avoids the real, confirmed flaw the Note-On version hit
(leaking through as playable/recordable note content on any track
with this port's Track input enabled). A real bug in the CC version's
own `_connect()` -- calling a `register_components()` method that
doesn't exist on `ControlSurface` -- silently aborted button binding
on every load until fixed (replaced with the real, public
`set_highlighting_session_component()`); see `scene_launch.py`'s own
module docstring for the full story. The Live API calls themselves
(`add_playing_status_listener`, `song().stop_all_clips()`,
`Clip.stop()`) are each individually confirmed against Ableton's own
bundled Remote Script source or the community AbletonOSC project.

## Not built yet

Everything `docs/protocol/README.md`'s own design notes list beyond
settings: pad remap, guided calibration capture, live sensor streaming,
profile read/write, firmware update, and the real framing/versioning/
schema decisions those need. This document's own scope will need
revisiting, not just extending, once any of those get designed --
they're likely to need actual binary framing and sequencing this
version deliberately does without. Scene Launch above is a separate,
narrower SysEx sub-protocol built alongside this one, not folded into
it -- it talks to Ableton's Remote Script, not the vendor-interface
settings channel this document is otherwise about.
