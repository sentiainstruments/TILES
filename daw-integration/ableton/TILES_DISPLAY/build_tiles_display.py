#!/usr/bin/env python3
"""
Builds "TILES DISPLAY.amxd", a Max for Live MIDI Effect that shows the
notes reaching a track's instrument on TILES's pads.

Real feedback: "ableton instruments send either midi or audio after the
vst... we need to build a max for live device that slots in between the
device must have an arm button to display on tiles and if multiple
instances are on different tracks then it should disarm from previous...
we have an arm button and its a toggle, we can call it VIEW and make it
sentia style... the button is sentia pink when armed. the plugin is
called TILES DISPLAY."

Why a device at all: a track that has an instrument on it outputs AUDIO
after that instrument, not MIDI, so there is no MIDI output to route to
TILES (see daw-integration/README.md). A Max for Live MIDI Effect placed
BEFORE the instrument sees every note that reaches it -- clip playback,
live input, anything an earlier MIDI effect (arp, chord, ...) produced --
which is exactly "the melody the track is playing."

How notes reach TILES: the device calls the Live Object Model's
ControlSurface.send_midi on the TILES control surface, which writes
straight to that script's MIDI OUTPUT port (the same port
scene_launch.py already uses for its SysEx feedback) -- confirmed
against Cycling '74's LOM docs and forum reports of exactly this use
(e.g. lighting Push pads from a device). No network layer, no extra
Remote Script code. The one thing a device can't know is WHICH
control surface TILES is (the LOM exposes no name for one), so the
device has a small SURFACE number, saved with the Live set, that the
player sets once. It is the position among LOADED control surfaces, NOT
the Preferences slot number -- Live's own device bridge skips empty slots
(_MxDCore/LomTypes.py: get_control_surfaces() is tuple(filter(lambda c:
c is not None, application.control_surfaces))). The first version got
this wrong and every send_midi was rejected with "no valid object set"
(seen in Live's Log.txt); it also capped the range at 6 when Live 12.4
has 7 slots. To make finding the right number a matter of stepping it
rather than guessing, the device flashes pads on TILES whenever the
route could have just changed (SURFACE edited, VIEW turned on).

MPE / expression pass-through: the device is a pure tap. The MIDI thru
is ONE direct patchline, midiin -> midiout, with nothing parsed,
reformatted, filtered or delayed on it (midiparse/midiformat are known to
truncate per-note pitch bend to semitones -- deliberately nothing like
them is on the thru). Everything the tap does (notein -> gate ->
send_midi to TILES, the route-confirmation flash, the disarm flush) runs
off to the side and never writes back into the MIDI chain. The one thing
the thru could not do by itself is declare MPE support: the patcher's
is_mpe property (see build_patcher()) must be 1 or Live doesn't route the
per-note MPE stream through the device at all.

Two instances at once. Real feedback: "make the device work on 2 channels
at once, if 2 devices are on then the secondary does color red." (It began
life as exclusive -- arming one disarmed the rest -- and this replaces
that.) Up to two instances are armed together. The first armed is the
PRIMARY: its notes go out on MIDI channel 1 and light TILES' pads green
(the firmware's original echo color) and its VIEW button is Sentia pink.
The second armed is the SECONDARY: channel 2, red pads, red VIEW button
(firmware: services/op_mode.c echo layers, services/lighting.c). Arming a
third replaces the secondary (the primary is never bumped); disarming the
primary promotes the secondary to primary, so a lone armed device is never
left red.

How the instances agree on who is primary without any shared variable:
every instance shares Max's global name space (a [send]/[receive] name
WITHOUT the "---" prefix is global across every Max for Live device in the
set) and each keeps its own slot (0 = not armed, 1 = primary, 2 =
secondary). Arming asks the others "who holds slot 1?" on
tiles_display_who -- a message send is synchronous, so the answer
(tiles_display_taken) is in before the send returns -- and takes slot 1 if
nobody answers, else slot 2 (announcing that on tiles_display_bump so a
previous secondary steps down). Disarming slot 1 announces
tiles_display_freed so a secondary promotes itself. No stored ids: the
live instances ARE the state, so a device that was deleted while armed
(no delete notification exists) can never wedge a slot -- the next arm
just finds nobody answering.

This file is the source of truth for the device -- the .amxd is generated
from it, so a change to the device is a change here, then re-run:

    python3 build_tiles_display.py

Not verified inside Live from the machine this was written on (no way to
drive Live's UI headlessly) -- the generator validates the patcher's
structure (every connection points at a real inlet/outlet of a real
object) and the output parses back through the same container reader used
on Ableton's own factory devices, but the first real proof is loading it
in Live. See daw-integration/README.md for the exact checklist.
"""

