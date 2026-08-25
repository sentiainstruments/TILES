# Defaults, safeguards, and V1 sensing scope

Status: agreed direction, not yet implemented. This is the reference
`firmware/src/board/` and `firmware/src/services/` get built against.
Nothing here overrides `docs/hardware/` — where a value is fixed by the
schematic, that's stated as fixed, not re-decided.

## V1 sensing scope: one axis only

The TMAG5273 gives raw X/Y/Z per pad, but for V1 firmware only derives
velocity, pressure, and aftertouch from a single axis — vertical press
depth. X/Y (tilt/lateral) are not mapped to any MIDI output yet.

- Per-pad calibration still captures full raw XYZ at rest/half-press/
  bottom-out (needed anyway to find which raw axis actually correlates
  with vertical travel for that pad, and its sign — see
  `docs/hardware/SENTIA_TILES_FIRMWARE_HANDOFF.md` §10/§26). What's
  deferred is the **four-direction tilt capture** and any X/Y-to-MIDI
  mapping, not the axis-selection work itself.
- MPE default mapping from the board map (`x_tilt -> pitch bend`,
  `y_tilt -> CC74/timbre`) is **not active in V1**. Only `pressure ->
  channel pressure` (from the depth axis) is live. Pitch bend stays
  centered/unused per pad until tilt sensing is scoped in.
- This is a firmware/services scope limit, not a hardware one — no board
  or driver changes implied. Re-enabling X/Y is adding a mapping in
  `services/`, not new sensing capability.

## Power safeguards (fixed by hardware — not a default, just the rule)

Restated here only so `board/` has one checklist, not re-derived from
first principles:

- USB-only boots to the 500mA operating budget and never auto-escalates;
  a higher USB profile is an explicit, manually-selected override for a
  validated computer/hub/cable, never inferred from the connection.
- CV and gate stay hard-disabled unless GP22 confirms external IN2 is
  selected.
- Both PCA9685 devices initialize to all-channels-off (MODE2.OUTDRV=1)
  before any other output service starts.
- GP20 (PCA9685 A5 address strap) is never driven, ever.
- All three LED mux banks are disabled before their selector bits change;
  exactly one bank enabled per pixel update.
- Only one Hall mux channel enabled across all three TCA9548A devices at
  a time.
- A failed subsystem disables itself and stays disabled; it never blocks
  USB diagnostics or takes other subsystems down with it.

## CV range

- Pitch and pressure/expression CV: 0–10V nominal (DAC80502 0–2.5V ×
  OPA2990 gain-of-4), per hardware.
- Zero-point calibration: real op-amp/DAC offset means commanded 0V won't
  be exact at the jack. Build the per-channel zero+gain trim slot into
  the calibration store from the start (`storage/`), even before there's
  a UI to set it — defaults to identity (no trim) until measured.
- CV and gate default **off** even when external power is present and
  valid. A profile must explicitly enable them. Fail-off by default, not
  just fail-off on fault.

## Gate

Active-high, full logic swing for note-on duration — fixed by the
two-stage driver design, nothing to configure here.

## MIDI (DIN) polarity

- Default: **Type A** TRS polarity (the MIDI Association's officially
  adopted standard; what most current gear ships with).
- Selectable via profile (GP0/GP2 role swap), not auto-detected — there's
  no way to sense polarity from the jack side alone.

## Pedal polarity

- **Auto-sensed at every boot**, not a fixed default. Read the pedal ADC
  before assuming anything: a disconnected pedal settles toward the
  100k-pullup rail; a connected NO/NC pedal settles differently at rest.
  Boot-time read establishes the rest level and infers polarity from it,
  same approach most keyboards use for sustain-pedal auto-sense.
- Re-check on wake from standby, in case a pedal was plugged in while the
  device was idle.

## LED color and brightness

**Color (V1 default):** white on both pad LEDs and underglow. No color
themes/palettes yet — that's a `profiles/` feature once it exists. White
also lets brightness be reasoned about as one scalar instead of per-
channel, which matters for the power governor below.

**Behavior (V1 default):**

- Underglow: always on, solid white, at its own fixed high brightness
  (230/255) that does **not** scale with the pad brightness ceiling
  below — only 4 LEDs are on that chain, so even full brightness is a
  negligible fraction of the board's current budget, unlike the 24-pad
  grid. Not reactive to anything — it's the ambient halo per the
  product's visual language (a soft perimeter, not a focal point), so it
  doesn't compete for attention with pad state.
