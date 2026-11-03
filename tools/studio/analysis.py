"""What the studio works out from the samples: every pad's rest field and
noise, the watched pad's notes (velocity broken down, bend per mode), and
the guided calibration's steps, results and suggestions. One state, read by
the page and by /api/state."""

import math
import statistics
import time

LSB_PER_MT = 409.6  # TMAG5273 at +/-80 mT
SEMI_PER_UNIT = 12 / (8191 * 12 / 48)
MODES = ("legacy", "cosine", "path")

# Sample columns: the capture row, then delta/semitones/vibrato per mode, the
# learned path, and (sim only) the true press and lean.
T, X, Y, Z, DEPTH, SD, BEND, FLAGS, STATE, VEL, AT, STRIKE_MS, PEAK = range(13)
MODE_COLS = {m: (13 + 3 * i, 14 + 3 * i, 15 + 3 * i) for i, m in enumerate(MODES)}
BX, BY, STROKES, TRUE_PRESS, TRUE_TILT = 22, 23, 24, 25, 26
SAMPLE_LEN = 27

VALID, TOUCHED, STARTED, STEPPED, BENT = 0x01, 0x02, 0x04, 0x08, 0x10
STROKE, STROKE_BEGIN, NOTE_ON, PRESSURE, NOTE_OFF = 0x20, 0x40, 0x80, 0x100, 0x200


def pct(values, q):
    if not values:
        return None
    s = sorted(values)
    return s[min(len(s) - 1, max(0, round(q * (len(s) - 1))))]


def rnd(v, digits=3):
    return None if v is None else round(v, digits)


def velocity_parts(tune, strike_ms, peak):
    """The firmware's velocity split into its two scores (expression.c), from the tuning in effect."""
    fast, slow = tune.get("velocity.time_fast_ms", 10), tune.get("velocity.time_slow_ms", 300)
    curve, weight = tune.get("velocity.curve", 1.0), tune.get("velocity.depth_weight", 0.85)
    strike, over = tune.get("strike.depth", 150), tune.get("strike.overshoot_full", 550)
    if strike_ms <= fast:
        time_score = 1.0
    elif strike_ms >= slow:
        time_score = 0.0
    else:
        time_score = ((slow - strike_ms) / (slow - fast)) ** curve
    depth_score = min(1.0, max(0.0, (peak - strike) / over))
    return {"time_score": round(time_score, 3), "depth_score": round(depth_score, 3),
            "depth_share": round(weight * depth_score, 3), "time_share": round((1 - weight) * time_score, 3)}


class PadStats:
    """One pad's resting field and noise (untouched reads), and what pressing it has shown. At rest the
    firmware reads Z only (hall.c), so X/Y there are stale: rest numbers are Z's."""

    def __init__(self, pad):
        self.pad = pad
        self.rest = []  # distinct Z while untouched and up
        self.max_b = 0.0
        self.rest_z = None
        self.rest_confirmed = None
        self.sensor_ok = None
        self.path = None  # (bx, by, strokes)

    def add(self, x, y, z, depth, touched, key_up):
        if not touched and depth < key_up:
            if not self.rest or self.rest[-1] != z:
                self.rest.append(z)
                if len(self.rest) > 120:
                    self.rest.pop(0)
        elif touched:
            self.max_b = max(self.max_b, math.sqrt(x * x + y * y + z * z))

    def summary(self):
        out = {"pad": self.pad, "samples": len(self.rest), "rest_z": self.rest_z,
               "rest_confirmed": self.rest_confirmed, "sensor_ok": self.sensor_ok}
        if len(self.rest) >= 8:
            rest_b = abs(statistics.fmean(self.rest))
            out.update({"rest_field_mt": rnd(rest_b / LSB_PER_MT, 2),
                        "z_noise_ut": round(statistics.stdev(self.rest) / LSB_PER_MT * 1000)})
            if self.max_b and rest_b:
                out["hardest_press_ratio"] = rnd(self.max_b / rest_b, 2)
        if self.path:
            out["path"] = {"bx": rnd(self.path[0], 4), "by": rnd(self.path[1], 4), "strokes": self.path[2]}
        return out


