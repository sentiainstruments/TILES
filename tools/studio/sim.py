"""A simulated TILES key for the studio, no hardware: a dipole magnet over a
TMAG5273, with the misalignment, sensor offset and noise that make a straight
press look like tilt (reports/Hall sensor tilt and pressure separation.md),
and a small copy of expression.c's strike logic, following the live tuning,
so its rows carry the same flags as a device capture
(firmware/src/diagnostics/capture.h).

Sizes are guesses until a real capture measures them: rest field, gap,
travel and pivot are all settable from the studio."""

import math
import random

LSB_PER_MT = 409.6  # +/-80 mT range, 16 bits
TICK_MS = 2  # one main-loop pass

# capture.h flags
VALID, TOUCHED, STARTED, STEPPED, BENT = 0x01, 0x02, 0x04, 0x08, 0x10
STROKE, STROKE_BEGIN, NOTE_ON, PRESSURE, NOTE_OFF = 0x20, 0x40, 0x80, 0x100, 0x200
IDLE, AWAITING, HELD = 0, 1, 2

ROUND_ROBIN_MS = 50  # an untouched pad is read this often
SETTLE_SAMPLES = 2

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

# The calibration steps' gestures, played one after another.
SEQUENCES = {
    "strikes": [("strike", 0.22 + 0.1 * i) for i in range(8)],
    "straight": [("press", None)] * 3,
    "lean": [("lean", 1), ("lean", -1), ("lean", 1), ("lean", -1)],
    "vibrato": [("vibrato_hold", None)],
}


def gesture(name, t, arg=None):
    """(finger_on, press, tilt_x_deg, tilt_y_deg) t ms into a scripted gesture, or None when it's over."""
    if t < 0:
        return False, 0.0, 0.0, 0.0  # the breath before it

    def strike(t, level):
        if t < 4:
            return 0.0
        if t < 16:
            return level * (t - 4) / 12
        return level

    if name == "strike":  # arg: level
        level, hold = (arg, 600) if arg is not None else (0.75, 1200)
        if t > hold + 40:
            return None
        if t > hold:
            return True, level * (hold + 40 - t) / 40, 0.0, 0.0
        return True, strike(t, level), 0.0, 0.0
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
    if name == "lean":  # arg: +1 or -1
        if t > 2200:
            return None
        p = strike(t, 0.5)
        lean = 0.0
        if 300 <= t < 500:
            lean = 3.2 * (t - 300) / 200
        elif 500 <= t < 1500:
            lean = 3.2
        elif 1500 <= t < 1800:
            lean = 3.2 * (1800 - t) / 300
        if t >= 2000:
            p = 0.5 * (2200 - t) / 200
        return True, p, lean * (arg or 1), 0.0
    if name in ("vibrato", "vibrato_hold"):
        wiggle_end, end = (2600, 2900) if name == "vibrato" else (4300, 8200)
        if t > end:
            return None
        p = strike(t, 0.6)
        lean = 0.0
        if 300 <= t < wiggle_end:
            ramp = min(1.0, (t - 300) / 200, (wiggle_end - t) / 150)
            lean = 1.4 * ramp * math.sin(2 * math.pi * 5.5 * (t - 300) / 1000)
        if t >= end - 200:
            p = 0.6 * (end - t) / 200
        return True, p, lean, 0.0
    return None


