# Defaults, safeguards, and sensing

The defaults and safety rules that span several firmware modules, and
the reasoning behind them. Each section names the code that owns it;
that code is authoritative for exact values. Hardware facts live in
`docs/hardware/` (with the corrections in `firmware/src/board/README.md`).

## Sensing

Each pad has a capacitive touch electrode (MPR121) and a 3-axis Hall
sensor (TMAG5273) under a magnet on the key.

- **Depth** is Z against the pad's rest baseline, the same way for
  every pad (flat-mounted sensor, straight vertical travel;
  `services/hall.h`). It gives velocity (depth over a short
  follow-through, plus a little speed), pressure while held, and release
  velocity from lift-off speed (`services/expression.c`).
- **Touch** gates note timing: a note starts on touch + a real press and
  ends when touch ends. A light tap never plays.
- **Tilt** (X/Y) gives per-note pitch bend, from the field's direction
  cosine so that pressing harder doesn't bend. Sign from X, magnitude
  from X and Y. A vibrato detector adds to it. Pitch bend boots off;
  square turns it on.
- **Not mapped:** Y tilt to CC74/slide (the board map's suggested
  default), and per-pad calibration curves. One depth full-scale applies
  to every pad (`expression.aftertouch_sensitivity`), set from strong
  strikes on a few pads.

Most sensing constants come from captures on the real board; the ones
that don't are marked in `expression.c`.

## Power safeguards

Fixed by hardware. The same list is `firmware/README.md`'s
non-negotiables:

- USB-only never uses more than the 500 mA budget and never escalates by
  itself. A higher USB budget would only ever be a manual choice for a
  validated computer/hub/cable (not built).
- CV and gate stay hard-disabled unless GP22 shows external power.
- Both PCA9685s start all-off (MODE2.OUTDRV=1) before any other output
  service. GP20 is their shared output enable (active low): driven low
  only after every channel is configured, never driven high. (The
  original hardware handoff called GP20 an address strap; its
  Corrections note and `firmware/src/board/README.md` have the details.)
- All three LED mux banks are disabled while their selector bits change;
  one bank is enabled per pixel update.
- Only one Hall mux channel is open across the three TCA9548As at a time.
- A failed subsystem disables itself and stays disabled; it never blocks
  USB or takes other subsystems down.

Budgets per power mode (`services/power.c`; every consumer reads them
from `services/power.h`):

| Mode | 5 V budget | Pad LED ceiling | Haptic voices | CV/gate |
|---|---|---|---|---|
| USB only | 500 mA | 50% | 4 | not allowed |
| External (with or without USB) | 2.5 A | 90% | 12 | allowed, still off until enabled |
| Fault (no external power, USB not mounted: usually a brief enumeration transient) | 500 mA | 37% | 0 | not allowed |

Ceilings from measured current (`docs/hardware/current-measurements.md`):
the board draws ~80 mA, all 28 LEDs ~4.1 mA per percent of brightness, a
motor ~28 mA running. At USB-only's 50% the worst case (every LED white,
4 motors) is ~0.42 A; normal playing measured 0.15-0.35 A. TILES declares
500 mA to the USB host. Fault keeps the old 37%: the source is unknown.

## LED color and brightness

- **Pads** are a flat fraction of the power mode's ceiling, never
  load-aware: a ceiling that rose when fewer pads were lit made the
  whole board's brightness shift while playing, which looked like a
  brownout (`services/lighting.h`).
- **Underglow** runs at its own fixed level (230/255), not scaled by the
  ceiling. Four LEDs are a small share of the budget, and a steady halo
  doesn't compete with pad feedback.
- **Idle look in melodic mode:** root in Sentia magenta, fifth in violet,
  other natural keys dim white, sharps dark; a pressed pad is white. The
  levels are `look.*` settings (defaults in `services/lighting.c`).
  Other modes, the menus and games draw their own colors.
- **Standby:** after 1 minute idle the pads, buttons and underglow run
  rotating ambient animations; after 20 minutes, deep sleep (dark except
  a slowly pulsing circle). Details in `services/standby.h`.

## CV and gate

