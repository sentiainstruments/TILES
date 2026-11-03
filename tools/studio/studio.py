#!/usr/bin/env python3
"""TILES calibration studio: what each key's Hall sensor reads, what
velocity, pressure and pitch bend make of it, a guided calibration, and live
tuning, in a browser.

    ~/.venvs/tiles-tools/bin/python tools/studio/studio.py      # then open http://localhost:8770

Three sources: a TILES board over USB (Alpha 6.8 or later to tune; 6.7
streams), a simulated key (sim.py, no hardware), or a saved capture. Every
row goes through firmware/test/bend_replay.c --pipe, the firmware's own
bend_core.c and tune.c built for this computer, so the page shows LEGACY,
COSINE and PATH side by side with the board's live tuning.

Everything the page shows is also at http://localhost:8770/api/state (JSON),
and calibration reports and captures are saved in the repo
(firmware/test/calibration/, firmware/test/captures/). See README.md."""

import collections
import datetime
import json
import os
import queue
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import analysis as A
import sim

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
FIRMWARE = REPO / "firmware"
CAPTURES = FIRMWARE / "test" / "captures"
REPORTS = FIRMWARE / "test" / "calibration"
SERVICES = FIRMWARE / "src" / "services"
REPLAY_SOURCES = [FIRMWARE / "test" / "bend_replay.c", SERVICES / "bend_core.c", SERVICES / "tune.c",
                  SERVICES / "bend_core.h", SERVICES / "tune.h"]
REPLAY_BIN = HERE / ".build" / "bend_replay"
PORT = int(os.environ.get("TILES_STUDIO_PORT", "8770"))
COLUMNS = "t_ms,x,y,z,depth,smoothed_depth,bend,flags,state,velocity,aftertouch,strike_ms,peak_depth"
KEEP_ROWS = 4000  # "save" writes the last ~8 s
SETTING_DEFAULTS = {"expression.pitch_bend_mode": "path", "expression.pitch_bend_sensitivity": 0.065,
                    "expression.aftertouch_sensitivity": 1450}


def build_replay():
    """Builds bend_replay for this computer when it's missing or older than its sources."""
    if REPLAY_BIN.exists() and all(REPLAY_BIN.stat().st_mtime >= p.stat().st_mtime for p in REPLAY_SOURCES):
        return
    REPLAY_BIN.parent.mkdir(exist_ok=True)
    subprocess.run(["cc", "-std=c11", "-O2", "-ffp-contract=off", f"-I{SERVICES}"] +
                   [str(p) for p in REPLAY_SOURCES if p.suffix == ".c"] + ["-lm", "-o", str(REPLAY_BIN)], check=True)


def kv(line):
    return dict(w.split("=", 1) for w in line.split() if "=" in w)


def tune_row(line):
    d = kv(line)
    return {"key": d["key"], "value": float(d["value"]), "default": float(d["default"]), "min": float(d["min"]),
            "max": float(d["max"]), "group": d["group"]}


def host_tune_table():
    out = subprocess.run([str(REPLAY_BIN), "--tune-list"], capture_output=True, text=True, check=True).stdout
    return [tune_row(l) for l in out.splitlines() if l.startswith("key=")]


def board_present():
    """A TILES on USB right now (the studio then starts on it)."""
    try:
        import usb.core
        return any(usb.core.find(idVendor=v, idProduct=p) for v, p in ((0x1209, 0x0001), (0x2E8A, 0x100A)))
    except Exception:
        return False


def number(text):
    if text == "":
        return None
    value = float(text)
    return int(value) if value.is_integer() and "." not in text else value