class Sim:
    def __init__(self):
        self.params = dict(DEFAULTS)
        self.tune = {}
        self.t = 0
        self.rng = random.Random(7)
        self.manual = (False, 0.0, 0.0, 0.0)
        self.queue = []
        self.gesture_name = None
        self.gesture_arg = None
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
        self.rise_depth = 0
        self.since_rise = 0
        self.sdepth = 0.0
        self.prev_depth = 0
        self.fall_rate = 0.0
        self.last_touch_ms = -10_000
        self.truth = (0.0, 0.0)

    def tv(self, key, default):
        return self.tune.get(key, default)

    def set_params(self, values):
        for key, value in values.items():
            if key in self.params:
                self.params[key] = int(value) if key == "averaging" else float(value)
        self.rest_z = self._field(0.0, 0.0, 0.0)[2]

    def play(self, name, arg=None):
        self.queue = []
        self._start(name, arg)

    def play_sequence(self, name):
        self.queue = list(SEQUENCES.get(name, []))
        self.gesture_name = None
        if self.queue:
            self._start(*self.queue.pop(0))

    def _start(self, name, arg):
        self.gesture_name, self.gesture_arg, self.gesture_t0 = name, arg, self.t

    def busy(self):
        return self.gesture_name is not None

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
        while self.gesture_name:
            g = gesture(self.gesture_name, self.t - self.gesture_t0, self.gesture_arg)
            if g is not None:
                return g
            self.gesture_name = None
            if self.queue:
                name, arg = self.queue.pop(0)
                self._start(name, arg)
                self.gesture_t0 += 300  # a breath between gestures
                return False, 0.0, 0.0, 0.0
        return self.manual

    def _velocity(self):
        fast, slow = self.tv("velocity.time_fast_ms", 10), self.tv("velocity.time_slow_ms", 300)
        if self.strike_ms <= fast:
            time_score = 1.0
        elif self.strike_ms >= slow:
            time_score = 0.0
        else:
            time_score = ((slow - self.strike_ms) / (slow - fast)) ** self.tv("velocity.curve", 1.0)
        strike = self.tv("strike.depth", 150)
        depth_score = min(1.0, max(0.0, self.peak - strike) / self.tv("strike.overshoot_full", 550))
        w = self.tv("velocity.depth_weight", 0.85)
        low = self.tv("velocity.min", 8)
        return max(int(low), min(127, int(low + (127 - low) * (w * depth_score + (1 - w) * time_score))))

    def _release_velocity(self):
        rate = -self.fall_rate
        lo, hi = self.tv("release.rate_min", 2.0), self.tv("release.rate_max", 25.0)
        if rate <= lo:
            return 1
        if rate >= hi:
            return 127
        return int(1 + 126 * (rate - lo) / (hi - lo))

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
        if finger:
            self.last_touch_ms = self.t
        x, y, z = self.sample
        depth = min(65535, abs(z - int(round(self.rest_z * LSB_PER_MT))))
        flags = VALID | (TOUCHED if finger else 0)
        state_at_start = self.state
        velocity = aftertouch = strike_ms = peak = 0
        sdepth = 0.0
        strike = self.tv("strike.depth", 150)

        if self.state == IDLE and finger:
            self.state = AWAITING
            flags |= STROKE_BEGIN
            self.peak = depth
            self.crossed_ms = self.t if depth >= strike else None
            self.touch_ms = self.t
            self.strike_ms = 0
        elif self.state == AWAITING:
            commit = False
            if finger:
                flags |= STROKE
                self.peak = max(self.peak, depth)
                if self.crossed_ms is None and self.peak >= strike:
                    self.crossed_ms = self.t
                    self.strike_ms = self.t - self.touch_ms
                    self.rise_depth, self.since_rise = depth, 0
                elif self.crossed_ms is not None:
                    if depth >= self.rise_depth + self.tv("strike.settle_rise", 8):
                        self.rise_depth, self.since_rise = depth, 0
                    else:
                        self.since_rise += 1
                    full = strike + self.tv("strike.overshoot_full", 550)
                    commit = (self.since_rise >= SETTLE_SAMPLES or self.peak >= full
                              or self.t - self.crossed_ms >= self.tv("strike.followthrough_ms", 20))
            else:
                commit = self.crossed_ms is not None
                if not commit:
                    self.state = IDLE
            if commit:
                velocity, strike_ms, peak = self._velocity(), self.strike_ms, int(self.peak)
                flags |= NOTE_ON
                self.state = HELD
                self.sdepth = float(depth)
                self.prev_depth = depth
                self.fall_rate = 0.0
        elif self.state == HELD:
            touch_gap = self.t - self.last_touch_ms
            key_down = depth >= strike and touch_gap < self.tv("key.pressed_dropout_ms", 300)
            if not finger and not key_down:
                flags |= NOTE_OFF
                velocity = self._release_velocity()
                self.state = IDLE
            else:
                self.sdepth += self.tv("pressure.smoothing", 0.35) * (depth - self.sdepth)
                rate = (depth - self.prev_depth) / TICK_MS
                self.fall_rate += self.tv("release.smoothing", 0.5) * (rate - self.fall_rate)
                self.prev_depth = depth
                sdepth = self.sdepth
                full = self.tune.get("expression.aftertouch_sensitivity", 1450)
                aftertouch = min(127, int(self.sdepth * 127 / full))
                flags |= PRESSURE
        return (f"{self.t},{x},{y},{z},{depth},{sdepth:.1f},8192,{flags},{state_at_start},{velocity},{aftertouch},"
                f"{strike_ms},{peak}")
