#!/usr/bin/env python3
"""TILES calibration studio: what one key's Hall sensor reads, and what the
pitch bend code makes of it in every bend mode, live in a browser.

    ~/.venvs/tiles-tools/bin/python tools/studio/studio.py      # then open http://localhost:8770

Three sources: a TILES board over USB (Alpha 6.7 or later: CAPTURE WATCH),
a simulated key (sim.py, no hardware), or a saved capture. Every row goes
through firmware/test/bend_replay.c --pipe, the firmware's own bend_core.c
built for this computer, so the page shows LEGACY, COSINE and PATH side by
side whichever mode the board plays. See tools/studio/README.md."""

import collections
import json
import os
import queue
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import sim

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
FIRMWARE = REPO / "firmware"
CAPTURES = FIRMWARE / "test" / "captures"
REPLAY_SOURCES = [FIRMWARE / "test" / "bend_replay.c", FIRMWARE / "src" / "services" / "bend_core.c",
                  FIRMWARE / "src" / "services" / "bend_core.h"]
REPLAY_BIN = HERE / ".build" / "bend_replay"
PORT = int(os.environ.get("TILES_STUDIO_PORT", "8770"))
COLUMNS = "t_ms,x,y,z,depth,smoothed_depth,bend,flags,state,velocity,aftertouch"
KEEP_ROWS = 4000  # "save" writes the last ~8 s


def build_replay():
    """Builds bend_replay for this computer when it's missing or older than its sources."""
    if REPLAY_BIN.exists() and all(REPLAY_BIN.stat().st_mtime >= p.stat().st_mtime for p in REPLAY_SOURCES):
        return
    REPLAY_BIN.parent.mkdir(exist_ok=True)
    subprocess.run(["cc", "-std=c11", "-O2", f"-I{FIRMWARE / 'src' / 'services'}", str(REPLAY_SOURCES[0]),
                    str(REPLAY_SOURCES[1]), "-lm", "-o", str(REPLAY_BIN)], check=True)


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
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        return self.proc.stdout.readline().strip()

    def close(self):
        self.proc.kill()


def number(text):
    if text == "":
        return None
    value = float(text)
    return int(value) if value.is_integer() and "." not in text else value