class Notes:
    """The watched pad's notes, from NOTE_ON to NOTE_OFF: velocity and its parts, pressure, and each mode's
    bend peak and vibrato energy."""

    def __init__(self):
        self.notes = []
        self.current = None

    def add(self, s, tune):
        f = s[FLAGS]
        if f & NOTE_ON or (f & STARTED and not f & NOTE_ON and (not self.current or self.current["ended"])):
            self.current = {"t_ms": s[T], "velocity": s[VEL], "strike_ms": s[STRIKE_MS], "peak_depth": s[PEAK],
                            "max_depth": s[DEPTH], "max_pressure": 0, "release_velocity": None, "ended": False,
                            "bend_peak": {m: 0.0 for m in MODES}, "delta_p95": {}, "vibrato_peak": {m: 0.0 for m in MODES},
                            "_deltas": {m: [] for m in MODES}, "steps": 0}
            if f & NOTE_ON and s[STRIKE_MS] is not None:
                self.current.update(velocity_parts(tune, s[STRIKE_MS], s[PEAK] or 0))
            self.notes.append(self.current)
            del self.notes[:-60]
        n = self.current
        if n is None or n["ended"]:
            return
        if f & (PRESSURE | STEPPED):
            n["steps"] += 1
            n["max_depth"] = max(n["max_depth"], s[DEPTH])
            n["max_pressure"] = max(n["max_pressure"], s[AT] or 0)
            for m, (d, semis, vib) in MODE_COLS.items():
                if s[semis] is not None and abs(s[semis]) > abs(n["bend_peak"][m]):
                    n["bend_peak"][m] = s[semis]
                if s[d] is not None:
                    n["_deltas"][m].append(abs(s[d]))
                if s[vib] is not None:
                    n["vibrato_peak"][m] = max(n["vibrato_peak"][m], s[vib])
        if f & NOTE_OFF:
            n["release_velocity"] = s[VEL]
            n["ended"] = True
            n["length_ms"] = s[T] - n["t_ms"]

    def public(self, count=20):
        out = []
        for n in self.notes[-count:]:
            d = {k: v for k, v in n.items() if not k.startswith("_")}
            d["delta_p95"] = {m: rnd(pct(v, 0.95), 4) for m, v in n["_deltas"].items()}
            d["bend_peak"] = {m: rnd(v, 2) for m, v in n["bend_peak"].items()}
            d["vibrato_peak"] = {m: rnd(v, 1) for m, v in n["vibrato_peak"].items()}
            out.append(d)
        return out


STEPS = [
    {"id": "rest", "title": "Hands off", "seconds": 4,
     "text": "Take your hands off TILES. This measures every pad's resting field and sensor noise."},
    {"id": "strikes", "title": "Strikes", "seconds": 12,
     "text": "Strike pad {pad} about 8 times, from the softest note you would play to the hardest."},
    {"id": "straight", "title": "Straight presses", "seconds": 12,
     "text": "Press pad {pad} straight down, slowly from light to as hard as you play and back, 3 times. "
             "Don't lean. Any bend here is pressure leaking into pitch."},
    {"id": "lean", "title": "Leans", "seconds": 12,
     "text": "Press pad {pad}, lean it to one side as far as you want a full bend, hold a second, come back, "
             "then the other side. Twice each way."},
    {"id": "vibrato", "title": "Vibrato", "seconds": 8,
     "text": "Press pad {pad} and wiggle it side to side like a vibrato for 4 seconds, then hold it still "
             "for 4 seconds."},
]


def suggestion(key, current, value, why, kind="tune"):
    return {"key": key, "current": current, "suggested": value, "why": why, "kind": kind}


def analyse_rest(pads):
    rows = [p.summary() for p in pads.values()]
    measured = [r for r in rows if r.get("rest_field_mt") is not None]
    fields = [r["rest_field_mt"] for r in measured]
    noise = [r["z_noise_ut"] for r in measured]
    med_field = statistics.median(fields) if fields else None
    med_noise = statistics.median(noise) if noise else None
    flags = []
    for r in rows:
        if r.get("sensor_ok") is False:
            flags.append(f"pad {r['pad']}: sensor not answering")
        elif r.get("rest_confirmed") is False:
            flags.append(f"pad {r['pad']}: rest not confirmed (touched while it was taken?)")
        if r.get("rest_field_mt") is not None and med_field:
            if not 0.6 <= r["rest_field_mt"] / med_field <= 1.6:
                flags.append(f"pad {r['pad']}: rest field {r['rest_field_mt']} mT vs median {med_field:.2f} "
                             "(magnet height or placement?)")
            if med_noise and r["z_noise_ut"] > 2 * med_noise:
                flags.append(f"pad {r['pad']}: Z noise {r['z_noise_ut']} uT, twice the median")
    if fields and max(fields) > 1.5 * min(fields):
        flags.append(f"rest fields span {min(fields)}-{max(fields)} mT: the same travel gives a weak pad far less "
                     "depth, so less velocity and pressure")
    return {"pads_measured": len(measured), "rest_field_median_mt": rnd(med_field, 2),
            "rest_field_range_mt": [min(fields), max(fields)] if fields else None,
            "z_noise_median_ut": med_noise, "flags": flags}, []