class Replay:
    """bend_replay --pipe: one trace row back for every capture row in."""

    def __init__(self):
        self.proc = subprocess.Popen([str(REPLAY_BIN), "--pipe"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     text=True, bufsize=1)
        self.proc.stdout.readline()  # its header

    def setup(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def row(self, line):
        self.setup(line)
        return self.proc.stdout.readline().strip()

    def close(self):
        self.proc.kill()


def sample(row, trace, truth):
    """One row for the page and the analysis (analysis.py's columns)."""
    out = [number(v) for v in row.split(",")]
    out += [0] * (13 - len(out))  # older captures stop earlier
    out += [number(v) for v in trace.split(",")[5:]]
    out += list(truth) if truth else [None, None]
    return out


class DeviceError(Exception):
    pass


class DeviceSource:
    kind = "device"

    def __init__(self, pad):
        self.pad = pad
        self.session = None
        self.next = 0
        self.last_try = 0.0
        self.last = {"hall": 0.0, "paths": 0.0, "info": 0.0}
        self.fast = False
        self.info = {}
        self.settings = {}
        self.tune = None  # the board's TUNE table, or None if it can't tune
        self.message = "Looking for TILES..."
        self.setup = ""

    def _command(self, line, multi=False):
        """tiles_control's Session without its 50 ms drain per command (done once at connect)."""
        try:
            self.session.ep_out.write((line + "\n").encode("ascii"), timeout=2000)
            lines = []
            while True:
                text = self.session.read_line(line)
                if multi and text == "OK":
                    return lines
                lines.append(text)
                if text.startswith("ERR ") or not multi:
                    return lines
        except SystemExit:
            raise DeviceError("TILES stopped answering. Unplugged?")
        except Exception as exc:  # usb.core.USBError and friends
            raise DeviceError(f"USB: {exc}")

    def _connect(self):
        sys.path.insert(0, str(REPO / "tools"))
        try:
            import tiles_control
        except SystemExit:
            raise DeviceError("pyusb is missing: run the studio with ~/.venvs/tiles-tools/bin/python")
        try:
            self.session = tiles_control.Session(*tiles_control.find_vendor_endpoints())
        except SystemExit:
            raise DeviceError("No TILES found on USB. Plug one in, or try the simulated key.")
        self.session.drain()
        self._read_info()
        rows = self._command("TUNE LIST", multi=True)
        self.tune = None if rows and rows[0].startswith("ERR") else [tune_row(r) for r in rows]
        self._read_settings()

    def _read_info(self):
        self.info = kv(" ".join(self._command("INFO", multi=True)))

    def _read_settings(self):
        for key in SETTING_DEFAULTS:
            reply = self._command(f"GET {key}")
            if reply and not reply[0].startswith("ERR"):
                value = reply[0].split("=", 1)[-1]
                self.settings[key] = value if key.endswith("_mode") else float(value)

    def _watch(self):
        reply = self._command(f"CAPTURE WATCH {self.pad}")
        if not reply or reply[0] != "OK":
            raise DeviceError(f"This firmware ({self.info.get('firmware', '?')}) can't stream. "
                              "Flash Alpha 6.8 or later.")
        status = self._command("CAPTURE STATUS", multi=True)
        self.next = 0
        self.setup = "# " + next((s for s in status if s.startswith("bend ")), "bend mode=legacy")
        return self.setup

    def status(self):
        connected = self.session is not None
        return {"source": self.kind, "connected": connected, "pad": self.pad,
                "firmware": self.info.get("firmware", ""), "unit": self.info.get("unit", ""),
                "can_tune": self.tune is not None, "message": "" if connected else self.message}

    def _fail(self, exc):
        self.session = None
        self.message = str(exc)
        return {"status": self.status()}

    def poll(self):
        lines, events = [], {}
        now = time.monotonic()
        try:
            if self.session is None:
                if now - self.last_try < 2.0:
                    time.sleep(0.05)
                    return lines, events
                self.last_try = now
                self._connect()
                lines.append(self._watch())
                events.update({"status": self.status(), "settings": dict(self.settings)})
                if self.tune is not None:
                    events["tune"] = self.tune
            reply = self._command(f"CAPTURE READ {self.next}", multi=True)
            if reply and reply[0].startswith("seq="):
                first = int(reply[0][4:])
                rows = reply[1:]
                if first > self.next and self.next > 0:
                    lines.append(self._watch())  # rows were lost: start over so the replay matches again
                    events["notice"] = "Fell behind the board; restarted the stream."
                else:
                    lines += rows
                    self.next = first + len(rows)
            if now - self.last["hall"] > (0.1 if self.fast else 0.25):
                self.last["hall"] = now
                events["grid"] = [[number(v) for v in r.split(",")] for r in self._command("HALL", multi=True)]
            if now - self.last["paths"] > 2.0:
                self.last["paths"] = now
                events["paths"] = [[int(d["pad"]), float(d["bx"]), float(d["by"]), int(d["strokes"])]
                                   for d in map(kv, self._command("BEND PATHS", multi=True))]
            if now - self.last["info"] > 3.0:
                self.last["info"] = now
                self._read_info()
                events["board"] = self.info
        except DeviceError as exc:
            events.update(self._fail(exc))
        return lines, events

    def control(self, msg):
        if self.session is None:
            return [], {}
        try:
            if "pad" in msg:
                self.pad = int(msg["pad"])
                return [self._watch()], {"status": self.status()}
        except DeviceError as exc:
            return [], self._fail(exc)
        return [], {}

    def tune_set(self, key, value):
        if self.session is None or self.tune is None:
            return "this board can't tune (Alpha 6.8 or later)"
        reply = self._command(f"TUNE SET {key} {value:g}")
        return None if reply == ["OK"] else reply[0]

    def tune_reset(self, key):
        if self.session is None or self.tune is None:
            return "this board can't tune"
        reply = self._command("TUNE RESET" + (f" {key}" if key else ""))
        if reply != ["OK"]:
            return reply[0]
        self.tune = [tune_row(r) for r in self._command("TUNE LIST", multi=True)]  # one reset can reset all
        return None

    def setting_set(self, key, value):
        """A saved setting: changes the board for good (flash), and restarts the stream so the replay starts
        from the board's new setup."""
        if self.session is None:
            return "no board", []
        reply = self._command(f"SET {key} {value}")
        if reply != ["OK"]:
            return reply[0], []
        self._read_settings()
        return None, [self._watch()]

    def reset_paths(self):
        if self.session is not None:
            self._command("BEND RESET")
            return [self._watch()]
        return []

    def fast_hall(self, on):
        self.fast = on

    def play_sequence(self, step):
        pass

    def close(self):
        if self.session is not None:
            try:
                self._command("CAPTURE STOP")
            except DeviceError:
                pass


class SimSource:
    kind = "sim"

    def __init__(self, pad):
        self.pad = pad
        self.sim = sim.Sim()
        self.t0 = time.monotonic()
        self.ticks = 0
        self.started = False
        self.settings = dict(SETTING_DEFAULTS)
        self.path = ("0", "0", "0")

    def setup_line(self):
        return (f"# bend mode={self.settings['expression.pitch_bend_mode']} "
                f"sensitivity={self.settings['expression.pitch_bend_sensitivity']} "
                f"bx={self.path[0]} by={self.path[1]} strokes={self.path[2]}")

    def status(self):
        return {"source": self.kind, "connected": True, "pad": self.pad, "unit": "sim", "can_tune": True,
                "params": self.sim.params, "message": ""}

    def poll(self):
        time.sleep(0.01)
        lines = []
        if not self.started:
            self.started = True
            lines.append(self.setup_line())
        due = int((time.monotonic() - self.t0) * 1000 / sim.TICK_MS)
        if due - self.ticks > 500:  # the computer slept: don't replay a backlog
            self.ticks = due - 500
        while self.ticks < due:
            self.ticks += 1
            row = self.sim.step()
            if row:
                lines.append((row, self.sim.truth))
        last = next((l for l in reversed(lines) if isinstance(l, tuple)), None)
        if last:
            v = last[0].split(",")
            grid = [[p, 0, 0, 0, 0, 0, 0] for p in range(1, 25)]  # only the watched pad is simulated
            grid[self.pad - 1] = [self.pad, int(v[1]), int(v[2]), int(v[3]), int(v[4]), 1,
                                  1 if int(v[7]) & sim.TOUCHED else 0]
            return lines, {"grid": grid}
        return lines, {}

    def control(self, msg):
        if "sim" in msg:
            self.sim.set_params(msg["sim"])
        if "gesture" in msg:
            self.sim.play(msg["gesture"])
        if "manual" in msg:
            m = msg["manual"]
            self.sim.manual = (bool(m.get("on")), float(m.get("press", 0)), float(m.get("tilt", 0)), 0.0)
        if "pad" in msg:
            self.pad = int(msg["pad"])
        return [], ({"status": self.status()} if ("sim" in msg or "pad" in msg) else {})

    def tune_set(self, key, value):
        self.sim.tune[key] = value
        return None

    def tune_reset(self, key):
        if key:
            self.sim.tune.pop(key, None)
        else:
            self.sim.tune.clear()
        return None

    def setting_set(self, key, value):
        if key not in self.settings:
            return "unknown setting", []
        self.settings[key] = value if key.endswith("_mode") else float(value)
        self.sim.tune[key] = self.settings[key]
        return None, [self.setup_line()]

    def reset_paths(self):
        self.path = ("0", "0", "0")
        return [self.setup_line()]

    def fast_hall(self, on):
        pass

    def play_sequence(self, step):
        self.sim.play_sequence(step)

    def close(self):
        pass


class FileSource:
    kind = "file"

    def __init__(self, path, pad):
        self.path = Path(path)
        self.pad = pad
        self.setup = []
        self.tune_lines = []
        self.rows = []
        self.message = ""
        try:
            for line in self.path.read_text().splitlines():
                if line.startswith("# bend"):
                    self.setup.append(line)
                elif line.startswith("# tune"):
                    self.tune_lines.append(line)
                elif line and line[0].isdigit():
                    self.rows.append(line)
        except OSError as exc:
            self.message = f"Can't read {self.path.name}: {exc.strerror}"
        self.index = 0
        self.speed = 1.0
        self.playing = True
        self.clock = None  # (wall time, capture ms) at the last restart
        self.settings = dict(SETTING_DEFAULTS)
        for line in self.setup:
            d = kv(line)
            if "mode" in d:
                self.settings["expression.pitch_bend_mode"] = d["mode"]
            if "sensitivity" in d:
                self.settings["expression.pitch_bend_sensitivity"] = float(d["sensitivity"])

    def status(self):
        return {"source": self.kind, "connected": bool(self.rows), "pad": self.pad, "file": self.path.name,
                "unit": "file", "rows": len(self.rows), "playing": self.playing, "speed": self.speed,
                "can_tune": True, "message": self.message or ("" if self.rows else "No rows in that file.")}

    def _t(self, i):
        return int(self.rows[i].split(",", 1)[0])

    def poll(self):
        time.sleep(0.01)
        if not self.rows or not self.playing:
            return [], {}
        lines = []
        if self.index == 0:
            lines += self.setup or ["# bend mode=legacy"]
            self.clock = (time.monotonic(), self._t(0))
        wall0, t0 = self.clock
        now_ms = t0 + (time.monotonic() - wall0) * 1000 * self.speed
        while self.index < len(self.rows) and self._t(self.index) <= now_ms:
            lines.append(self.rows[self.index])
            self.index += 1
        if self.index >= len(self.rows):
            self.index = 0  # loop
        return lines, {}

    def control(self, msg):
        if "playing" in msg:
            self.playing = bool(msg["playing"])
        if "speed" in msg:
            self.speed = float(msg["speed"])
        if self.clock and self.index:
            self.clock = (time.monotonic(), self._t(self.index))
        return [], {"status": self.status()} if ("playing" in msg or "speed" in msg) else {}

    def tune_set(self, key, value):
        return None  # what-if: only the replay changes

    def tune_reset(self, key):
        return None

    def setting_set(self, key, value):
        if key not in self.settings:
            return "unknown setting", []
        self.settings[key] = value if key.endswith("_mode") else float(value)
        return None, []

    def reset_paths(self):
        return []

    def fast_hall(self, on):
        pass

    def play_sequence(self, step):
        pass

    def close(self):
        pass


class Hub:
    def __init__(self):
        self.lock = threading.Lock()
        self.clients = []
        self.last = {}

    def subscribe(self):
        q = queue.Queue(maxsize=400)
        with self.lock:
            self.clients.append(q)
            for message in self.last.values():
                q.put_nowait(message)
        return q

    def unsubscribe(self, q):
        with self.lock:
            if q in self.clients:
                self.clients.remove(q)

    def publish(self, event, data):
        message = f"event: {event}\ndata: {json.dumps(data, separators=(',', ':'))}\n\n"
        with self.lock:
            if event in ("status", "grid", "state"):
                self.last[event] = message
            for q in self.clients:
                try:
                    q.put_nowait(message)
                except queue.Full:
                    pass  # a stalled page misses frames rather than stalling the studio


class Pipeline(threading.Thread):
    """Source rows -> bend_replay -> analysis and the page, on one thread (USB calls included)."""

    def __init__(self, hub):
        super().__init__(daemon=True)
        self.hub = hub
        self.controls = queue.Queue()
        self.host_tune = host_tune_table()
        self.state_json = "{}"
        self.source = DeviceSource(1) if board_present() else SimSource(1)
        self._fresh()

    def _fresh(self):
        """Everything learned from one source; a new source starts over."""
        self.replay = Replay()
        self.setup = ""
        self.path_state = None
        self.recent = collections.deque(maxlen=KEEP_ROWS)
        self.pads = {p: A.PadStats(p) for p in range(1, 25)}
        self.notes = A.Notes()
        self.cal = A.Calibration()
        self.tune = [dict(r) for r in self.host_tune]
        self.settings = dict(getattr(self.source, "settings", SETTING_DEFAULTS))
        self.board = {}
        self.files = []
        self.step_samples, self.step_rows = [], []
        self.last_sample = None

    def control(self, msg):
        self.controls.put(msg)

    # ---- tuning and settings ----
    def tune_values(self):
        values = {r["key"]: r["value"] for r in self.tune}
        values.update({k: v for k, v in self.settings.items() if not k.endswith("_mode")})
        return values

    def _tune_set(self, key, value):
        row = next((r for r in self.tune if r["key"] == key), None)
        if row is None:
            return f"unknown key {key}"
        value = float(value)
        if not row["min"] <= value <= row["max"]:
            return f"{key} must be {row['min']:g} to {row['max']:g}"
        error = self.source.tune_set(key, value)
        if error:
            return f"{key}: {error}"
        row["value"] = value
        self.replay.setup(f"# tune {key}={value:g}")
        return None

    def _tune_reset(self, key):
        error = self.source.tune_reset(key)
        if error:
            return error
        if isinstance(self.source, DeviceSource):
            self.tune = [dict(r) for r in self.source.tune]
        for r in self.tune:
            if key in (None, r["key"]):
                r["value"] = r["default"]
        self.replay.setup("# tune reset " + " ".join(f"{r['key']}={r['value']:g}" for r in self.tune
                                                    if r["value"] != r["default"]))
        return None

    def _setting_set(self, key, value):
        error, lines = self.source.setting_set(key, value)
        if error:
            return f"{key}: {error}"
        self.settings.update(getattr(self.source, "settings", {}))
        self.settings[key] = value if key.endswith("_mode") else float(value)
        self._feed(lines, [])
        return None

    def _apply_board_tune(self, rows):
        """The board's live tuning into the table and the replay, so both match it."""
        self.tune = [dict(r) for r in rows]
        self.replay.setup("# tune reset " + " ".join(f"{r['key']}={r['value']:g}" for r in self.tune
                                                    if r["value"] != r["default"]))

    # ---- files ----
    def _unit(self):
        unit = str(self.source.status().get("unit", "")).split("/")[0]
        return f"unit{unit}" if unit.isdigit() else unit or "unknown"

    def _write_capture(self, rows, name):
        """Rows from the first idle one on, with `# bend` (the learned path there) and `# tune` lines, so
        bend_replay starts where we did."""
        start = next((i for i, (row, _) in enumerate(rows) if row.split(",")[8] == "0"), 0)
        rows = rows[start:]
        setup = self.setup
        if rows and rows[0][1] and setup:
            words = [w for w in setup.split() if not w.startswith(("bx=", "by=", "strokes="))]
            bx, by, strokes = rows[0][1]
            setup = " ".join(words + [f"bx={bx}", f"by={by}", f"strokes={strokes}"])
        changed = " ".join(f"{r['key']}={r['value']:g}" for r in self.tune if r["value"] != r["default"])
        CAPTURES.mkdir(parents=True, exist_ok=True)
        with open(CAPTURES / name, "w") as f:
            if setup:
                f.write(setup + "\n")
            if changed:
                f.write(f"# tune {changed}\n")
            f.write(COLUMNS + "\n")
            f.writelines(row + "\n" for row, _ in rows)
        path = f"firmware/test/captures/{name}"
        self.files.append(path)
        return path

    def _save_recent(self):
        stamp = time.strftime("%Y%m%d-%H%M%S")
        return self._write_capture(list(self.recent), f"studio-{self._unit()}-pad{self.source.pad}-{stamp}.csv")

    def _save_report(self):
        state = self.state()
        stamp = time.strftime("%Y%m%d-%H%M%S")
        base = f"{self._unit()}-{stamp}"
        REPORTS.mkdir(parents=True, exist_ok=True)
        (REPORTS / f"{base}.json").write_text(json.dumps(state, indent=1, default=str) + "\n")
        (REPORTS / f"{base}.md").write_text(report_markdown(state))
        paths = [f"firmware/test/calibration/{base}.md", f"firmware/test/calibration/{base}.json"]
        self.files += paths
        return paths[0]

    # ---- calibration ----
    def _calibrate(self, action, msg):
        cal = self.cal
        if action == "start":
            cal.reset()
            cal.active = True
        elif action == "stop":
            cal.active = cal.recording = False
            self.source.fast_hall(False)
        elif action == "goto":
            cal.index, cal.recording = int(msg.get("index", 0)), False
        elif action == "next":
            cal.index, cal.recording = cal.index + 1, False
            if cal.index >= len(A.STEPS):
                self.hub.publish("notice", f"Calibration done. Report: {self._save_report()}")
                cal.active = False
        elif action in ("record", "redo") and cal.step():
            step = cal.step()
            cal.recording, cal.started = True, time.monotonic()
            self.step_samples, self.step_rows = [], []
            if step["id"] == "rest":
                for p in self.pads.values():
                    p.rest.clear()
                self.source.fast_hall(True)
            else:
                self.source.play_sequence(step["id"])

    def _finish_step(self):
        cal, step = self.cal, self.cal.step()
        cal.recording = False
        self.source.fast_hall(False)
        tune, mode = self.tune_values(), self.settings.get("expression.pitch_bend_mode")
        samples = self.step_samples
        if step["id"] == "rest":
            res, sugg = A.analyse_rest(self.pads)
        else:
            t0 = samples[0][A.T] if samples else 0
            notes = [n for n in self.notes.notes if n["t_ms"] >= t0]
            if step["id"] == "strikes":
                res, sugg = A.analyse_strikes(notes, tune)
            elif step["id"] == "straight":
                res, sugg = A.analyse_straight(samples, tune, self.settings, mode)
            elif step["id"] == "lean":
                res, sugg = A.analyse_lean(samples, tune, self.settings, mode)
            else:
                res, sugg = A.analyse_vibrato(samples, tune, mode)
            if self.step_rows:
                stamp = time.strftime("%Y%m%d-%H%M%S")
                res["capture"] = self._write_capture(
                    self.step_rows, f"{self._unit()}-pad{self.source.pad}-{step['id']}-{stamp}.csv")
        cal.results[step["id"]] = res
        cal.suggestions[step["id"]] = sugg

    def _apply(self, keys):
        A.reconcile(self.cal.suggestions, self.tune_values(), self.settings)
        errors = []
        for sid, suggestions in self.cal.suggestions.items():
            for s in suggestions:
                if (keys != "all" and s["key"] not in keys) or s.get("blocked"):
                    continue
                if s["kind"] == "tune":
                    error = self._tune_set(s["key"], s["suggested"])
                else:
                    error = self._setting_set(s["key"], s["suggested"])
                if error:
                    errors.append(error)
                else:
                    s["applied"] = True
        return errors

    # ---- the loop ----
    def _switch(self, msg):
        pad = int(msg.get("pad", self.source.pad))
        self.source.close()
        kind = msg["source"]
        if kind == "device":
            self.source = DeviceSource(pad)
        elif kind == "file":
            self.source = FileSource(CAPTURES / Path(msg.get("file", "")).name, pad)
        else:
            self.source = SimSource(pad)
        self.replay.close()
        self._fresh()
        if kind == "file":
            for line in self.source.tune_lines:  # the tuning it was recorded with
                for key, value in kv(line[6:]).items():
                    self._tune_set(key, value)
        self.hub.publish("reset", {})
        status = self.source.status() if kind != "device" else {"source": "device", "connected": False,
                                                                  "pad": pad, "message": "Looking for TILES..."}
        self.hub.publish("status", status)

    def _handle(self, msg, batch):
        error = None
        if "source" in msg:
            self._switch(msg)
        elif msg.get("save"):
            self.hub.publish("notice", f"Saved {self._save_recent()}")
        elif msg.get("report"):
            self.hub.publish("notice", f"Saved {self._save_report()}")
        elif "tune" in msg:
            error = self._tune_set(msg["tune"]["key"], msg["tune"]["value"])
        elif "tune_reset" in msg:
            error = self._tune_reset(msg["tune_reset"])
        elif "setting" in msg:
            error = self._setting_set(msg["setting"]["key"], msg["setting"]["value"])
        elif msg.get("reset_paths"):
            self._feed(self.source.reset_paths(), batch)
        elif "calibrate" in msg:
            self._calibrate(msg["calibrate"], msg)
        elif "apply" in msg:
            errors = self._apply(msg["apply"])
            error = "; ".join(errors) if errors else None
        else:
            lines, events = self.source.control(msg)
            self._feed(lines, batch)
            self._events(events)
        if error:
            self.hub.publish("notice", error)

    def _events(self, events):
        for event, data in events.items():
            if event == "grid":
                key_up = self.tune_values().get("key.up_depth", 100)
                for r in data:
                    if len(r) >= 7 and r[5]:
                        self.pads[r[0]].add(r[1], r[2], r[3], r[4], bool(r[6]), key_up)
                    if len(r) >= 10:
                        p = self.pads[r[0]]
                        p.rest_z, p.rest_confirmed, p.sensor_ok = r[7], bool(r[8]), bool(r[9])
                self.hub.publish("grid", data)
            elif event == "paths":
                for pad, bx, by, n in data:
                    self.pads[pad].path = (bx, by, n)
            elif event == "board":
                self.board = data
            elif event == "settings":
                self.settings.update(data)
            elif event == "tune":
                self._apply_board_tune(data)
            else:
                self.hub.publish(event, data)
                if event == "status" and data.get("connected") and hasattr(self.source, "info"):
                    self.board = self.source.info

    def _feed(self, lines, batch):
        tune = None
        for line in lines:
            truth = None
            if isinstance(line, tuple):
                line, truth = line
            if line.startswith("#"):
                self.setup = line
                self.replay.setup(line)
                self.path_state = None
                continue
            trace = self.replay.row(line)
            try:
                s = sample(line, trace, truth)
            except ValueError:
                continue
            self.recent.append((line, self.path_state))  # with the learned path before this row
            self.path_state = tuple(trace.split(",")[-3:])
            if isinstance(self.source, SimSource):
                self.source.path = self.path_state
            batch.append(s)
            self.last_sample = s
            if tune is None:
                tune = self.tune_values()
            pad = self.pads[self.source.pad]
            pad.add(s[A.X], s[A.Y], s[A.Z], s[A.DEPTH], bool(s[A.FLAGS] & A.TOUCHED), tune.get("key.up_depth", 100))
            if not isinstance(self.source, DeviceSource) and s[A.STROKES] is not None:
                pad.path = (s[A.BX], s[A.BY], s[A.STROKES])
            self.notes.add(s, tune)
            if self.cal.recording:
                self.step_samples.append(s)
                self.step_rows.append(self.recent[-1])

    def state(self):
        status = self.source.status()
        return {
            "time": datetime.datetime.now().isoformat(timespec="seconds"),
            "source": status,
            "board": self.board,
            "settings": self.settings,
            "tune": self.tune,
            "tune_changed": {r["key"]: {"value": r["value"], "default": r["default"]}
                             for r in self.tune if r["value"] != r["default"]},
            "pads": [p.summary() for p in self.pads.values()],
            "pad": status.get("pad"),
            "notes": self.notes.public(12),
            "calibration": self.cal.public(status.get("pad"), self.tune_values(), self.settings),
            "files": self.files[-20:],
        }

    def run(self):
        self.hub.publish("status", self.source.status())
        batch, last_flush, last_state = [], time.monotonic(), 0.0
        while True:
            while not self.controls.empty():
                self._handle(self.controls.get(), batch)
            lines, events = self.source.poll()
            self._feed(lines, batch)
            self._events(events)
            now = time.monotonic()
            if self.cal.recording and now - self.cal.started >= self.cal.step()["seconds"]:
                self._finish_step()
                last_state = 0.0
            if now - last_flush > 0.033:
                if batch:
                    self.hub.publish("samples", batch)
                batch, last_flush = [], now
            if now - last_state > 0.5:
                last_state = now
                state = self.state()
                self.state_json = json.dumps(state, default=str)
                self.hub.publish("state", state)


def step_markdown(sid, r):
    """One calibration step's results in a few lines."""
    def mmm(d, unit=""):
        return f"{d['min']} / {d['median']} / {d['max']}{unit}" if d else "–"

    out = []
    if sid == "rest":
        out.append(f"{r['pads_measured']} pads measured. Rest field (Bz) median {r['rest_field_median_mt']} mT "
                   f"(range {r['rest_field_range_mt']}); Z noise median {r['z_noise_median_ut']} uT. X/Y are "
                   "read only while a key is touched.")
    elif sid == "strikes":
        out.append(f"{r['notes']} notes. Velocity {mmm(r.get('velocity'))}, touch to threshold "
                   f"{mmm(r.get('strike_ms'), ' ms')}, peak depth {mmm(r.get('peak_depth'))} (min / median / max).")
    elif sid in ("straight", "lean"):
        if sid == "lean" and "lean_median" in r:
            out.append(f"Lean peaks ({r['mode']} delta): + {r['leans']['+']}, - {r['leans']['-']}; median "
                       f"{r['lean_median']}" + (f", one side {r['left_right_ratio']}x the other." if "left_right_ratio" in r else "."))
        if sid == "straight":
            out.append(f"Hardest pressure depth {r.get('max_pressure_depth')}.")
        out += ["", "| Mode | Peak bend (st) | 95th / 99th pct delta | Time bent |", "|---|---|---|---|"]
        for mode, b in r.get("bend", {}).items():
            out.append(f"| {mode} | {b['peak_semitones']} | {b['delta_p95']} / {b['delta_p99']} | {b['time_bent']} |")
    elif sid == "vibrato" and "wiggle_energy_p75" in r:
        out.append(f"Vibrato energy ({r['mode']}): wiggle {r['wiggle_energy_p75']} (75th pct) vs holding still "
                   f"{r['still_energy_p95']} (95th pct).")
    if r.get("flags") or r.get("capture"):
        out.append("")
    out += [f"- {flag}" for flag in r.get("flags", [])]
    if r.get("capture"):
        out.append(f"- capture: `{r['capture']}`")
    return out


def report_markdown(s):
    """The calibration report, for people: what was measured, what to change."""
    src = s["source"]
    lines = [f"# TILES calibration: {src.get('unit') or src.get('source')}, {s['time'].replace('T', ' ')}", ""]
    board = s.get("board") or {}
    lines += [f"Source: {src.get('source')}" + (f", firmware {board.get('firmware')}" if board.get("firmware") else "")
              + f", pad {s.get('pad')} watched. Every number: the .json next to this file.", ""]
    if board:
        lines += ["## Board", ""]
        for key in ("loop.avg_us", "loop.max_us", "hall.rezeros", "hall.unconfirmed", "hall.reinits", "hall.dead",
                    "touch.recoveries", "power", "tune.changed"):
            if key in board:
                lines.append(f"- {key}: {board[key]}")
        lines.append("")
    pads = [p for p in s["pads"] if p.get("rest_field_mt") is not None]
    if pads:
        lines += ["## Pads at rest", "", "| Pad | Rest Bz (mT) | Z noise (uT) | Hardest press (x rest) | Press path b (x, y) | Strokes |",
                  "|---|---|---|---|---|---|"]
        for p in pads:
            path = p.get("path") or {}
            lines.append(f"| {p['pad']} | {p['rest_field_mt']} | {p['z_noise_ut']} | {p.get('hardest_press_ratio', '')} | "
                         f"{path.get('bx', '')}, {path.get('by', '')} | {path.get('strokes', '')} |")
        lines.append("")
    cal = s["calibration"]
    if cal["results"]:
        lines += ["## Steps", ""]
        for step in cal["steps"]:
            res = cal["results"].get(step["id"])
            if res is not None:
                lines += [f"### {step['title']}", ""] + step_markdown(step["id"], res) + [""]
    if cal["suggestions"]:
        lines += ["## Suggestions", "", "| Key | Now | Suggested | Why | Applied |", "|---|---|---|---|---|"]
        for x in cal["suggestions"]:
            why = x["why"] + (f" (held back: {x['blocked']})" if x.get("blocked") else "")
            lines.append(f"| `{x['key']}` | {x['current']} | {x['suggested']} | {why} | "
                         f"{'yes' if x.get('applied') else 'no'} |")
        lines.append("")
    if s["tune_changed"]:
        lines += ["## Live tuning in effect (lost at power-off)", ""]
        lines += [f"- `{k}` = {v['value']:g} (default {v['default']:g})" for k, v in s["tune_changed"].items()]
        lines.append("")
    lines += ["## Settings", ""] + [f"- `{k}` = {v}" for k, v in s["settings"].items()] + [""]
    if s["files"]:
        lines += ["## Files", ""] + [f"- {f}" for f in s["files"]] + [""]
    return "\n".join(lines)


class Handler(BaseHTTPRequestHandler):
    hub = None
    pipeline = None

    def log_message(self, *args):
        pass

    def _send(self, code, body, kind):
        self.send_response(code)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path in ("/", "/index.html"):
            self._send(200, (HERE / "index.html").read_bytes(), "text/html; charset=utf-8")
        elif self.path == "/api/state":
            self._send(200, self.pipeline.state_json.encode(), "application/json")
        elif self.path in ("/api/notes", "/api/tune", "/api/pads", "/api/calibration"):
            part = self.path.rsplit("/", 1)[1]
            self._send(200, json.dumps(json.loads(self.pipeline.state_json).get(part)).encode(), "application/json")
        elif self.path == "/api/captures":
            files = sorted(p.name for p in CAPTURES.glob("*.csv")) if CAPTURES.exists() else []
            self._send(200, json.dumps(files).encode(), "application/json")
        elif self.path == "/api/events":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            q = self.hub.subscribe()
            try:
                while True:
                    try:
                        message = q.get(timeout=10)
                    except queue.Empty:
                        message = ": still here\n\n"
                    self.wfile.write(message.encode())
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass
            finally:
                self.hub.unsubscribe(q)
        else:
            self._send(404, b"not found", "text/plain")

    def do_POST(self):
        if self.path != "/api/control":
            self._send(404, b"not found", "text/plain")
            return
        length = int(self.headers.get("Content-Length", "0"))
        try:
            msg = json.loads(self.rfile.read(length) or b"{}")
        except json.JSONDecodeError:
            self._send(400, b'{"ok":false}', "application/json")
            return
        self.pipeline.control(msg)
        self._send(200, b'{"ok":true}', "application/json")


def main():
    build_replay()
    hub = Hub()
    pipeline = Pipeline(hub)
    pipeline.start()
    Handler.hub, Handler.pipeline = hub, pipeline
    server = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    server.daemon_threads = True
    print(f"TILES calibration studio: http://localhost:{PORT}  (state: /api/state; Ctrl+C to stop)")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