def sample(row, trace, truth):
    """One row for the page: the 11 capture columns, then delta and semitones for legacy, cosine and path,
    the learned slope and strokes, and (sim only) the true press and lean."""
    out = [number(v) for v in row.split(",")]
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
        self.last_hall = 0.0
        self.last_paths = 0.0
        self.firmware = ""
        self.message = "Looking for TILES..."

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
                if not multi or text.startswith("ERR "):
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
        info = " ".join(self._command("INFO", multi=True))
        self.firmware = next((w.split("=", 1)[1] for w in info.split() if w.startswith("firmware=")), "")

    def _watch(self):
        reply = self._command(f"CAPTURE WATCH {self.pad}")
        if not reply or reply[0] != "OK":
            raise DeviceError(f"This firmware ({self.firmware or 'unknown'}) can't stream. Flash Alpha 6.7 or later.")
        status = self._command("CAPTURE STATUS", multi=True)
        self.next = 0
        setup = next((s for s in status if s.startswith("bend ")), "bend mode=legacy")
        self.setup = "# " + setup
        return self.setup

    def status(self):
        connected = self.session is not None
        settings = {}
        if connected:
            for key in ("expression.pitch_bend_mode", "expression.pitch_bend_sensitivity"):
                value = self._command(f"GET {key}")
                settings[key] = value[0] if value else ""
        return {"source": self.kind, "connected": connected, "pad": self.pad, "firmware": self.firmware,
                "mode": settings.get("expression.pitch_bend_mode", ""),
                "sensitivity": settings.get("expression.pitch_bend_sensitivity", ""),
                "message": "" if connected else self.message}

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
                events["status"] = self.status()
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
            if now - self.last_hall > 0.25:
                self.last_hall = now
                events["grid"] = [[number(v) for v in r.split(",")] for r in self._command("HALL", multi=True)]
            if now - self.last_paths > 2.0:
                self.last_paths = now
                paths = []
                for r in self._command("BEND PATHS", multi=True):
                    kv = dict(p.split("=") for p in r.split())
                    paths.append([int(kv["pad"]), float(kv["bx"]), float(kv["by"]), int(kv["strokes"])])
                events["paths"] = paths
        except DeviceError as exc:
            self.session = None
            self.message = str(exc)
            events["status"] = {"source": self.kind, "connected": False, "pad": self.pad, "message": self.message}
        return lines, events

    def control(self, msg):
        if self.session is None:
            return [], {}
        lines = []
        try:
            if "mode" in msg:
                self._command(f"SET expression.pitch_bend_mode {msg['mode']}")
            if "sensitivity" in msg:
                self._command(f"SET expression.pitch_bend_sensitivity {float(msg['sensitivity'])}")
            if msg.get("reset_paths"):
                self._command("BEND RESET")
            if "pad" in msg:
                self.pad = int(msg["pad"])
            lines.append(self._watch())
            return lines, {"status": self.status()}
        except DeviceError as exc:
            self.session = None
            self.message = str(exc)
            return lines, {"status": {"source": self.kind, "connected": False, "message": self.message}}

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

    def status(self):
        return {"source": self.kind, "connected": True, "pad": self.pad, "mode": "path", "sensitivity": "0.065",
                "params": self.sim.params, "message": ""}

    def poll(self):
        time.sleep(0.01)
        lines = []
        if not self.started:
            self.started = True
            lines.append(self.sim.setup_line())
        due = int((time.monotonic() - self.t0) * 1000 / sim.TICK_MS)
        due = max(due, self.ticks)
        if due - self.ticks > 500:  # the computer slept: don't replay a backlog
            self.ticks = due - 500
        while self.ticks < due:
            self.ticks += 1
            row = self.sim.step()
            if row:
                lines.append((row, self.sim.truth))
        grid = [[p, 0, 0, 0, 0, 1, 0] for p in range(1, 25)]
        last = next((l for l in reversed(lines) if isinstance(l, tuple)), None)
        if last:
            v = last[0].split(",")
            grid[self.pad - 1] = [self.pad, 0, 0, 0, int(v[4]), 1, 1 if int(v[7]) & sim.TOUCHED else 0]
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
        if msg.get("reset_paths"):
            return [self.sim.setup_line()], {"status": self.status()}
        if "pad" in msg:
            self.pad = int(msg["pad"])
        return [], {"status": self.status()} if ("sim" in msg or "pad" in msg) else {}

    def close(self):
        pass


class FileSource:
    kind = "file"

    def __init__(self, path, pad):
        self.path = Path(path)
        self.pad = pad
        self.setup = []
        self.rows = []
        self.message = ""
        try:
            for line in self.path.read_text().splitlines():
                if line.startswith("# bend"):
                    self.setup.append(line)
                elif line and line[0].isdigit():
                    self.rows.append(line)
        except OSError as exc:
            self.message = f"Can't read {self.path.name}: {exc.strerror}"
        self.index = 0
        self.speed = 1.0
        self.playing = True
        self.clock = None  # (wall time, capture ms) at the last restart

    def status(self):
        return {"source": self.kind, "connected": bool(self.rows), "pad": self.pad, "file": self.path.name,
                "rows": len(self.rows), "playing": self.playing, "speed": self.speed,
                "message": self.message or ("" if self.rows else "No rows in that file.")}

    def poll(self):
        time.sleep(0.01)
        if not self.rows or not self.playing:
            return [], {}
        lines = []
        if self.index == 0:
            lines += self.setup or ["# bend mode=legacy"]
            self.clock = (time.monotonic(), int(self.rows[0].split(",", 1)[0]))
        wall0, t0 = self.clock
        now_ms = t0 + (time.monotonic() - wall0) * 1000 * self.speed
        while self.index < len(self.rows) and int(self.rows[self.index].split(",", 1)[0]) <= now_ms:
            lines.append(self.rows[self.index])
            self.index += 1
        if self.index >= len(self.rows):
            self.index = 0  # loop
        return lines, {}

    def control(self, msg):
        if "playing" in msg:
            self.playing = bool(msg["playing"])
            if self.playing and self.clock and self.index:
                t = int(self.rows[self.index].split(",", 1)[0])
                self.clock = (time.monotonic(), t)
        if "speed" in msg:
            self.speed = float(msg["speed"])
            if self.clock and self.index:
                self.clock = (time.monotonic(), int(self.rows[self.index].split(",", 1)[0]))
        return [], {"status": self.status()}

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
            for event in ("status", "grid", "paths"):
                if event in self.last:
                    q.put_nowait(self.last[event])
        return q

    def unsubscribe(self, q):
        with self.lock:
            if q in self.clients:
                self.clients.remove(q)

    def publish(self, event, data):
        message = f"event: {event}\ndata: {json.dumps(data, separators=(',', ':'))}\n\n"
        with self.lock:
            if event in ("status", "grid", "paths"):
                self.last[event] = message
            for q in self.clients:
                try:
                    q.put_nowait(message)
                except queue.Full:
                    pass  # a stalled page misses frames rather than stalling the studio