- Pitch CV on VOUTA (1 V/octave) and pressure CV on VOUTB, 0-10 V
  (DAC80502 0-2.5 V x the OPA2990's gain of 4). Gate on GP12, active
  high while any note is held; last-note priority, like a standard
  monophonic MIDI-to-CV converter (`services/cv_gate.h`).
- Off unless external power is present **and** CV/gate is enabled. The
  enable (`cv_gate.enabled`) is never saved: CV/gate boots off every
  time. Fail-off by default, not only on a fault.
- Per-channel zero and gain trims are settings (`cv_gate.*`), identity
  until measured.
- Not yet verified on hardware.

## DIN MIDI polarity

- Default **Type A** TRS polarity (the MIDI Association standard);
  Type B via the `midi.din_trs_type` setting. Never auto-detected: the
  jack can't sense polarity (`midi/din_midi.h`).
- Type A = GP0 is inferred from the handoff's naming; the electrical
  side hasn't been verified on hardware.

## Pedal polarity

- Normally-open by default (unpressed reads high through the pull-up);
  normally-closed via `pedal.polarity`. Mode (sustain CC64 or expression
  CC11) and sustain style are settings too (`services/pedal.h`).
- Not built: auto-sensing polarity and connection from the rest level
  at boot and on wake, as most keyboards do. Until then the setting
  decides.

## Pad baseline calibration and drift compensation

Owned by `services/hall.c`:

- **Rest baseline:** each pad's rest Z is captured at init and again at
  the end of the boot animation. Both assume hands off the pads; a pad
  the touch sensor reports touched at the second capture keeps its old
  baseline, unconfirmed (below). The serial calibration command `r`
  recaptures every pad on demand (`diagnostics/calibration.h`).
- **Rest guard** (`services/rest_guard.h`), fed every untouched read:
  - Drift: a pad's baseline creeps toward its reading only while the pad
    is untouched and each read stays within 8 counts of the previous one
    for 400 ms, and then only by 1/128 of the gap per qualifying read.
    This follows slow thermal and mechanical drift through a session
    without mistaking a slow press for drift.
  - Re-zero: untouched and steady for 600 ms at least a strike's depth
    (150) from the baseline, the baseline jumps to the reading. Keys
    spring back, so an untouched key can't be held down: the baseline
    was taken pressed (a finger on the key at start-up). Found on unit 2
    (2026-10-23): pad 6, baseline taken under a finger, played on its
    release instead of its press.
  - Unconfirmed: no depth (the pad can't play or press) until a steady
    400 ms untouched sets the baseline.
  INFO reports `hall.rezeros` and `hall.unconfirmed`.

## Sensor and controller upkeep

- **Hall sensors** (`services/hall.c`), only while no pad is touched (a
  retry costs about a millisecond): a pad whose reads fail 10 times in a
  row is set aside; it, and any sensor that never set up, is retried one
  every 2 s. One pad's config is checked every 250 ms (each pad every
  ~6 s): a sensor that lost power answers but stays in standby, and is
  configured again. INFO `hall.reinits`, `hall.dead`.
- **Touch controllers** (`services/touch.c`): each checked once a second
  (one register read); one that fails two checks in a row (reset by
  static or a power glitch, it sits in Stop Mode reporting nothing; or
  never set up) is set up again. INFO `touch.recoveries`. Not done: re-
  taking a controller's baselines when a pad reads touched for minutes
  with no press (Alpha 5): it fired 28 times in normal playing on unit 2
  and left pads less sensitive (a baseline taken under a hovering hand),
  so it was removed in Alpha 5.2.
- **After a crash restart** (`main.c`): all notes off on every port once
  at start and again when USB mounts, since notes held then never get
  their Note-Off.
- **Why both gates:** each alone is foolable. A light resting finger may
  not register as touched, and slow drift and a very slow press look
  alike in variance alone. Untouched also implies no note is sounding
  (`expression.c`'s state machine), so there is no separate note check.

## Not built yet

- Per-pad Hall calibration curves, and a guided calibration flow in the
  companion app.
- Y tilt to CC74/slide.
- Pedal polarity/connection auto-sense.
- A manually selected higher USB power budget.
- Measured motor current and haptic duty: `services/haptics.c`'s kick
  and sustain curves are unmeasured estimates, as are the LED ceilings
  above.
