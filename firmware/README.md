# firmware/

Raspberry Pi Pico 2 (RP2350) firmware for SENTIA TILES Rev A0. Pico SDK
C/C++ + TinyUSB. Hardware truth for this revision lives in
`../docs/hardware/` — nothing in here should hard-code a GPIO number, I2C
address, or channel that isn't already in that board map.

## Module boundary

| Module | Owns |
|---|---|
| `src/board/` | Raw GPIO, I2C/SPI/PIO resources, hardware constants, and the canonical `PadConfig[24]` table. Nothing outside this module touches a physical pin or channel number directly. |
| `src/drivers/` | Small, independently testable per-chip drivers: TCA9548A, TMAG5273, MPR121, PCA9685, TCA9554, SK6805, DAC80502. |
| `src/services/` | Hall scanning, touch events, lighting, haptics, pedal input, calibration, expression mapping, current/power governance. |
| `src/midi/` | TinyUSB USB-MIDI/MPE, DIN MIDI in, polarity-selectable DIN MIDI out. |
| `src/usb_vendor/` | The custom USB vendor interface used by the companion app for remap/calibration/diagnostics/live-monitor traffic. Implements the protocol defined in `../shared/protocol/`. Kept separate from `midi/` so config traffic never competes with MIDI bandwidth. |
| `src/profiles/` | Runtime feature flags, power budgets, note/scale layouts, button bindings, visual/haptic themes. What the companion app edits and downloads to the device. |
| `src/diagnostics/` | Per-pad test commands, I2C enumeration, raw sensor streaming, fault reporting — exposed over `usb_vendor/`. |
| `src/storage/` | Versioned, CRC-protected, two-slot flash configuration (profiles + per-pad calibration). |

## Bring-up order

Per `docs/hardware/SENTIA_FIRMWARE_CODEX_START.md`: board constants + safe
boot + diagnostics first, then I2C discovery with outputs forced off, then
buttons/power-status/pedal, then Hall scanning, then touch, then USB MIDI,
then lighting, then haptics, then DIN MIDI, then CV/gate. Each phase must
leave the project building and the previous phase's safety guarantees
intact.

## Non-negotiables (see hardware docs for the full list)