- Pad LEDs: always on at a dim white **idle baseline** (~10% of the
  active ceiling) rather than off, so the grid reads as "alive" at rest.
  A touched/pressed pad ramps up toward the active profile's brightness
  **ceiling** (below) — full brightness means "this pad is active," not
  a fixed absolute value, since the ceiling itself moves with the power
  profile.
- The press→brightness curve should key off whatever's actually
  available first: touch state now (once `drivers/mpr121` exists), Hall
  depth once that's live too. Until either driver exists, pads simply
  sit at idle baseline — there's nothing to react to yet.

**Brightness ceiling** — needs an actual number, not just "cap it
somewhere": full-white across 24 pad LEDs + 4 underglow is ~448mA
against a 500mA USB-only budget before anything else on the board draws
current.

- USB-only: global brightness ceiling **35–40%**.
- External power (FULL_DEMO_EXTERNAL or equivalent): ceiling **70–80%**.
- Enforced in the lighting service as a hard clamp regardless of what a
  profile, animation, or press state requests — something can ask for
  100%, the power governor still clamps it to the active budget's
  ceiling. The idle baseline (~10% of ceiling) is comfortably under this
  even summed across all 24 pads + underglow simultaneously.
- These two numbers are engineering estimates, not hardware-mandated —
  revisit once real 5V input current is measured per
  `docs/hardware/.../measure_before_full_power`.

**Standby animations — still open, not V1.** Once the device has been
idle long enough to enter a standby profile, pad/underglow behavior
should switch to some animated pattern instead of the static idle
baseline. The legacy prototype's `standbySmoothWave()` /
`standbyCenterRipple()` (see
`docs/reference/legacy-prototype-v1/legacy_tiles_prototype.ino`) are
reasonable starting inspiration for the *shape* of this, but need
redesigning for a 6×4 addressable-RGB grid instead of a 4×4 shift-
register on/off LED bar — this needs its own design pass (idle timeout
duration, wake trigger, whether animations run through the same muxed
LED path or need a different refresh strategy given the mux can only
show one pixel at a time).

## Pad baseline calibration and drift compensation

- **At boot**: full raw capture for all 24 pads (rest, and — per V1 scope
  above — enough of half/bottom to find the depth axis and its sign).
  This is the anchor baseline for the session.
- **During operation**: a slow, *gated* drift tracker, not a timer. A
  pad's baseline updates only when all of the following hold
  simultaneously:
  - `touched == false` for that pad,
  - no active MIDI note/voice currently allocated to that pad,
  - the depth-axis reading has been stable (variance below a noise
    threshold) for a dwell window of roughly 300–500ms.

  When all three hold, nudge the baseline toward the current reading
  with a slew-rate-limited filter (move on the order of 0.5–1% of the
  gap per update), not a snap. This tracks slow thermal/mechanical drift
  over a session but cannot mistake a slow press for drift, because
  touch and active-note both gate it off immediately, and Hall-variance
  alone gates out anything mid-motion.
- **Manual override**: a diagnostics command (and later, a function-
  button action) forces an immediate full recalibration of one or all
  pads, for cases like a reseated keycap mid-session where the slow
  tracker would take too long to catch up.
- Touch-alone and Hall-variance-alone are each individually foolable
  (a light resting finger may not register as touched; thermal drift and
  a very slow press look similar in variance alone) — gating on both
  together is the standard approach capacitive touch strips use for the
  same problem, and is what makes the slow tracker safe to run
  continuously rather than only during an explicit idle/standby state.

## Explicitly deferred (not decided against, just not V1)

- X/Y tilt sensing and any pitch-bend/CC74 mapping from it.
- Companion app and the USB vendor config protocol.
- Per-pad LED animation themes beyond basic touch/note feedback.
- Real per-motor current/duty measurement through the actual flex --
  `firmware/src/services/haptics.c`'s kick/sustain duty curves ship as
  unmeasured placeholder estimates (built ahead of this measurement, at
  explicit user direction, not because the measurement stopped
  mattering) pending it, same as this doc's LED brightness ceilings did
  before hardware existed to check them against.