import json
import os
import struct

# ---- Sentia palette (RGBA 0-1) -------------------------------------------
# Sentia Instruments Magenta (#FF00FF) -- the same brand color the firmware
# already calls "Sentia magenta" (services/lighting.c root pad, op_mode.c
# OP_MENU_MELODIC_*).
PINK = [1.0, 0.0, 1.0, 1.0]
PINK_DIM = [0.55, 0.0, 0.55, 1.0]
BG = [0.055, 0.055, 0.063, 1.0]
BUTTON_OFF_BG = [0.13, 0.13, 0.15, 1.0]
TEXT_LIGHT = [0.92, 0.92, 0.95, 1.0]
TEXT_DIM = [0.55, 0.55, 0.60, 1.0]
TEXT_ON_PINK = [0.04, 0.04, 0.05, 1.0]

DEVICE_WIDTH = 160.0
# NO "---" prefix on any of these: they must be global across instances.
BUS_WHO = "tiles_display_who"  # "does anyone hold slot 1?"
BUS_TAKEN = "tiles_display_taken"  # ...yes (sent by the slot-1 holder)
BUS_BUMP = "tiles_display_bump"  # "I just took slot 2" (previous slot 2 steps down)
BUS_FREED = "tiles_display_freed"  # "slot 1 just opened up" (slot 2 promotes)
RED = [1.0, 0.09, 0.09, 1.0]

FONT = "Ableton Sans Medium Regular"

_boxes = []
_lines = []
_by_name = {}


def _add(name, box):
    box["id"] = "obj-%d" % (len(_boxes) + 1)
    _boxes.append({"box": box})
    _by_name[name] = box
    return box


def newobj(name, text, rect, inlets, outlets, outlettype=None):
    box = {
        "fontname": "Arial Bold",
        "fontsize": 10.0,
        "maxclass": "newobj",
        "numinlets": inlets,
        "numoutlets": outlets,
        "patching_rect": rect,
        "text": text,
    }
    if outlets:
        box["outlettype"] = outlettype or [""] * outlets
    return _add(name, box)


def message(name, text, rect):
    return _add(
        name,
        {
            "fontname": "Arial Bold",
            "fontsize": 10.0,
            "maxclass": "message",
            "numinlets": 2,
            "numoutlets": 1,
            "outlettype": [""],
            "patching_rect": rect,
            "text": text,
        },
    )


def comment(name, text, patch_rect, pres_rect, size, color, bold=False):
    return _add(
        name,
        {
            "fontname": "Ableton Sans Bold Regular" if bold else FONT,
            "fontsize": size,
            "maxclass": "comment",
            "numinlets": 1,
            "numoutlets": 0,
            "patching_rect": patch_rect,
            "presentation": 1,
            "presentation_rect": pres_rect,
            "text": text,
            "textcolor": color,
        },
    )


def panel(name, patch_rect, pres_rect, color, background):
    box = {
        "bgcolor": color,
        "border": 0,
        "maxclass": "panel",
        "mode": 0,
        "numinlets": 1,
        "numoutlets": 0,
        "patching_rect": patch_rect,
        "presentation": 1,
        "presentation_rect": pres_rect,
        "rounded": 0,
    }
    if background:
        box["background"] = 1
    return _add(name, box)


def conn(src, src_outlet, dst, dst_inlet):
    _lines.append(
        {
            "patchline": {
                "destination": [_by_name[dst]["id"], dst_inlet],
                "source": [_by_name[src]["id"], src_outlet],
            }
        }
    )