- Never drive GP20 to a *high* level -- GP20 is the shared PCA9685 OE
  pin (see `board/board_pins.h`; the original hardware doc's "A5
  address strap" claim was wrong, corrected 2026-08-21 against the real
  fabricated board's flying-probe netlist). It's fine, and required, to
  drive it *low* to enable outputs -- `board_pca9685_enable_outputs()`
  does exactly that, but only once every channel is already configured.
- Only one Hall mux channel enabled across all three TCA9548A devices at a
  time.
- All LED mux banks disabled while changing selector bits.
- Both PCA9685 devices initialize all-off before any other output service
  starts.
- Never auto-select a USB power budget above 500mA.
- CV/gate stay off unless GP22 proves external power is selected.
- A failed subsystem disables itself; it never blocks USB diagnostics.

## Status

**Real hardware, not just simulated/reasoned-through.** As of
2026-08-21 this has been flashed to and run on an actual soldered Rev
A0 board: all 8 expected I2C devices ACK, underglow and all 24 pad LEDs
are lit white and touch-reactive, function buttons light while held,
and Hall gives live changing raw XYZ. USB MIDI has not yet had its
first on-hardware note-on test.

- `board/` — done: GPIO/I2C-address constants, the `PadConfig[24]`
  table (verified unique by `test/test_pad_config.c`), GPIO-safe-state
  + I2C-bus-init, plus `board_pca9685_enable_outputs()` (see
  non-negotiables above).
- `diagnostics/` — `i2c_scan` (expected-device scan, looped every 2s)
  and a full 0x08-0x77 bus scan (`tiles_diag_i2c_full_scan()`, not
  currently called from `main.c` -- it did its job finding the real
  PCA9685 addresses and is kept around for the next time something
  isn't where it's expected). `calibration` done for a capture-only V1:
  single-character serial commands over the same stdio channel capture
  and print rest-baseline/full-press/max-press Hall depth tables (+
  averages) across all 24 pads, for hand-picking real constants from --
  see `diagnostics/README.md`. Doesn't persist or derive/apply a
  calibration curve itself.
- `drivers/` — `sk6805`, `tca9554`, `tca9548a`, `tmag5273`, `pca9685`,
  `mpr121` all done and now verified against real silicon (register
  maps were datasheet-confirmed from the start; behavior confirmed
  on hardware 2026-08-21). Only `dac80502` (CV DAC) is not built.
- `services/` — `lighting`, `buttons`, `touch`, `hall` (raw XYZ + rest
  baseline + depth, touched-pad-priority scan) all done and
  hardware-verified. `pedal` done (sustain on by default, expression
  built but off) but not yet hardware-tested. `note_map` done: pad ->
  MIDI note layout confirmed against real hardware's stated pattern
  (pad 19 = lowest note, bottom-to-top/left-to-right), scale-mode
  architecture in place with only chromatic implemented, verified by
  `firmware/test/test_note_map.c`. Also owns an octave shift
  (+/-3 octaves) applied on top of the scale-derived note.
  `octave_control` done for V1: SW1 ("-")/SW2 ("+")'s default function is
  octave shift down/up (one octave per press), the active direction's
  LED showing the shift's magnitude via a distinct pattern (solid /
  slow pulse / triple-blink-then-hold) -- claims both buttons
  permanently via a new per-button LED override in `buttons.c`. First
  real-hardware run of this surfaced a genuine bug, since fixed: two of
  `set_button_led_level()`'s branches (the exact 0.0/1.0 endpoints) had
  the active-low polarity backwards, so "off" was actually driving the
  LED lit and "solid" was actually driving it dark -- see
  `services/README.md`'s `buttons.h` entry.
  `game_mode` done for V1: real, player-controlled snake and brick
  breaker (distinct from `standby`'s autonomous versions of the same two
  games below, which stay unchanged) -- hold SW3+SW4+SW5+SW6 to toggle a
  menu, touch pad 1 or 2 to launch Snake or Brick Breaker. Snake:
  SW1-SW4 = left/right/up/down, eats food to grow, wraps at the edges,
  dies on self-collision. Brick Breaker: SW1/SW2 move the paddle. Either
  game ending flashes underglow red/purple, then returns to the menu.
  Shares standby's rendering-ownership mechanism; `main.c` skips
  `tiles_standby_scan()` entirely while a game is active so the two
  can't fight over the same pads/buttons/underglow. See
  `services/README.md` for the full reasoning.
  `expression` done for V1: touch+Hall
  fusion deriving real velocity (from strike acceleration) and
  aftertouch (from press depth) -- see `services/README.md` for the
  state machine and the explicitly-unmeasured scaling constants.
  `power` done for V1: derives USB-only/external-only/both/fault from
  GP22 + TinyUSB's mounted state (debounced), exposing both a live
  accessor and a change-callback so haptics/CV-gate can plug in once
  they exist -- currently wired into `lighting.c`'s brightness ceiling
  and `haptics.c`'s voice ceiling, not yet hardware-tested against a
  real source-switch transition.
  `standby` done for a demo V1: after 1 minute idle, pads + buttons +
  underglow run one of 9 rotating ambient animations (diagonal wave,
  sharp center-out ring, complex shooting stars, an actual game of snake
  that eats food and grows, a blue/purple RGB showcase where underglow
  shows the same color as the pads, a graphic equalizer with per-column
  peak-hold, a circular underglow-only wave, brick breaker, and a
  scrolling "SENTIA - TILES -" marquee in a tiny pixel font), in
  randomized order (never repeating the current
  animation or the one before it), instead of reflecting touch state,
  until any touch/button/pedal activity exits it. After 15 minutes of
  total inactivity it drops further into power-saving: everything dark
  except the circle button pulsing gently. See `services/README.md` for
  the button-column/underglow-anchor mapping assumptions this still
  needs verified on real hardware, and for a real bug this rework fixed
  (standby pads were routing through the touch-driven idle-baseline
  floor, so they never actually reached true black).
  `boot_sequence` done for V1: a ~4-second power-on animation (white
  "rain" flooding down through the pad grid, fade to black, a slow
  smoothstep-eased magenta pulse across pads + underglow) that also
  re-captures Hall's rest baseline once the animation's given the
  sensors a few settled seconds, instead of only ever trusting the
  very-first-instant-of-boot capture. Function buttons are held dark for
  the entire sequence -- they can't show color at all, and even a plain
  white glow on them read as wrong. Reworked twice from real feedback
  (direction reversed and "jumpy" linear transitions replaced with eased
  ones; then buttons dropped from the sequence entirely after residual
  glow was still visible with only the magenta phase excluded).
  `haptics` done for
  V1: an overdrive
  spike then velocity-mapped kick on strike, then a hard cutoff -- a
  single click. Continuous aftertouch-mapped sustain while held is
  built but disabled (felt like continuous buzzing on real hardware,
  and the magnets aren't calibrated yet -- see `services/README.md`).
  Staggers actual
  motor starts >= 15ms apart (no added latency for normal single-note
  play) and enforces `power.c`'s voice ceiling -- see
  `services/README.md` for why real active braking isn't physically
  possible on this board's motor drive circuit (single low-side NMOS,
  no H-bridge) and what the closest achievable substitutes are for both
  attack and stop. Everything else (a real per-pad Hall calibration
  curve -- capture-only exists, see `diagnostics/`; DIN MIDI; CV/gate)
  not built.
