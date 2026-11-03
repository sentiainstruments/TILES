"""A simulated TILES key for the studio, no hardware: a dipole magnet over a
TMAG5273, with the misalignment, sensor offset and noise that make a straight
press look like tilt (reports/Hall sensor tilt and pressure separation.md),
and a small copy of expression.c's strike logic so its rows carry the same
flags as a device capture (firmware/src/diagnostics/capture.h).

Sizes are guesses until a real capture measures them: rest field, gap,
travel and pivot are all settable from the studio."""

import math
import random

LSB_PER_MT = 409.6  # +/-80 mT range, 16 bits
TICK_MS = 2  # one main-loop pass

# capture.h flags
VALID, TOUCHED, STARTED, STEPPED, BENT = 0x01, 0x02, 0x04, 0x08, 0x10
STROKE, STROKE_BEGIN, NOTE_ON, PRESSURE = 0x20, 0x40, 0x80, 0x100
IDLE, AWAITING, HELD = 0, 1, 2

# expression.c / strike_settle.h
STRIKE_DEPTH = 150
OVERSHOOT_FULL = 550
FOLLOW_MS = 20
SETTLE_RISE = 8
AFTERTOUCH_FULL = 1450
ROUND_ROBIN_MS = 50  # an untouched pad is read this often

DEFAULTS = {
    "offset_x_mm": 0.4,  # magnet off the Hall elements' axis
    "offset_y_mm": 0.1,
    "rest_mt": 7.5,  # Bz at rest
    "gap_mm": 4.0,
    "travel_mm": 0.6,  # at press 1.0
    "pivot_mm": 5.0,  # pivot below the magnet centre
    "sensor_offset_x_ut": 300.0,
    "sensor_offset_y_ut": -150.0,
    "averaging": 1,  # 1, 8 or 16: noise / sqrt(N)
    "tremor_deg": 0.04,
}


def gesture(name, t):
    """(finger_on, press, tilt_x_deg, tilt_y_deg) t ms into a scripted gesture, or None when it's over."""

    def strike(t, level):
        if t < 4:
            return 0.0
        if t < 16:
            return level * (t - 4) / 12
        return level

    if name == "strike":
        if t > 1240:
            return None
        if t > 1200:
            return True, 0.75 * (1240 - t) / 40, 0.0, 0.0
        return True, strike(t, 0.75), 0.0, 0.0
    if name == "press":  # strike, then press harder and back, straight
        if t > 3200:
            return None
        p = strike(t, 0.4)
        if 500 <= t < 2000:
            p = 0.4 + 0.6 * (t - 500) / 1500
        elif 2000 <= t < 2400:
            p = 1.0
        elif 2400 <= t < 3000:
            p = 1.0 - 0.6 * (t - 2400) / 600
        elif t >= 3000:
            p = 0.4 * (3200 - t) / 200
        return True, p, 0.0, 0.0
    if name == "tilt":  # strike, lean 3 degrees, then press harder while leaning
        if t > 3200:
            return None
        p = strike(t, 0.5)
        lean = 0.0
        if t >= 400:
            lean = 3.0 * min(1.0, (t - 400) / 150)
        if 1200 <= t < 1800:
            p = 0.5 + 0.45 * (t - 1200) / 600
        elif 1800 <= t < 3000:
            p = 0.95
        if t >= 3000:
            p, lean = 0.95 * (3200 - t) / 200, lean * (3200 - t) / 200
        return True, p, lean, 0.0
    if name == "vibrato":
        if t > 2900:
            return None
        p = strike(t, 0.6)
        lean = 0.0
        if 300 <= t < 2600:
            ramp = min(1.0, (t - 300) / 200)
            lean = 1.4 * ramp * math.sin(2 * math.pi * 5.5 * (t - 300) / 1000)
        if t >= 2700:
            p = 0.6 * (2900 - t) / 200
        return True, p, lean, 0.0
    return None


