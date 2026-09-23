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

Exclusive arming across instances: every instance shares Max's global
name space (a [send]/[receive] name WITHOUT the "---" prefix is global
across every Max for Live device in the set), so turning VIEW on
broadcasts this instance's device id on one global name; every OTHER
instance hears a different id and turns its own VIEW off.

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
GLOBAL_ARM_NAME = "tiles_display_view"  # NO "---" prefix: must be global across instances

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
            "annotation": "Arm this track's notes to show on TILES' pads. Only one TILES DISPLAY is armed at a time.",
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
        "Pads flash green on TILES when SURFACE is right. Step it (1-7) until they do.",
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
    newobj("prep_send", "prepend call send_midi 144", [232.0, 232.0, 170.0, 20.0], 1, 1)
    newobj("lobj", "live.object", [232.0, 296.0, 68.0, 20.0], 2, 1)
    conn("notein", 0, "pack_live", 0)  # pitch  -> hot
    conn("notein", 1, "pack_live", 1)  # velocity -> cold
    conn("pack_live", 0, "gate", 1)
    conn("gate", 0, "prep_send", 0)
    conn("prep_send", 0, "lobj", 0)

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

    # ---- Exclusive arming across instances via Max's global name space ----
    newobj("thisdev", "live.thisdevice", [32.0, 328.0, 83.0, 20.0], 1, 3, ["bang", "int", "int"])
    message("msg_this", "path this_device", [32.0, 360.0, 92.0, 20.0])
    newobj("lpath_id", "live.path", [32.0, 392.0, 62.0, 20.0], 1, 3, ["", "", ""])
    newobj("route_id", "route id", [32.0, 424.0, 48.0, 20.0], 1, 2)
    newobj("myid", "int", [32.0, 456.0, 32.0, 20.0], 2, 1, ["int"])
    newobj("ne", "!= 0", [120.0, 456.0, 38.0, 20.0], 2, 1, ["int"])
    conn("thisdev", 0, "msg_this", 0)
    conn("msg_this", 0, "lpath_id", 0)
    conn("lpath_id", 0, "route_id", 0)
    conn("route_id", 0, "myid", 1)  # store, don't fire
    conn("route_id", 0, "ne", 1)  # right inlet stores the compare value

    # Turning VIEW on: broadcast this instance's id. Turning it off:
    # flush every pitch so no pad is left lit on TILES.
    newobj("sel_view", "select 1 0", [32.0, 344.0, 62.0, 20.0], 2, 3, ["bang", "bang", ""])
    newobj("send_arm", "s " + GLOBAL_ARM_NAME, [32.0, 488.0, 120.0, 20.0], 1, 0)
    conn("view_change", 0, "sel_view", 0)
    conn("sel_view", 0, "myid", 0)  # bang -> outputs the stored id
    conn("myid", 0, "send_arm", 0)

    # Hearing a DIFFERENT instance arm: turn this one's VIEW off.
    newobj("recv_arm", "r " + GLOBAL_ARM_NAME, [120.0, 392.0, 120.0, 20.0], 1, 1)
    newobj("sel_other", "select 1", [120.0, 488.0, 52.0, 20.0], 2, 2, ["bang", ""])
    message("msg_zero", "0", [120.0, 520.0, 24.0, 20.0])
    conn("recv_arm", 0, "ne", 0)
    conn("ne", 0, "sel_other", 0)
    conn("sel_other", 0, "msg_zero", 0)
    conn("msg_zero", 0, "view", 0)

    # Flush: 128 Note-Offs (pitch 0-127, velocity 0), bypassing the gate
    # (which is closing) straight into the same send_midi prepend.
    newobj("uzi", "uzi 128", [232.0, 360.0, 52.0, 20.0], 2, 3, ["bang", "bang", "int"])
    newobj("uzi_zero", "- 1", [300.0, 392.0, 32.0, 20.0], 2, 1, ["int"])
    newobj("pack_flush", "pack 0 0", [300.0, 424.0, 52.0, 20.0], 2, 1)
    conn("sel_view", 1, "uzi", 0)
    conn("uzi", 2, "uzi_zero", 0)
    conn("uzi_zero", 0, "pack_flush", 0)
    conn("pack_flush", 0, "prep_send", 0)

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
    conn("sel_view", 0, "flash_gate", 1)  # VIEW just turned on
    conn("flash_gate", 0, "flash_go", 0)
    conn("flash_go", 1, "delay_off", 0)  # right first: schedule the Note-Offs
    conn("flash_go", 0, "uzi_on", 0)  # then fire the Note-Ons
    conn("uzi_on", 2, "add_on", 0)
    conn("add_on", 0, "pack_on", 0)
    conn("pack_on", 0, "prep_send", 0)
    conn("delay_off", 0, "uzi_off", 0)
    conn("uzi_off", 2, "add_off", 0)
    conn("add_off", 0, "pack_off", 0)
    conn("pack_off", 0, "prep_send", 0)

    patcher = {
        "fileversion": 1,
        "appversion": {"major": 8, "minor": 1, "revision": 2, "architecture": "x64", "modernui": 1},
        "classnamespace": "box",
        "rect": [65.0, 399.0, 760.0, 640.0],
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