- `midi/` — composite USB CDC+MIDI device done (see `midi/README.md`);
  note on/off with real velocity, poly aftertouch, and CC (sustain/
  expression) all wired up, single MIDI channel. Not yet verified with
  a real MIDI-receiving host. DIN MIDI and MPE channel allocation not
  built.
- `usb_vendor/`, `profiles/`, `storage/` still empty module skeletons.

Builds clean end-to-end against a real pico-sdk checkout (`cmake` +
`arm-none-eabi-gcc`; see `BUILD.md`) with zero warnings, and produces a
flashable `.uf2`.

**Known gaps / open items:**
- USB MIDI hasn't had a real on-hardware note-on/off test in a DAW or
  MIDI monitor yet -- the composite descriptor builds and the device
  should enumerate, but "builds clean" and "a host actually receives
  the right notes" are different claims until checked.
- The LED brightness ceiling and idle-baseline/underglow percentages
  (now sourced from `services/power.c`'s per-mode state rather than a
  single hardcoded constant) are still engineering estimates -- tuned
  once against real hardware for "too dim" feedback on USB power, but
  neither the USB-only nor the external-power ceiling has been measured
  against an actual current budget.
- `services/power.c`'s GP22-derived mode has only been reasoned through
  against the documented truth table, not exercised on real hardware
  yet -- no test has actually unplugged USB, plugged in external 12V,
  or forced the FAULT combination to confirm the debounce and the
  resulting mode/ceiling actually behave as designed.