class Pipeline(threading.Thread):
    """Source rows -> bend_replay -> the page, on one thread (USB calls included)."""

    def __init__(self, hub):
        super().__init__(daemon=True)
        self.hub = hub
        self.controls = queue.Queue()
        self.source = SimSource(1)
        self.replay = Replay()
        self.setup = ""
        self.path_state = None
        self.recent = collections.deque(maxlen=KEEP_ROWS)

    def control(self, msg):
        self.controls.put(msg)

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
        self.replay = Replay()
        self.recent.clear()
        self.hub.publish("reset", {})
        status = self.source.status() if kind != "device" else {"source": "device", "connected": False,
                                                                  "pad": pad, "message": "Looking for TILES..."}
        self.hub.publish("status", status)

    def _save(self):
        """Writes the last ~8 s from the first idle row on (no stroke or note half recorded), with a
        `# bend` line holding the learned path as it was there, so bend_replay starts where we did."""
        rows = list(self.recent)
        start = next((i for i, (row, _) in enumerate(rows) if row.split(",")[8] == "0"), 0)
        rows = rows[start:]
        setup = self.setup
        if rows and rows[0][1] and setup:
            words = [w for w in setup.split() if not w.startswith(("bx=", "by=", "strokes="))]
            bx, by, strokes = rows[0][1]
            setup = " ".join(words + [f"bx={bx}", f"by={by}", f"strokes={strokes}"])
        CAPTURES.mkdir(parents=True, exist_ok=True)
        name = f"studio-{self.source.kind}-pad{self.source.pad}-{time.strftime('%Y%m%d-%H%M%S')}.csv"
        with open(CAPTURES / name, "w") as f:
            if setup:
                f.write(setup + "\n")
            f.write(COLUMNS + "\n")
            f.writelines(row + "\n" for row, _ in rows)
        return name

    def _feed(self, lines, batch):
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
            self.recent.append((line, self.path_state))  # with the learned path before this row
            self.path_state = tuple(trace.split(",")[-3:])
            try:
                batch.append(sample(line, trace, truth))
            except ValueError:
                pass

    def run(self):
        self.hub.publish("status", self.source.status())
        batch, last_flush = [], time.monotonic()
        while True:
            while not self.controls.empty():
                msg = self.controls.get()
                if "source" in msg:
                    self._switch(msg)
                elif msg.get("save"):
                    self.hub.publish("notice", f"Saved firmware/test/captures/{self._save()}")
                else:
                    lines, events = self.source.control(msg)
                    self._feed(lines, batch)
                    for event, data in events.items():
                        self.hub.publish(event, data)
            lines, events = self.source.poll()
            self._feed(lines, batch)
            for event, data in events.items():
                self.hub.publish(event, data)
            if time.monotonic() - last_flush > 0.033:
                if batch:
                    self.hub.publish("samples", batch)
                batch, last_flush = [], time.monotonic()


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
    print(f"TILES calibration studio: http://localhost:{PORT}  (Ctrl+C to stop)")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