def analyse_strikes(notes, tune):
    if not notes:
        return {"notes": 0, "flags": ["no notes: strike harder than the strike depth?"]}, []
    vels = [n["velocity"] for n in notes]
    peaks = [n["peak_depth"] for n in notes if n["peak_depth"]]
    times = [n["strike_ms"] for n in notes if n["strike_ms"] is not None]
    res = {"notes": len(notes), "velocity": {"min": min(vels), "median": statistics.median(vels), "max": max(vels)},
           "strike_ms": {"min": min(times), "median": statistics.median(times), "max": max(times)} if times else None,
           "peak_depth": {"min": min(peaks), "median": statistics.median(peaks), "max": max(peaks)} if peaks else None,
           "flags": []}
    sugg = []
    strike, over = tune.get("strike.depth", 150), tune.get("strike.overshoot_full", 550)
    if peaks and len(notes) >= 4:
        want = round(max(100, pct(peaks, 0.9) - strike))
        if abs(want - over) / over > 0.15:
            sugg.append(suggestion("strike.overshoot_full", over, want,
                                   "your hardest strikes (90th percentile) should just reach full velocity"))
    if max(vels) < 110:
        res["flags"].append("the hardest strike stayed under 110")
    if sum(v >= 127 for v in vels) > len(vels) / 3:
        res["flags"].append("a third or more of the strikes hit 127")
    return res, sugg


def bend_stats(samples):
    """Per mode over held passes: peak |semitones|, 95th/99th percentile |delta|, share of time bent."""
    out = {}
    held = [s for s in samples if s[FLAGS] & (PRESSURE | STEPPED)]
    for m, (d, semis, _) in MODE_COLS.items():
        deltas = [abs(s[d]) for s in held if s[d] is not None]
        bends = [abs(s[semis]) for s in held if s[semis] is not None]
        out[m] = {"peak_semitones": rnd(max(bends), 2) if bends else None,
                  "delta_p95": rnd(pct(deltas, 0.95), 4), "delta_p99": rnd(pct(deltas, 0.99), 4),
                  "time_bent": rnd(sum(b > 0.05 for b in bends) / len(bends), 3) if bends else None}
    return out


def analyse_straight(samples, tune, settings, board_mode):
    stats = bend_stats(samples)
    depths = [s[SD] for s in samples if s[FLAGS] & PRESSURE and s[SD]]
    res = {"bend": stats, "max_pressure_depth": round(max(depths)) if depths else None, "flags": []}
    sugg = []
    mode = board_mode if board_mode in MODES else "path"
    leak = stats[mode]["delta_p99"]
    dead = tune.get("bend.deadzone", 0.025)
    if leak is not None:
        want = round(min(0.08, max(0.01, 1.25 * leak)), 3)
        if leak > dead * 0.9:
            sugg.append(suggestion("bend.deadzone", dead, want,
                                   f"straight presses read up to {leak:.3f} in {mode}: the deadzone should sit above that"))
        elif dead > 2 * want and dead > 0.015:
            sugg.append(suggestion("bend.deadzone", dead, max(want, 0.012),
                                   f"straight presses stay under {leak:.3f}: a smaller deadzone makes small bends easier"))
    best = min((m for m in MODES if stats[m]["peak_semitones"] is not None),
               key=lambda m: stats[m]["peak_semitones"], default=None)
    if best and board_mode and best != board_mode and stats[board_mode]["peak_semitones"] is not None:
        if stats[board_mode]["peak_semitones"] - stats[best]["peak_semitones"] > 0.25:
            sugg.append(suggestion("expression.pitch_bend_mode", board_mode, best,
                                   f"{best} leaked least on straight presses", kind="setting"))
    if depths:
        full = settings.get("expression.aftertouch_sensitivity")
        want = int(round(max(depths) * 0.95))
        if full and abs(want - full) / full > 0.15:
            sugg.append(suggestion("expression.aftertouch_sensitivity", full, want,
                                   "your hardest press should just reach full pressure (127)", kind="setting"))
    for m in MODES:
        if (stats[m]["peak_semitones"] or 0) > 0.5:
            res["flags"].append(f"{m} bent {stats[m]['peak_semitones']} semitones on straight presses")
    return res, sugg