- `services/standby.c`: on real hardware, entering standby works and
  buttons/pedal reliably wake it, but MPR121 touch alone does NOT
  reliably wake it while the animation is running -- root cause
  unconfirmed, most likely candidate is the pad-LED SK6805 chain's
  continuous ~25fps rewrite interfering with capacitive sensing (see
  `services/README.md`'s standby entry for the fuller history). A Hall-
  depth wake fallback exists in the code but is currently disabled
  (`TILES_STANDBY_HALL_WAKE_ENABLED 0`) because the magnets aren't in
  their final position yet, making hall.c's baseline/depth meaningless
  until the mid-plate assembly is done. The button-column and
  underglow-anchor mappings (`board/board_layout.h`) are based on the
  user's verbal description of the physical board rather than a hardware
  doc -- confirmed absent from `docs/hardware/`. The 1-minute idle
  timeout, 2-minute animation-cycle period, and 15-minute power-saving
  timeout are explicit demo-mode defaults, not final values. Animations
  1-5 and the button-brightness/standby-baseline-floor fixes are based on
  user feedback from watching an earlier version on real hardware, but
  the two newest animations (6, the graphic equalizer; 7, the circular
  underglow wave) and the power-saving state have NOT been seen on real
  hardware at all yet -- every one of their constants
  (`EQ_PEAK_DECAY_PER_MS`, `CIRCLE_PERIOD_MS`,
  `POWER_SAVING_PULSE_PERIOD_MS`, etc.) is a first guess.
  `BUTTON_STANDBY_BRIGHTNESS_SCALE` (0.35) is likewise still an
  unmeasured guess at how much dimmer buttons need to be, not a measured
  match to pad brightness.
- `services/boot_sequence.c`: seen on real hardware twice now, reworked
  both times (direction/pacing, then buttons dropped entirely after
  residual glow was still visible with only the magenta phase
  excluded) -- this latest version hasn't itself been flashed and
  watched yet, so every duration/edge-width constant is again a first
  guess pending that. The Hall baseline re-capture at the end is a real
  mechanism (reuses `hall.c`'s existing per-pad read path), but whether
  ~4 seconds is actually enough settling time to matter is unverified.
- `services/octave_control.c`: real hardware testing already found and
  fixed one genuine bug, not just an untuned constant --
  `buttons.c`'s `set_button_led_level()` had its active-low endpoints
  backwards (see `services/README.md`'s `buttons.h` entry). Every LED
  pattern timing constant (`PULSE_PERIOD_MS`, the blink/hold durations)
  is still a first guess, and whether the three patterns read as
  visually distinct at a glance is unverified.
- `services/game_mode.c` has not been hardware-tested at all -- the
  entry-gesture hold duration, every step-timing constant for both
  games, and the round-end flash timing are first attempts. Whether
  snake's edge-wrap (chosen over instant wall-death as friendlier on
  such a small board) actually feels right in practice, and whether
  holding 4 buttons simultaneously is comfortable/reliable to do on the
  real hardware at all, are both open questions this hasn't been able
  to answer yet.
- `services/haptics.c` has not been hardware-tested at all -- every
  duty/timing constant (kick duration, overdrive duration, gap duration,
  min kick duty, max sustain duty, stagger gap) is an unmeasured
  placeholder. True active braking isn't physically possible on this
  board (single low-side NMOS per motor, no H-bridge, no haptic driver
  IC) -- the GAP phase's hard cutoff is the closest achievable
  substitute for stopping quickly, and the overdrive spike at the start
  of every kick is the closest achievable substitute for starting
  quickly; neither is a compromise made for convenience, both are the
  real limits and real techniques of this specific drive circuit.
- `diagnostics/calibration.c`'s rest/full-press/max-press snapshots are
  useful for reading real numbers off the terminal, but nothing yet
  turns those numbers into an applied per-pad calibration curve --
  picking new constants (e.g. `expression.c`'s
  `DEPTH_TO_AFTERTOUCH_FULL_SCALE`) from what it prints is still a
  manual step, and there's no persistence (`storage/` doesn't exist) so
  a capture doesn't survive a reboot.
- `sk6805.pio`'s bit timing mirrors Raspberry Pi's reference `ws2812.pio`
  program; confirmed working on real SK6805 parts (all pads + underglow
  show correct white output on hardware), not independently verified on
  a scope.
- `hall.c` now prioritizes touched pads (scanned every call) over the
  background round-robin for untouched ones, and the main loop's
  throttle was cut from `sleep_ms(10)` to `sleep_ms(1)` specifically so
  `services/expression.c`'s strike-detection window gets more samples
  -- but the actual achieved sample rate for a touched pad is still
  unmeasured (depends on real I2C transaction timing, and on how many
  other pads are touched at once). This is a direction, not a verified
  number.
- No per-pad axis-selection or calibration curve exists yet -- Z is
  assumed to be the vertical-press axis for every pad (unverified per-
  pad), and `hall.c`'s depth is a raw, uncalibrated magnitude with no
  dead zone, offset correction, or saturation margin.
- `services/expression.c`'s acceleration->velocity scale, depth->
  aftertouch range, and strike-detection window durations are all
  explicitly-flagged placeholder constants -- there is no calibrated
  mT/LSB relationship to derive them from yet, so they're starting
  guesses that will need real-hardware tuning once this can actually be
  played and heard.
- MPR121 touch thresholds (12/6) are Freescale's generic quickstart
  defaults, not tuned for this board's actual electrode/keycap/acrylic
  stack. Touch works and feels responsive on hardware as of the
  latency fix in `services/lighting.c`, but sensitivity tuning is still
  open.
- No MPE (per-note channel allocation) yet -- V1 MIDI is single
  channel. Velocity and aftertouch are now real (Hall-derived), not
  fixed, but unverified against actual playing since USB MIDI itself
  hasn't been hardware-tested.