def build_patcher():
    # ---- Presentation (what the player sees in Live) ---------------------
    panel("bg", [600.0, 16.0, 40.0, 40.0], [0.0, 0.0, DEVICE_WIDTH, 169.0], BG, background=True)
    comment("title", "TILES DISPLAY", [600.0, 64.0, 100.0, 20.0], [12.0, 10.0, 136.0, 20.0], 12.0, TEXT_LIGHT, bold=True)
    panel("accent", [600.0, 92.0, 40.0, 10.0], [12.0, 34.0, 136.0, 2.0], PINK, background=False)

    # VIEW: the arm toggle. Sentia pink when armed. mode 1 = toggle.
    # activebg*/activetext* apply while the device is active (the normal
    # case); bg*/text* are the deactivated-device look, kept as a dimmer
    # take on the same colors so a greyed-out device doesn't suddenly
    # look off-brand.
    _add(
        "view",
        {
            "activebgcolor": BUTTON_OFF_BG,
            "activebgoncolor": PINK,
            "activetextcolor": TEXT_DIM,
            "activetextoncolor": TEXT_ON_PINK,
            "annotation": "Arm this track's notes to show on TILES' pads. Two can be armed at once: the first is pink (green pads), the second red.",
            "bgcolor": [0.10, 0.10, 0.11, 1.0],
            "bgoncolor": PINK_DIM,
            "bordercolor": PINK_DIM,
            "fontname": "Ableton Sans Bold Regular",
            "fontsize": 15.0,
            "maxclass": "live.text",
            "mode": 1,
            "numinlets": 1,
            "numoutlets": 2,
            "outlettype": ["", ""],
            "parameter_enable": 1,
            "patching_rect": [32.0, 260.0, 60.0, 20.0],
            "presentation": 1,
            "presentation_rect": [12.0, 46.0, 136.0, 44.0],
            "rounded": 6.0,
            "saved_attribute_attributes": {
                "valueof": {
                    "parameter_longname": "VIEW",
                    "parameter_shortname": "VIEW",
                    "parameter_type": 2,
                    "parameter_mmax": 1.0,
                    "parameter_enum": ["off", "on"],
                    "parameter_initial_enable": 1,
                    "parameter_initial": [0],
                    "parameter_unitstyle": 0,
                }
            },
            "text": "VIEW",
            "textcolor": TEXT_DIM,
            "texton": "VIEW",
            "textoncolor": TEXT_ON_PINK,
            "varname": "VIEW",
        },
    )

    comment("surface_label", "SURFACE", [600.0, 120.0, 60.0, 16.0], [12.0, 106.0, 70.0, 16.0], 10.0, TEXT_DIM)
    # SURFACE: which LOADED control surface TILES is (1-based here, 0-based
    # to the LOM -- see the "- 1" below). NOT the Preferences slot number:
    # Live's own device bridge (_MxDCore/LomTypes.py get_control_surfaces)
    # is tuple(filter(lambda c: c is not None, app.control_surfaces)) --
    # empty slots are skipped, so N counts loaded scripts only. Found from
    # a real failed first run: the log showed every send_midi rejected with
    # "no valid object set" because the first version told the player to
    # enter their slot number (3) when the index among loaded scripts
    # could be as low as 1. Saved with the set.
    _add(
        "surface",
        {
            "activebgcolor": BUTTON_OFF_BG,
            "activetricolor2": PINK,
            "appearance": 1,
            "bordercolor": PINK_DIM,
            "fontface": 0,
            "fontsize": 11.0,
            "maxclass": "live.numbox",
            "numinlets": 1,
            "numoutlets": 2,
            "outlettype": ["", "float"],
            "parameter_enable": 1,
            "patching_rect": [232.0, 40.0, 40.0, 16.0],
            "presentation": 1,
            "presentation_rect": [96.0, 104.0, 52.0, 18.0],
            "saved_attribute_attributes": {
                "valueof": {
                    "parameter_longname": "SURFACE",
                    "parameter_shortname": "SURFACE",
                    "parameter_type": 1,
                    "parameter_mmin": 1.0,
                    "parameter_mmax": 7.0,
                    "parameter_initial_enable": 1,
                    "parameter_initial": [1],
                    "parameter_unitstyle": 0,
                }
            },
            "varname": "SURFACE",
        },
    )
    comment(
        "hint",
        "Pads flash on TILES when SURFACE is right. Step it (1-7) until they do. 2nd armed = red.",
        [600.0, 150.0, 200.0, 30.0],
        [12.0, 128.0, 136.0, 34.0],
        9.0,
        TEXT_DIM,
    )

    # ---- MIDI thru: always, unconditionally -- the device must never
    # change what the instrument after it hears. ---------------------------
    newobj("midiin", "midiin", [32.0, 16.0, 45.0, 20.0], 1, 1, ["int"])
    newobj("midiout", "midiout", [32.0, 560.0, 52.0, 20.0], 1, 0)
    conn("midiin", 0, "midiout", 0)

    # ---- Note tap: notein fires right-to-left (channel, velocity, pitch),
    # so pitch (leftmost) arrives LAST and is the hot inlet of the pack --
    # velocity is already stored by then. Note-offs arrive as velocity 0,
    # which is exactly what the firmware's midi_in.c parser already reads
    # as a Note-Off (Note-On with velocity 0). -----------------------------
    newobj("notein", "notein", [232.0, 16.0, 50.0, 20.0], 1, 3, ["int", "int", "int"])
    newobj("pack_live", "pack 0 0", [232.0, 96.0, 52.0, 20.0], 2, 1)
    newobj("gate", "gate 1", [232.0, 168.0, 45.0, 20.0], 2, 1)
    # Everything that sends to TILES -- the note tap, the disarm flush, the
    # route flash -- lands on status_gate's data inlet as a (pitch velocity)
    # list; its control inlet picks which Note-On status the send_midi gets:
    # 1 -> 144 (channel 1, the primary), 2 -> 145 (channel 2, the secondary).
    # Two fixed prepends and a gate rather than one prepend re-pointed with
    # "set": a gate is what this patch already trusts, and there's nothing
    # to get subtly wrong about which status a queued message picked up.
    # Opens on 1 -- an unarmed instance (whose route flash still fires)
    # behaves as the primary.
    newobj("status_gate", "gate 2 1", [232.0, 200.0, 45.0, 20.0], 2, 2, ["", ""])
    newobj("prep_send_a", "prepend call send_midi 144", [232.0, 232.0, 170.0, 20.0], 1, 1)
    newobj("prep_send_b", "prepend call send_midi 145", [420.0, 232.0, 170.0, 20.0], 1, 1)
    newobj("lobj", "live.object", [232.0, 296.0, 68.0, 20.0], 2, 1)
    conn("notein", 0, "pack_live", 0)  # pitch  -> hot
    conn("notein", 1, "pack_live", 1)  # velocity -> cold
    conn("pack_live", 0, "gate", 1)
    conn("gate", 0, "status_gate", 1)
    conn("status_gate", 0, "prep_send_a", 0)
    conn("status_gate", 1, "prep_send_b", 0)
    conn("prep_send_a", 0, "lobj", 0)
    conn("prep_send_b", 0, "lobj", 0)

    # ---- Which control_surfaces slot is TILES: SURFACE (1-6) -> 0-based
    # LOM path -> live.path resolves it to an id -> live.object's right
    # inlet. ----------------------------------------------------------------
    newobj("surface_zero", "- 1", [232.0, 64.0, 32.0, 20.0], 2, 1, ["int"])
    newobj("prep_path", "prepend path control_surfaces", [320.0, 64.0, 180.0, 20.0], 1, 1)
    newobj("lpath_surface", "live.path", [320.0, 128.0, 62.0, 20.0], 1, 3, ["", "", ""])
    conn("surface", 0, "surface_zero", 0)
    conn("surface_zero", 0, "prep_path", 0)
    conn("prep_path", 0, "lpath_surface", 0)
    # Both outlets, on purpose: the left sends the id in direct response to
    # this path message; the middle ALSO re-sends whenever the object at
    # that path changes later (e.g. the TILES Remote Script gets loaded or
    # reloaded after this device already did) -- per live.path's own docs,
    # and how Max's own patches wire it (12 of 13 use the middle outlet).
    # Duplicate identical ids into live.object are harmless.
    conn("lpath_surface", 0, "lobj", 1)
    conn("lpath_surface", 1, "lobj", 1)

    # ---- VIEW: only real transitions matter (a redundant 0, e.g. from an
    # unarmed instance being told to disarm, must not flush). ----------------
    newobj("view_change", "change", [32.0, 296.0, 46.0, 20.0], 1, 3, ["int", "int", "int"])
    conn("view", 0, "view_change", 0)
    conn("view_change", 0, "gate", 0)

    # ---- Two instances at once: slots, see the module docstring ------------
    # thisdev is still needed further down (the route flash's load guard).
    newobj("thisdev", "live.thisdevice", [32.0, 328.0, 83.0, 20.0], 1, 3, ["bang", "int", "int"])

    # This instance's slot (0 = not armed, 1 = primary, 2 = secondary), kept
    # in FOUR [int] copies -- one per question this instance can be asked
    # (an [int]'s outlet fans out to every connection on each bang, so the
    # questions can't share one). slot_store is the single write point: it
    # feeds every copy's cold (right) inlet.
    newobj("slot_store", "t i", [760.0, 16.0, 30.0, 20.0], 1, 1, ["int"])
    for name, y in (("slot_who", 60.0), ("slot_free", 92.0), ("slot_bump", 124.0), ("slot_prom", 156.0)):
        newobj(name, "int 0", [760.0, y, 32.0, 20.0], 2, 1, ["int"])
        conn("slot_store", 0, name, 1)

    # Only real transitions matter (a redundant 0, e.g. from an unarmed
    # instance being told to step down, must not flush).
    newobj("view_change", "change", [32.0, 296.0, 46.0, 20.0], 1, 3, ["int", "int", "int"])
    conn("view", 0, "view_change", 0)
    conn("view_change", 0, "gate", 0)
    newobj("sel_view", "select 1 0", [32.0, 344.0, 62.0, 20.0], 2, 3, ["bang", "bang", ""])
    conn("view_change", 0, "sel_view", 0)

    # ARM (VIEW just turned on). [t b b b b] fires right to left:
    #   1. clear taken_flag        2. ask "who holds slot 1?" (synchronous)
    #   3. slot = taken_flag + 1   4. the route-confirmation flash, last, so
    #      it goes out on the NEW slot's channel (a secondary flashes red).
    newobj("arm_t", "t b b b b", [32.0, 392.0, 66.0, 20.0], 1, 4, ["bang", "bang", "bang", "bang"])
    message("msg_t1_reset", "0", [120.0, 392.0, 24.0, 20.0])
    newobj("taken_flag", "int 0", [120.0, 424.0, 32.0, 20.0], 2, 1, ["int"])
    newobj("send_who", "s " + BUS_WHO, [160.0, 392.0, 120.0, 20.0], 1, 0)
    newobj("plus1", "+ 1", [120.0, 456.0, 32.0, 20.0], 2, 1, ["int"])
    conn("sel_view", 0, "arm_t", 0)
    conn("arm_t", 3, "msg_t1_reset", 0)
    conn("msg_t1_reset", 0, "taken_flag", 1)
    conn("arm_t", 2, "send_who", 0)
    conn("arm_t", 1, "taken_flag", 0)
    conn("taken_flag", 0, "plus1", 0)

    # Slot 1's holder answers the question; everyone's receiver sets their
    # taken_flag, but only the asker (who just cleared it) reads it.
    newobj("recv_who", "r " + BUS_WHO, [800.0, 60.0, 110.0, 20.0], 1, 1)
    newobj("sel_who", "select 1", [800.0, 92.0, 52.0, 20.0], 2, 2, ["bang", ""])
    newobj("send_taken", "s " + BUS_TAKEN, [800.0, 124.0, 120.0, 20.0], 1, 0)
    newobj("recv_taken", "r " + BUS_TAKEN, [800.0, 156.0, 120.0, 20.0], 1, 1)
    message("msg_t1_set", "1", [800.0, 188.0, 24.0, 20.0])
    conn("recv_who", 0, "slot_who", 0)
    conn("slot_who", 0, "sel_who", 0)
    conn("sel_who", 0, "send_taken", 0)
    conn("recv_taken", 0, "msg_t1_set", 0)
    conn("msg_t1_set", 0, "taken_flag", 1)

    # Take the slot. [t i i i i] right to left: announce (BEFORE storing --
    # our own slot copies are still 0, so we don't hear our own bump and
    # step ourselves down), store, pick the channel, recolor VIEW.
    newobj("slot_t", "t i i i i", [120.0, 488.0, 76.0, 20.0], 1, 4, ["int", "int", "int", "int"])
    newobj("sel_bump_send", "select 2", [232.0, 520.0, 52.0, 20.0], 2, 2, ["bang", ""])
    newobj("send_bump", "s " + BUS_BUMP, [232.0, 552.0, 120.0, 20.0], 1, 0)
    newobj("sel_color", "select 2", [120.0, 520.0, 52.0, 20.0], 2, 2, ["bang", ""])
    message("msg_red", "activebgoncolor 1. 0.09 0.09 1.", [120.0, 552.0, 190.0, 20.0])
    message("msg_pink", "activebgoncolor 1. 0. 1. 1.", [120.0, 584.0, 170.0, 20.0])
    conn("plus1", 0, "slot_t", 0)
    conn("slot_t", 3, "sel_bump_send", 0)
    conn("sel_bump_send", 0, "send_bump", 0)
    conn("slot_t", 2, "slot_store", 0)
    conn("slot_t", 1, "status_gate", 0)
    conn("slot_t", 0, "sel_color", 0)
    conn("sel_color", 0, "msg_red", 0)
    conn("sel_color", 1, "msg_pink", 0)
    conn("msg_red", 0, "view", 0)
    conn("msg_pink", 0, "view", 0)

    # Hearing "someone took slot 2": if that was us before, turn VIEW off
    # (msg_zero -> view -> change -> the disarm chain below flushes).
    newobj("recv_bump", "r " + BUS_BUMP, [800.0, 232.0, 120.0, 20.0], 1, 1)
    newobj("sel_bump_me", "select 2", [800.0, 264.0, 52.0, 20.0], 2, 2, ["bang", ""])
    message("msg_zero", "0", [800.0, 296.0, 24.0, 20.0])
    conn("recv_bump", 0, "slot_bump", 0)
    conn("slot_bump", 0, "sel_bump_me", 0)
    conn("sel_bump_me", 0, "msg_zero", 0)
    conn("msg_zero", 0, "view", 0)

    # DISARM (VIEW just turned off). [t b b b] right to left: flush every
    # pitch (still on our own channel), tell the others if we held slot 1,
    # then clear our slot.
    newobj("disarm_t", "t b b b", [32.0, 620.0, 52.0, 20.0], 1, 3, ["bang", "bang", "bang"])
    newobj("sel_was1", "select 1", [120.0, 620.0, 52.0, 20.0], 2, 2, ["bang", ""])
    newobj("send_freed", "s " + BUS_FREED, [120.0, 652.0, 120.0, 20.0], 1, 0)
    message("msg_slot0", "0", [232.0, 620.0, 24.0, 20.0])
    conn("sel_view", 1, "disarm_t", 0)
    conn("disarm_t", 1, "slot_free", 0)
    conn("slot_free", 0, "sel_was1", 0)
    conn("sel_was1", 0, "send_freed", 0)
    conn("disarm_t", 0, "msg_slot0", 0)
    conn("msg_slot0", 0, "slot_store", 0)

    # PROMOTE (slot 1 just freed, and we hold slot 2). [t b b b]: flush our
    # notes on the old channel, switch to channel 1 + slot 1, recolor pink.
    newobj("recv_freed", "r " + BUS_FREED, [800.0, 340.0, 120.0, 20.0], 1, 1)
    newobj("sel_prom", "select 2", [800.0, 372.0, 52.0, 20.0], 2, 2, ["bang", ""])
    newobj("prom_t", "t b b b", [800.0, 404.0, 52.0, 20.0], 1, 3, ["bang", "bang", "bang"])
    message("msg_p1", "1", [860.0, 436.0, 24.0, 20.0])
    conn("recv_freed", 0, "slot_prom", 0)
    conn("slot_prom", 0, "sel_prom", 0)
    conn("sel_prom", 0, "prom_t", 0)
    conn("prom_t", 1, "msg_p1", 0)
    conn("msg_p1", 0, "status_gate", 0)
    conn("msg_p1", 0, "slot_store", 0)
    conn("prom_t", 0, "msg_pink", 0)

    # Flush: 128 Note-Offs (pitch 0-127, velocity 0), bypassing the gate
    # (which is closing) straight into the status gate -- still set to this
    # instance's own channel, so a secondary clears only its own notes.
    newobj("uzi", "uzi 128", [232.0, 360.0, 52.0, 20.0], 2, 3, ["bang", "bang", "int"])
    newobj("uzi_zero", "- 1", [300.0, 392.0, 32.0, 20.0], 2, 1, ["int"])
    newobj("pack_flush", "pack 0 0", [300.0, 424.0, 52.0, 20.0], 2, 1)
    conn("disarm_t", 2, "uzi", 0)
    conn("prom_t", 2, "uzi", 0)
    conn("uzi", 2, "uzi_zero", 0)
    conn("uzi_zero", 0, "pack_flush", 0)
    conn("pack_flush", 0, "status_gate", 1)

    # ---- Route confirmation: a brief flash of pads on TILES whenever the
    # route could have just changed (SURFACE edited, or VIEW turned on), so
    # finding the right SURFACE number is "step it until pads flash" rather
    # than guesswork -- the LOM gives a device no way to ask a control
    # surface what script it is. Notes 36-96 all at once (a scale/octave
    # setting maps only some of them to pads, so a wide run guarantees some
    # pads light in any scale), held 300 ms, then Note-Off. Bypasses the VIEW
    # gate on purpose (straight into the same send_midi prepend as the
    # flush), so it works before VIEW is armed. A load guard swallows the
    # numbox's own restore-on-load output so a Live set full of instances
    # doesn't flash TILES on every open. ------------------------------------
    newobj("load_delay", "delay 1500", [560.0, 40.0, 62.0, 20.0], 2, 1, ["bang"])
    message("msg_one", "1", [560.0, 72.0, 24.0, 20.0])
    newobj("flash_gate", "gate 1", [560.0, 168.0, 45.0, 20.0], 2, 1)
    newobj("surf_delay", "delay 250", [640.0, 100.0, 62.0, 20.0], 2, 1, ["bang"])
    newobj("flash_go", "t b b", [560.0, 200.0, 40.0, 20.0], 1, 2, ["bang", "bang"])
    newobj("uzi_on", "uzi 61", [560.0, 232.0, 46.0, 20.0], 2, 3, ["bang", "bang", "int"])
    newobj("add_on", "+ 35", [560.0, 264.0, 32.0, 20.0], 2, 1, ["int"])
    newobj("pack_on", "pack 0 100", [560.0, 296.0, 62.0, 20.0], 2, 1)
    newobj("delay_off", "delay 300", [640.0, 232.0, 62.0, 20.0], 2, 1, ["bang"])
    newobj("uzi_off", "uzi 61", [640.0, 264.0, 46.0, 20.0], 2, 3, ["bang", "bang", "int"])
    newobj("add_off", "+ 35", [640.0, 296.0, 32.0, 20.0], 2, 1, ["int"])
    newobj("pack_off", "pack 0 0", [640.0, 328.0, 52.0, 20.0], 2, 1)
    conn("thisdev", 0, "load_delay", 0)
    conn("load_delay", 0, "msg_one", 0)
    conn("msg_one", 0, "flash_gate", 0)  # gate opens 1.5 s after load
    conn("surface", 0, "surf_delay", 0)  # let the new id land first
    conn("surf_delay", 0, "flash_gate", 1)
    conn("arm_t", 0, "flash_gate", 1)  # VIEW just turned on (after the slot is taken)
    conn("flash_gate", 0, "flash_go", 0)
    conn("flash_go", 1, "delay_off", 0)  # right first: schedule the Note-Offs
    conn("flash_go", 0, "uzi_on", 0)  # then fire the Note-Ons
    conn("uzi_on", 2, "add_on", 0)
    conn("add_on", 0, "pack_on", 0)
    conn("pack_on", 0, "status_gate", 1)
    conn("delay_off", 0, "uzi_off", 0)
    conn("uzi_off", 2, "add_off", 0)
    conn("add_off", 0, "pack_off", 0)
    conn("pack_off", 0, "status_gate", 1)

    patcher = {
        "fileversion": 1,
        "appversion": {"major": 8, "minor": 1, "revision": 2, "architecture": "x64", "modernui": 1},
        "classnamespace": "box",
        "rect": [65.0, 399.0, 960.0, 700.0],
        "openrect": [0.0, 0.0, 0.0, 169.0],
        "bglocked": 0,
        "openinpresentation": 1,
        "default_fontsize": 10.0,
        "default_fontface": 0,
        "default_fontname": "Arial Bold",
        "gridonopen": 1,
        "gridsize": [8.0, 8.0],
        "gridsnaponopen": 1,
        "objectsnaponopen": 1,
        "statusbarvisible": 2,
        "toolbarvisible": 1,
        "lefttoolbarpinned": 0,
        "toptoolbarpinned": 0,
        "righttoolbarpinned": 0,
        "bottomtoolbarpinned": 0,
        "toolbars_unpinned_last_save": 0,
        "tallnewobj": 0,
        "boxanimatetime": 500,
        "enablehscroll": 1,
        "enablevscroll": 1,
        "devicewidth": DEVICE_WIDTH,
        "description": "Shows this track's playing notes on TILES' pads. Place BEFORE the instrument.",
        "digest": "Shows a track's notes on TILES",
        "tags": "",
        "style": "",
        "subpatcher_template": "",
        "title": "TILES DISPLAY",
        "boxes": _boxes,
        "lines": _lines,
        "dependency_cache": [],
        "latency": 0,
        # "Patch Supports MPE" (is_mpe) -- a top-level patcher property, and
        # the reason this device used to strip MPE even though its thru is
        # a single direct midiin -> midiout patchline: a Max for Live device
        # has to DECLARE MPE support, or Live doesn't hand it the per-note
        # MPE stream (per-note pitch bend, slide/CC74, channel pressure, on
        # member channels 2-16) in the first place. Max's own help text
        # (help/m4l/live.push.maxhelp): "To receive MPE, make sure the
        # 'Patch Supports MPE' (is_mpe) attribute is set to 1 for the
        # device"; the Cycling '74 forum thread on passing MPE through a
        # MIDI effect says the same; real devices serialize it right next
        # to "latency" (checked in an unencrypted Ableton pack device). The
        # generator left it out, so it defaulted to 0.
        "is_mpe": 1,
        "minimum_live_version": "",
        "minimum_max_version": "",
        "platform_compatibility": 0,
        "project": {
            "version": 1,
            "creationdate": 3590052786,
            "modificationdate": 3590052786,
            "viewrect": [0.0, 0.0, 300.0, 500.0],
            "autoorganize": 1,
            "hideprojectwindow": 1,
            "showdependencies": 1,
            "autolocalize": 0,
            "contents": {"patchers": {}},
            "layout": {},
            "searchpath": {},
            "detailsvisible": 0,
            "amxdtype": 1835887981,
            "readonly": 0,
            "devpathtype": 0,
            "devpath": ".",
            "sortmode": 0,
            "viewmode": 0,
        },
        "autosave": 0,
    }
    return {"patcher": patcher}