def analyse_lean(samples, tune, settings, board_mode):
    mode = board_mode if board_mode in MODES else "path"
    d_col = MODE_COLS[mode][0]
    dead = tune.get("bend.deadzone", 0.025)
    # Each excursion past the deadzone, by sign: its peak |delta|.
    peaks, sign, peak = {"+": [], "-": []}, None, 0.0
    for s in samples:
        d = s[d_col]
        if d is None:
            continue
        side = "+" if d > dead else "-" if d < -dead else None
        if side != sign:
            if sign and peak:
                peaks[sign].append(peak)
            sign, peak = side, 0.0
        if side:
            peak = max(peak, abs(d))
    if sign and peak:
        peaks[sign].append(peak)
    allp = peaks["+"] + peaks["-"]
    res = {"mode": mode, "leans": {k: [round(v, 4) for v in vs] for k, vs in peaks.items()},
           "bend": bend_stats(samples), "flags": []}
    sugg = []
    if len(allp) >= 2:
        sens = settings.get("expression.pitch_bend_sensitivity", 0.065)
        want = round(min(0.3, max(dead + 0.015, statistics.median(allp) * 0.95)), 3)
        res["lean_median"] = round(statistics.median(allp), 4)
        if abs(want - sens) / sens > 0.12:
            sugg.append(suggestion("expression.pitch_bend_sensitivity", sens, want,
                                   "a comfortable lean (the median of yours) should give a full bend", kind="setting"))
        if peaks["+"] and peaks["-"]:
            ratio = statistics.median(peaks["+"]) / statistics.median(peaks["-"])
            res["left_right_ratio"] = round(ratio, 2)
            if not 0.7 <= ratio <= 1.4:
                res["flags"].append(f"one side reads {ratio:.2f}x the other (key pivot or magnet off-centre)")
    else:
        res["flags"].append("fewer than two leans past the deadzone")
    return res, sugg


def analyse_vibrato(samples, tune, board_mode):
    mode = board_mode if board_mode in MODES else "path"
    vib = MODE_COLS[mode][2]
    held = [s for s in samples if s[vib] is not None]
    res = {"mode": mode, "flags": []}
    if len(held) < 50:
        res["flags"].append("too little held time to judge")
        return res, []
    half = len(held) // 2
    wiggle = [s[vib] for s in held[:half]]
    still = [s[vib] for s in held[half + half // 5:]]  # skip the slowdown
    w75, s95 = pct(wiggle, 0.75), pct(still, 0.95)
    res.update({"wiggle_energy_p75": rnd(w75, 1), "still_energy_p95": rnd(s95, 1)})
    floor, full = tune.get("vibrato.floor", 8), tune.get("vibrato.full", 16)
    sugg = []
    if w75 < 1.4 * s95:
        res["flags"].append("a wiggle barely reads above holding still: at this noise the vibrato can't tell them apart")
        if tune.get("vibrato.depth", 150) > 0:
            sugg.append(suggestion("vibrato.depth", tune.get("vibrato.depth", 150), 0,
                                   "turn the added vibrato off until the sensor is quieter"))
        return res, sugg
    want_floor = round(1.25 * s95, 1)
    want_full = round(max(want_floor + 2, 0.8 * w75), 1)
    if abs(want_floor - floor) > 1:
        sugg.append(suggestion("vibrato.floor", floor, want_floor, "just above the energy of holding still"))
    if abs(want_full - full) > 1:
        sugg.append(suggestion("vibrato.full", full, want_full, "most of your wiggle reaches full vibrato depth"))
    return res, sugg


def reconcile(suggestions, tune, settings):
    """Suggestions from different steps can contradict: a deadzone at or past the full-bend point turns bend
    into a switch. Holds both back with the reason instead."""
    flat = [x for sid in (s["id"] for s in STEPS) for x in suggestions.get(sid, [])]
    find = lambda key: next((x for x in flat if x["key"] == key), None)
    dz_s, sens_s = find("bend.deadzone"), find("expression.pitch_bend_sensitivity")
    dead = dz_s["suggested"] if dz_s else tune.get("bend.deadzone", 0.025)
    sens = sens_s["suggested"] if sens_s else settings.get("expression.pitch_bend_sensitivity", 0.065)
    for x in flat:
        x.pop("blocked", None)
    if (dz_s or sens_s) and sens < dead + 0.015:
        for x in (dz_s, sens_s):
            if x:
                x["blocked"] = (f"a straight press reads about as much as a lean (deadzone {dead:g}, full bend "
                                f"{sens:g}): no threshold separates them yet")
    return flat


class Calibration:
    def __init__(self):
        self.reset()

    def reset(self):
        self.active = False
        self.index = 0
        self.recording = False
        self.started = 0.0
        self.results = {}
        self.suggestions = {}
        self.files = []
        self.mark = 0

    def step(self):
        return STEPS[self.index] if self.active and self.index < len(STEPS) else None

    def public(self, pad, tune=None, settings=None):
        step = self.step()
        left = max(0.0, step["seconds"] - (time.monotonic() - self.started)) if step and self.recording else None
        return {"active": self.active, "index": self.index, "recording": self.recording,
                "seconds_left": rnd(left, 1),
                "steps": [{"id": s["id"], "title": s["title"], "seconds": s["seconds"],
                           "text": s["text"].format(pad=pad), "done": s["id"] in self.results} for s in STEPS],
                "results": self.results,
                "suggestions": reconcile(self.suggestions, tune or {}, settings or {}),
                "files": self.files}