class Sim:
    def __init__(self):
        self.params = dict(DEFAULTS)
        self.t = 0
        self.rng = random.Random(7)
        self.manual = (False, 0.0, 0.0, 0.0)
        self.gesture_name = None
        self.gesture_t0 = 0
        self.press = 0.0
        self.tilt_x = 0.0
        self.tilt_y = 0.0
        self.state = IDLE
        self.last_read_ms = -ROUND_ROBIN_MS
        self.sample = (0, 0, 0)
        self.rest_z = self._field(0.0, 0.0, 0.0)[2]
        self.peak = 0
        self.crossed_ms = None
        self.touch_ms = 0
        self.strike_ms = 0
        self.last_depths = []
        self.sdepth = 0.0
        self.truth = (0.0, 0.0)

    def set_params(self, values):
        for key, value in values.items():
            if key in self.params:
                self.params[key] = int(value) if key == "averaging" else float(value)
        self.rest_z = self._field(0.0, 0.0, 0.0)[2]

    def play(self, name):
        self.gesture_name = name
        self.gesture_t0 = self.t

    def setup_line(self):
        return "# bend mode=path sensitivity=0.065 bx=0 by=0 strokes=0"

    def _field(self, press, tilt_x_deg, tilt_y_deg):
        """Noise-free (x, y, z) in mT at the sensor."""
        p = self.params
        h0 = p["gap_mm"]
        k = p["rest_mt"] * h0**3 / 2.0  # on-axis Bz = 2k / h^3
        ax, ay = math.radians(tilt_x_deg), math.radians(tilt_y_deg)
        mx, my, mz = math.sin(ax), math.sin(ay), math.cos(ax) * math.cos(ay)
        px = p["offset_x_mm"] + p["pivot_mm"] * math.sin(ax)
        py = p["offset_y_mm"] + p["pivot_mm"] * math.sin(ay)
        h = h0 - p["travel_mm"] * press
        rx, ry, rz = -px, -py, -h
        r = math.sqrt(rx * rx + ry * ry + rz * rz)
        ux, uy, uz = rx / r, ry / r, rz / r
        dot = mx * ux + my * uy + mz * uz
        scale = k / r**3
        bx = scale * (3 * dot * ux - mx) + p["sensor_offset_x_ut"] / 1000.0
        by = scale * (3 * dot * uy - my) + p["sensor_offset_y_ut"] / 1000.0
        bz = scale * (3 * dot * uz - mz)
        return bx, by, bz

    def _read(self):
        # 1x noise below the datasheet's 125/68 uT: set so a still hold gives
        # the vibrato energy captured on unit 2 (~7.5, services/HISTORY.md).
        n = max(1, self.params["averaging"])
        sxy = 0.085 / math.sqrt(n) * LSB_PER_MT
        sz = 0.046 / math.sqrt(n) * LSB_PER_MT
        bx, by, bz = self._field(self.press, self.tilt_x, self.tilt_y)
        clamp = lambda v: max(-32768, min(32767, int(round(v))))
        return (
            clamp(bx * LSB_PER_MT + self.rng.gauss(0, sxy)),
            clamp(by * LSB_PER_MT + self.rng.gauss(0, sxy)),
            clamp(bz * LSB_PER_MT + self.rng.gauss(0, sz)),
        )

    def _inputs(self):
        if self.gesture_name:
            g = gesture(self.gesture_name, self.t - self.gesture_t0)
            if g is not None:
                return g
            self.gesture_name = None
        return self.manual

    def step(self):
        """One 2 ms pass; returns a capture row (a string) or None when nothing new was read."""
        self.t += TICK_MS
        finger, press, tilt_x, tilt_y = self._inputs()
        # The finger moves the key in a few ms; tremor on top while touched.
        follow = 1.0 if self.gesture_name else 0.25
        self.press += follow * (max(0.0, min(1.0, press)) - self.press)
        tremor = self.params["tremor_deg"] if finger else 0.0
        wobble = tremor * math.sin(2 * math.pi * 9.0 * self.t / 1000) + tremor * 0.5 * self.rng.gauss(0, 1)
        self.tilt_x += follow * (tilt_x - self.tilt_x)
        self.tilt_y += follow * (tilt_y - self.tilt_y)
        self.truth = (self.press, self.tilt_x)
        saved = self.tilt_x
        self.tilt_x += wobble
        fresh = finger or self.state != IDLE or self.t - self.last_read_ms >= ROUND_ROBIN_MS
        if fresh:
            self.sample = self._read()
            self.last_read_ms = self.t
        self.tilt_x = saved
        if not fresh:
            return None  # the device's watch stream drops repeated passes too
        x, y, z = self.sample
        depth = min(65535, abs(z - int(round(self.rest_z * LSB_PER_MT))))
        flags = VALID | (TOUCHED if finger else 0)
        state_at_start = self.state
        velocity = 0
        aftertouch = 0
        sdepth = 0.0

        if self.state == IDLE and finger:
            self.state = AWAITING
            flags |= STROKE_BEGIN
            self.peak = depth
            self.crossed_ms = self.t if depth >= STRIKE_DEPTH else None
            self.touch_ms = self.t
            self.strike_ms = 0
            self.last_depths = []
        elif self.state == AWAITING:
            commit = False
            if finger:
                flags |= STROKE
                self.peak = max(self.peak, depth)
                if self.crossed_ms is None and self.peak >= STRIKE_DEPTH:
                    self.crossed_ms = self.t
                    self.strike_ms = self.t - self.touch_ms
                elif self.crossed_ms is not None:
                    self.last_depths.append(depth)
                    rise = self.last_depths[-1] - self.last_depths[-3] if len(self.last_depths) >= 3 else SETTLE_RISE
                    stalled = rise < SETTLE_RISE
                    commit = stalled or self.t - self.crossed_ms >= FOLLOW_MS
            else:
                commit = self.crossed_ms is not None
                if not commit:
                    self.state = IDLE
            if commit:
                time_score = 1.0 if self.strike_ms <= 10 else max(0.0, (300 - self.strike_ms) / 290)
                depth_score = min(1.0, max(0.0, self.peak - STRIKE_DEPTH) / OVERSHOOT_FULL)
                velocity = int(8 + 119 * (0.85 * depth_score + 0.15 * time_score))
                flags |= NOTE_ON
                self.state = HELD
                self.sdepth = float(depth)
        elif self.state == HELD:
            if not finger and depth < STRIKE_DEPTH:
                self.state = IDLE
            else:
                self.sdepth += 0.35 * (depth - self.sdepth)
                sdepth = self.sdepth
                aftertouch = min(127, int(self.sdepth * 127 / AFTERTOUCH_FULL))
                flags |= PRESSURE
        return f"{self.t},{x},{y},{z},{depth},{sdepth:.1f},8192,{flags},{state_at_start},{velocity},{aftertouch}"