def validate(doc):
    """Every connection must name a real box and an inlet/outlet index that
    box actually has -- a bad index silently drops the connection when Max
    loads the file, which would look like a device that just doesn't work."""
    boxes = {b["box"]["id"]: b["box"] for b in doc["patcher"]["boxes"]}
    problems = []
    for line in doc["patcher"]["lines"]:
        pl = line["patchline"]
        sid, so = pl["source"]
        did, di = pl["destination"]
        if sid not in boxes or did not in boxes:
            problems.append("unknown box in %r" % (pl,))
            continue
        if so >= boxes[sid]["numoutlets"]:
            problems.append("%s (%s) has no outlet %d" % (sid, boxes[sid].get("text", boxes[sid]["maxclass"]), so))
        if di >= boxes[did]["numinlets"]:
            problems.append("%s (%s) has no inlet %d" % (did, boxes[did].get("text", boxes[did]["maxclass"]), di))
    return problems


def write_amxd(doc, path):
    # Same container Ableton's own factory "Max MIDI Effect" template uses:
    # "ampf" + version(4) + "mmmm" (MIDI effect; "aaaa" audio effect,
    # "iiii" instrument) + "meta" chunk (4 bytes, zero) + "ptch" chunk
    # (uint32 little-endian length, then the patcher JSON and one NUL).
    body = json.dumps(doc, indent="\t", ensure_ascii=False).encode("utf-8") + b"\n\x00"
    header = (
        b"ampf"
        + struct.pack("<I", 4)
        + b"mmmm"
        + b"meta"
        + struct.pack("<I", 4)
        + struct.pack("<I", 0)
        + b"ptch"
        + struct.pack("<I", len(body))
    )
    with open(path, "wb") as f:
        f.write(header + body)


def read_back(path):
    """Parse the file back the way Live's own devices parse -- proves the
    container and JSON are well-formed."""
    data = open(path, "rb").read()
    assert data[:4] == b"ampf", "bad magic"
    assert data[8:12] == b"mmmm", "not a MIDI effect"
    i = 12
    while i + 8 <= len(data):
        tag = data[i : i + 4]
        length = struct.unpack("<I", data[i + 4 : i + 8])[0]
        body = data[i + 8 : i + 8 + length]
        if tag == b"ptch":
            assert len(body) == length, "truncated ptch chunk"
            return json.loads(body.rstrip(b"\x00").decode("utf-8"))
        i += 8 + length
    raise AssertionError("no ptch chunk")


if __name__ == "__main__":
    doc = build_patcher()
    problems = validate(doc)
    if problems:
        raise SystemExit("device graph invalid:\n  " + "\n  ".join(problems))
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "TILES DISPLAY.amxd")
    write_amxd(doc, out)
    back = read_back(out)
    assert back["patcher"]["title"] == "TILES DISPLAY"
    print("wrote %s (%d bytes, %d objects, %d connections)" % (out, os.path.getsize(out), len(doc["patcher"]["boxes"]), len(doc["patcher"]["lines"])))
