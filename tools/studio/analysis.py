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
        self.depth_gain = None  # the board's automatic gain (key.depth_gain)
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
               "rest_confirmed": self.rest_confirmed, "sensor_ok": self.sensor_ok, "depth_gain": self.depth_gain}
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
     "text": "Take your hands off TILES. This measures every pad's resting field and sensor noise, and picks "
             "the test pads: the weakest, a middle and the strongest field."},
    {"id": "strikes", "title": "Strikes", "seconds": 12,
     "text": "Strike pad {pad} about 8 times, from the softest note you would play to the hardest."},
    {"id": "straight", "title": "Straight presses", "seconds": 12,
     "text": "Press pad {pad} straight down, slowly from light to as hard as it goes and back, 3 times. "
             "Don't lean. Any bend here is pressure leaking into pitch."},
    {"id": "lean", "title": "Leans", "seconds": 12,
     "text": "Press pad {pad}, lean it to one side as far as you want a full bend, hold a second, come back, "
             "then the other side. Twice each way."},
    {"id": "vibrato", "title": "Vibrato", "seconds": 8,
     "text": "Press pad {pad} and wiggle it side to side like a vibrato for 4 seconds, then hold it still "
             "for 4 seconds."},
]
STEP = {s["id"]: s for s in STEPS}
PAD_STEPS = ("strikes", "straight", "lean", "vibrato")


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
    ranked = sorted(measured, key=lambda r: r["rest_field_mt"])
    test = [ranked[0]["pad"], ranked[len(ranked) // 2]["pad"], ranked[-1]["pad"]] if len(ranked) >= 3 else \
        [r["pad"] for r in ranked]
    return {"pads_measured": len(measured), "rest_field_median_mt": rnd(med_field, 2),
            "rest_field_range_mt": [min(fields), max(fields)] if fields else None,
            "z_noise_median_ut": med_noise, "test_pads": list(dict.fromkeys(test)), "flags": flags}


def analyse_strikes(notes, tune):
    if not notes:
        return {"notes": 0, "peaks": [], "flags": ["no notes: strike harder than the strike depth?"]}
    vels = [n["velocity"] for n in notes]
    peaks = [n["peak_depth"] for n in notes if n["peak_depth"]]
    times = [n["strike_ms"] for n in notes if n["strike_ms"] is not None]
    res = {"notes": len(notes), "velocity": {"min": min(vels), "median": statistics.median(vels), "max": max(vels)},
           "strike_ms": {"min": min(times), "median": statistics.median(times), "max": max(times)} if times else None,
           "peak_depth": {"min": min(peaks), "median": statistics.median(peaks), "max": max(peaks)} if peaks else None,
           "peaks": peaks, "flags": []}
    if max(vels) < 110:
        res["flags"].append("the hardest strike stayed under 110")
    if sum(v >= 127 for v in vels) > len(vels) / 3:
        res["flags"].append("a third or more of the strikes hit 127")
    return res


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


def bend_was_off(samples):
    return any(s[FLAGS] & PRESSURE for s in samples) and not any(s[FLAGS] & STEPPED for s in samples)


def analyse_straight(samples, board_mode, on_board=True):
    stats = bend_stats(samples)
    depths = [s[SD] for s in samples if s[FLAGS] & PRESSURE and s[SD]]
    res = {"bend": stats, "max_pressure_depth": round(max(depths)) if depths else None, "flags": []}
    for m in MODES:
        if (stats[m]["peak_semitones"] or 0) > 0.5:
            res["flags"].append(f"{m} bent {stats[m]['peak_semitones']} semitones on straight presses")
    if on_board and bend_was_off(samples):
        res["flags"].append("pitch bend was off on the board: these bends are the replay's")
    return res


def analyse_lean(samples, tune, board_mode):
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
    if len(allp) >= 2:
        res["lean_median"] = round(statistics.median(allp), 4)
        if peaks["+"] and peaks["-"]:
            ratio = statistics.median(peaks["+"]) / statistics.median(peaks["-"])
            res["left_right_ratio"] = round(ratio, 2)
            if not 0.7 <= ratio <= 1.4:
                res["flags"].append(f"one side reads {ratio:.2f}x the other (key pivot or magnet off-centre)")
    else:
        res["flags"].append("fewer than two leans past the deadzone")
    return res


def xy_noise(samples):
    """X/Y noise (raw counts) of touched samples: each against a centred 15-sample average."""
    out = []
    for col in (X, Y):
        v = [s[col] for s in samples if s[FLAGS] & TOUCHED]
        if len(v) < 40:
            return None
        res = [v[i] - statistics.fmean(v[max(0, i - 7):i + 8]) for i in range(len(v))]
        out.append(statistics.pstdev(res))
    return round(max(out), 1)


def analyse_vibrato(samples, board_mode):
    mode = board_mode if board_mode in MODES else "path"
    vib = MODE_COLS[mode][2]
    held = [s for s in samples if s[vib] is not None]
    res = {"mode": mode, "flags": []}
    if len(held) < 50:
        res["flags"].append("too little held time to judge")
        return res
    half = len(held) // 2
    still_samples = held[half + half // 5:]  # skip the slowdown
    w75 = pct([s[vib] for s in held[:half]], 0.75)
    s95 = pct([s[vib] for s in still_samples], 0.95)
    res.update({"wiggle_energy_p75": rnd(w75, 1), "still_energy_p95": rnd(s95, 1),
                "xy_noise_raw": xy_noise(still_samples)})
    if w75 < 1.4 * s95:
        res["flags"].append("a wiggle barely reads above holding still: at this noise the vibrato can't tell them apart")
    return res


def general(results, pads, tune, settings, board_mode):
    """One set of values from every test pad together: the worst pad for leaks, medians for feel, and how
    depth grows with the rest field for the automatic depth gain. Never a value per pad."""
    def by(step):
        return {int(k.split(":")[1]): v for k, v in results.items() if k.startswith(step + ":")}

    strikes, straight, lean, vib = by("strikes"), by("straight"), by("lean"), by("vibrato")
    mode = board_mode if board_mode in MODES else "path"
    sugg, summary = [], {"test_pads": sorted(set(strikes) | set(straight) | set(lean) | set(vib))}

    peaks = [p for r in strikes.values() for p in r.get("peaks", [])]
    strike, over = tune.get("strike.depth", 150), tune.get("strike.overshoot_full", 550)
    if len(peaks) >= 4:
        want = round(max(100, pct(peaks, 0.9) - strike))
        summary["strike_peak_p90"] = pct(peaks, 0.9)
        if abs(want - over) / over > 0.15:
            sugg.append(suggestion("strike.overshoot_full", over, want,
                                   "the hardest strikes across the test pads (90th percentile) just reach full velocity"))

    leaks = {p: r["bend"][mode]["delta_p99"] for p, r in straight.items() if r["bend"][mode]["delta_p99"] is not None}
    lean_peaks = [v for r in lean.values() for side in r.get("leans", {}).values() for v in side]
    summary.update({"leak_p99": leaks, "lean_median": rnd(statistics.median(lean_peaks), 4) if lean_peaks else None})
    dead = tune.get("bend.deadzone", 0.025)
    sens = settings.get("expression.pitch_bend_sensitivity", 0.065)
    want_dead = want_sens = None
    if leaks:
        worst = max(leaks.values())
        want = round(min(0.08, max(0.01, 1.25 * worst)), 3)
        if worst > dead * 0.9 or (dead > 2 * want and dead > 0.015):
            want_dead = max(want, 0.012)
            sugg.append(suggestion("bend.deadzone", dead, want_dead,
                                   f"the leakiest test pad read up to {worst:.3f} pressing straight ({mode})"))
    if len(lean_peaks) >= 3:
        floor = (want_dead or dead) + 0.015
        want_sens = round(min(0.3, max(floor, statistics.median(lean_peaks) * 0.95)), 3)
        if abs(want_sens - sens) / sens > 0.12:
            sugg.append(suggestion("expression.pitch_bend_sensitivity", sens, want_sens,
                                   "a comfortable lean (the median across the test pads) gives a full bend", "setting"))

    hardest = {p: r["max_pressure_depth"] for p, r in straight.items() if r.get("max_pressure_depth")}
    summary["hardest_depth"] = hardest
    full = settings.get("expression.aftertouch_sensitivity")
    if hardest and full:
        want = int(round(statistics.median(hardest.values()) * 0.95))
        if abs(want - full) / full > 0.15:
            sugg.append(suggestion("expression.aftertouch_sensitivity", full, want,
                                   "a hard press on a typical test pad just reaches full pressure", "setting"))
    # Depth vs rest field across the test pads: the exponent that evens them out.
    fields = {p: pads[p].summary().get("rest_field_mt") for p in hardest if p in pads}
    pts = [(math.log(fields[p]), math.log(hardest[p])) for p in hardest if fields.get(p) and hardest[p] > 0]
    if len(pts) >= 2 and max(f for f, _ in pts) - min(f for f, _ in pts) > math.log(1.2):
        mx, my = statistics.fmean(f for f, _ in pts), statistics.fmean(d for _, d in pts)
        slope = sum((f - mx) * (d - my) for f, d in pts) / sum((f - mx) ** 2 for f, _ in pts)
        current = tune.get("key.depth_gain", 0.0)
        want = round(min(2.0, max(0.0, current + slope)), 2)  # depths already carry the current gain
        summary["depth_vs_field_slope"] = round(slope, 2)
        if abs(want - current) > 0.2:
            sugg.append(suggestion("key.depth_gain", current, want,
                                   f"hard presses grew with the rest field (slope {slope:.2f} on a log scale): this "
                                   "evens out weak and strong pads"))

    stills = [r["still_energy_p95"] for r in vib.values() if r.get("still_energy_p95") is not None]
    wiggles = [r["wiggle_energy_p75"] for r in vib.values() if r.get("wiggle_energy_p75") is not None]
    noise = [r["xy_noise_raw"] for r in vib.values() if r.get("xy_noise_raw")]
    summary.update({"still_energy": max(stills) if stills else None, "wiggle_energy": min(wiggles) if wiggles else None,
                    "xy_noise_raw": max(noise) if noise else None})
    if stills and wiggles:
        s95, w75 = max(stills), min(wiggles)
        v_floor, v_full, v_depth = tune.get("vibrato.floor", 8), tune.get("vibrato.full", 16), tune.get("vibrato.depth", 150)
        if w75 < 1.4 * s95:
            if v_depth > 0:
                sugg.append(suggestion("vibrato.depth", v_depth, 0,
                                       "on some test pad a wiggle barely reads above holding still: added vibrato off"))
        else:
            want_floor = round(1.25 * s95, 1)
            want_full = round(max(want_floor + 2, 0.8 * w75), 1)
            if abs(want_floor - v_floor) > 1:
                sugg.append(suggestion("vibrato.floor", v_floor, want_floor, "just above holding still on every test pad"))
            if abs(want_full - v_full) > 1:
                sugg.append(suggestion("vibrato.full", v_full, want_full, "every test pad's wiggle reaches full depth"))
    if noise:
        n = max(noise)
        want = round(n, 0)
        if abs(want - tune.get("bend.noise_xy", 20)) > 4:
            sugg.append(suggestion("bend.noise_xy", tune.get("bend.noise_xy", 20), want,
                                   f"X/Y noise measured while holding still: {n} raw"))
        averaging = tune.get("sensor.averaging", 8)
        if n > 30 and averaging < 16:
            sugg.append(suggestion("sensor.averaging", averaging, min(32, int(averaging) * 2),
                                   f"X/Y noise of {n} raw is still large next to a ~1 mT field"))

    if want_dead is not None and (want_sens or sens) < want_dead + 0.015:
        for x in sugg:
            if x["key"] in ("bend.deadzone", "expression.pitch_bend_sensitivity"):
                x["blocked"] = (f"a straight press reads about as much as a lean ({want_dead:g} vs "
                                f"{summary['lean_median']}): no threshold separates them yet")
    return sugg, summary


class Calibration:
    """Hands off, then strikes, straight presses, leans and vibrato on each test pad."""

    def __init__(self):
        self.reset()

    def reset(self):
        self.active = False
        self.index = 0
        self.recording = False
        self.started = 0.0
        self.plan = [("rest", None)]
        self.results = {}
        self.suggestions = []
        self.summary = {}
        self.applied = set()

    def set_test_pads(self, pads):
        self.plan = [("rest", None)] + [(s, p) for p in pads for s in PAD_STEPS]

    def item(self):
        return self.plan[self.index] if self.active and self.index < len(self.plan) else None

    @staticmethod
    def key(step, pad):
        return step if pad is None else f"{step}:{pad}"

    def public(self):
        item = self.item()
        left = max(0.0, STEP[item[0]]["seconds"] - (time.monotonic() - self.started)) \
            if item and self.recording else None
        for x in self.suggestions:
            x["applied"] = x["key"] in self.applied
        return {"active": self.active, "index": self.index, "recording": self.recording,
                "seconds_left": rnd(left, 1),
                "test_pads": [p for s, p in self.plan if s == "strikes"],
                "plan": [{"step": s, "pad": p, "title": STEP[s]["title"], "seconds": STEP[s]["seconds"],
                          "text": STEP[s]["text"].format(pad=p), "done": self.key(s, p) in self.results}
                         for s, p in self.plan],
                "results": self.results, "suggestions": self.suggestions, "summary": self.summary}
