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
  (+/-3 octaves) and a key-center transpose offset (0-11 semitones,
  wraps) applied on top of the scale-derived note.
  `octave_control` done for V1: SW1 ("-")/SW2 ("+")'s default function is
  octave shift down/up (one octave per press), the active direction's
  LED showing the shift's magnitude via a distinct pattern -- all three
  magnitudes now built from one shared raised-cosine pulse shape after
  real feedback that they didn't pulse evenly with each other, see
  `services/README.md`'s entry for the full rework history -- claims
  both buttons permanently via a per-button LED override in
  `buttons.c`. First real-hardware run of this surfaced a genuine bug,
  since fixed: two of `set_button_led_level()`'s branches (the exact
  0.0/1.0 endpoints) had the active-low polarity backwards, so "off" was
  actually driving the LED lit and "solid" was actually driving it dark
  -- see `services/README.md`'s `buttons.h` entry.
  Also owns transpose mode: holding SW1+SW2 together toggles a mode
  where "-"/"+" step the key center instead of the octave, both LEDs
  pulse together, and the pad grid shows the current key's letter (via
  the shared `pixel_font.h`/`.c`, now a proper 4x4 grid per glyph styled
  after a user-supplied reference font), flashing a cross for sharp keys
  since a 4x4 glyph can't draw "#" -- the cross itself is now also
  contained in a 4x4 box (real feedback that its horizontal arm was
  originally too long). Not yet hardware-tested.
  `game_mode` done for V1: real, player-controlled snake, brick breaker,
  Tetris, and Pong (distinct from `standby`'s autonomous versions of the
  same four games below, which stay unchanged) -- hold SW3+SW4+SW5+SW6
  to toggle a menu, touch pad 1/2/3/4 to launch Snake/Brick
  Breaker/Tetris/Pong.
  Snake: SW1-SW4 = left/right/up/down, eats food to grow, wraps at the
  edges, dies on self-collision. Brick Breaker: SW1/SW2 move the paddle.
  Tetris: SW1/SW2 move the piece, SW3 rotates (2 states per piece, no
  wall kicks), SW4 hard-drops; a custom 5-piece small set (dot, domino,
  3-cell straight/corner trominoes, 2x2 square) replaces the standard 7
  tetrominoes, which real feedback said were too big for this board; a
  line clear flashes underglow white dramatically, topping out flashes
  it plain red (both real feedback).
  Pong: two players on one board, column 1/6 paddles (SW1/SW2 up/down
  left, SW5/SW6 up/down right -- unverified guess at which button the
  user meant by "circle and the other one"), blue ball, first to 2
  points wins (real feedback: it wasn't tracking a winner at all). Score
  shows as a breathing glow on each side's own control buttons (0/1/2
  points = 0/1/2 buttons lit); a non-winning miss flashes white and
  re-serves immediately, but reaching 2 points freezes the board and
  returns to the game menu after a couple of seconds instead (real
  feedback: don't reset immediately).
  Every other game's ending flashes underglow red/purple, then returns
  to the menu.
  Since Tetris/brick breaker/Pong reuse SW1/SW2, `octave_control.c` now
  checks `tiles_game_mode_is_active()` and skips its own action logic
  entirely while a game owns the buttons -- without this, every in-game
  left/right press would have also silently stepped the octave or
  transpose key underneath the game (a real gap the previous transpose-
  mode work exposed, not just an untuned constant).
  Shares standby's rendering-ownership mechanism; `main.c` skips
  `tiles_standby_scan()` entirely while a game is active so the two
  can't fight over the same pads/buttons/underglow. See
  `services/README.md` for the full reasoning.
  `expression` done for V1: touch+Hall
  fusion deriving real velocity (from elapsed time to reach a real
  press, not acceleration -- see `services/README.md` for why that
  changed), aftertouch (from press depth), and optional pitch bend
  (from sideways Hall motion while held, toggled via the square/"sentia"
  button -- see `services/expression_control.h`) -- see
  `services/README.md` for the state machine and the
  explicitly-unmeasured scaling constants.
  `power` done for V1: derives USB-only/external-only/both/fault from
  GP22 + TinyUSB's mounted state (debounced), exposing both a live
  accessor and a change-callback so haptics/CV-gate can plug in once
  they exist -- currently wired into `lighting.c`'s brightness ceiling
  and `haptics.c`'s voice ceiling, not yet hardware-tested against a
  real source-switch transition.
  `standby` done for a demo V1: after 1 minute idle, pads + buttons +
  underglow run one of 13 rotating ambient animations (see
  `services/README.md` for the full list, including Snake/Tetris/Pong
  "attract mode" demos and a scrolling "TILES -" marquee in a tiny pixel
  font), weighted so plain ambient ones show up roughly twice as often as
  the game demos and never immediately repeating the current animation or
  the one before it, instead of reflecting touch state, until any
  touch/button/pedal activity exits it. After 15 minutes of total
  inactivity it drops further into deep sleep: everything dark except the
  circle button pulsing slowly, the one indicator it's in this state.
  Holding the circle button (SW6) for 6s manually forces the screensaver
  on early and repurposes SW1/SW2 as scroll-without-waking animation
  controls (a longer, 20-minute deep-sleep timeout applies while in this
  manual mode); holding it further, to 10s, escalates straight into that
  *same* deep sleep state directly -- not a separate blank state, per
  real feedback that the two should be one and the same thing. Circle is
  this board's shift/power button and stays scoped to exactly these two
  gestures -- an earlier pass built a pitch-bend-toggle + haptic-shift
  "modifier" role directly onto circle here, on a misreading of which
  physical button "sentia" is; real feedback corrected it ("our shift
  and power button is circle. sentia is square button") and that
  behavior was moved out to `services/expression_control.h`/`.c`
  entirely -- SW5/square alone (short click, held-shift, and a
  held-3-seconds-alone toggle for a fuller expression sub-menu), plus a
  separate circle+square combo held 3 seconds for expression mute -- see
  that file's own writeup. See
  `services/README.md` for the button-column/underglow-anchor mapping
  assumptions this still needs verified on real hardware, and for a real
  bug this rework fixed (standby pads were routing through the
  touch-driven idle-baseline floor, so they never actually reached true
  black).
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
  spike then velocity-mapped kick on strike (boosted -- real feedback
  it was "too soft for the touch"), then a hard cutoff, then a
  continuous SUSTAIN blending that strike's velocity with ongoing
  pressure/key travel (a mix, pressure-dominant, with a fast-attack/
  slow-release feel on the applied motor duty) -- re-enabled and
  reworked from an earlier aftertouch-only design that had been
  disabled for reading as continuous buzzing, now that the magnets are
  seated and aftertouch itself is calibrated -- see
  `services/README.md`. Staggers actual
  motor starts >= 15ms apart (no added latency for normal single-note
  play) and enforces `power.c`'s voice ceiling by **stealing the oldest
  active pad's haptic voice** (FIFO, oldest first) rather than dropping
  the new strike -- real feedback: "additional notes pressed after the
  limit of haptic voices steal the first voices pressed so new notes
  always have priority." Stealing only cuts the stolen pad's motor; its
  MIDI note keeps sounding unaffected -- see `services/README.md` for
  why real active braking isn't physically
  possible on this board's motor drive circuit (single low-side NMOS,
  no H-bridge) and what the closest achievable substitutes are for both
  attack and stop. Real feedback: haptics don't always activate on
  touch -- prime suspect is `power.c`'s `TILES_POWER_MODE_FAULT` (a
  hard 0-voice ceiling, silently drops every kick), an untested-on-
  hardware GP22-derived mode that could be flickering transiently; a new
  `printf` on every dropped kick (mode/ceiling/active-voice-count) is
  meant to confirm this next session, see `services/README.md`. Also
  drives a separate, lighter touch-only pulse (independent of the
  note-strike kick, fired on capacitive contact alone), a global
  intensity scalar adjustable via the square button's "shift" gesture,
  and a hard expression-mute kill switch
  (`services/expression_control.h`'s circle+square 3-second combo) --
  see `services/README.md` for all three.
  Everything else (a real per-pad Hall calibration
  curve -- capture-only exists, see `diagnostics/`; DIN MIDI; CV/gate)
  not built.
- `midi/` — composite USB CDC+MIDI device done (see `midi/README.md`);
  note on/off with real velocity, poly aftertouch, CC (sustain/
  expression), and pitch bend all wired up, single MIDI channel --
  pitch bend is channel-wide by MIDI's own spec, a real limitation
  without MPE that `services/expression.c` works around with a single
  "owner pad" concept, see `services/README.md`. Not yet verified with
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
  the right notes" are different claims until checked. There is real
  reason to suspect it may not have worked at all until just now: while
  chasing why the USB-CDC debug console printed nothing on real
  hardware, found that `main.c` never called `tud_task()` anywhere --
  the function that actually services the USB stack (control transfers,
  moving CDC/MIDI data to and from the hardware). pico-sdk's automatic
  background-IRQ servicing for this is compiled out by design whenever
  an app links `tinyusb_device` directly and supplies its own
  descriptors (confirmed by reading pico-sdk's own `pico/stdio_usb.h`),
  exactly this project's setup -- the app is expected to call
  `tud_task()` itself, and nothing did. Now fixed (`tud_task()` called
  at the top of every main-loop iteration, see `main.c`), but not yet
  confirmed on real hardware whether this also explains any past MIDI
  unreliability -- plausible given `tud_midi_stream_write()` just queues
  bytes that need `tud_task()` to actually reach the host, but unproven
  until tested in a DAW.
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
  timeout, 2-minute animation-cycle period, and 15-minute deep-sleep
  timeout are explicit demo-mode defaults, not final values. Animations
  1-3, 5-7 and the deep sleep state are based on user feedback from
  watching an earlier version on real hardware (animation 3's fall speed
  most recently halved after still reading as too fast); animation 4's
  real-snake rework and animations 8 (brick breaker), 9 (marquee,
  including its font's move to the new shared `services/pixel_font.h`/
  `.c`), 10 (bouncing glow, a "simple but elegant" white animation),
  11 (Tetris, AI-placed via a greedy landing-depth heuristic), 12
  (Pong, AI-vs-AI), and 13 (falling dots, a slow "filling up"
  screensaver) have NOT been seen on real hardware at all yet --
  every one of their constants
  (`EQ_BEAT_MS`, `CIRCLE_PERIOD_MS`,
  `DEEP_SLEEP_PULSE_PERIOD_MS`, `BOUNCE_ROW_PERIOD_MS`,
  `TETRIS_STEP_MS`, `FALLINGDOTS_STEP_MS`, etc.) is a first guess. `BUTTON_STANDBY_BRIGHTNESS_SCALE`
  (0.35) is likewise still an unmeasured guess at how much dimmer buttons need to be, not a measured
  match to pad brightness. The circle-button (SW6) 6s/10s long-press
  gestures, the deep sleep consolidation, and manual scroll-through-animations
  mode are all brand new and untested on real hardware -- both hold
  thresholds and the 20-minute manual deep-sleep timeout are first
  guesses, and whether excluding SW1/SW2 (and circle itself) from the
  wake check while scrolling actually feels right in practice (versus,
  say, accidentally exiting scroll mode) hasn't been observed yet.
  Square's "sentia" role (short click toggles pitch bend, hold alone +
  SW1/SW2 steps the expression sub-menu's haptics column, its own LED
  reflecting the toggle state -- see `services/expression_control.h`) is
  also completely untested on real hardware -- `SQUARE_LED_TOGGLE_ON_
  LEVEL`'s "close to full brightness, not by a lot" is a first guess. The
  expression sub-menu itself (4 rows of pad sliders for haptics/
  pitch-bend/aftertouch sensitivity, one reserved row, toggled open/
  closed by holding square alone for 3 seconds) and the separate
  circle+square 3-second-hold expression mute are equally untested -- the
  column-to-value mapping for every row, row 1's off/blink indicator at
  column 1, the mute LED's blink pacing, and even whether the Sentia
  Magenta selected-pad color reads clearly against the sub-menu's
  otherwise-dark grid are all first attempts, not measurements.
- `services/boot_sequence.c`: seen on real hardware twice now, reworked
  three times total -- direction/pacing, then buttons dropped entirely
  after residual glow was still visible with only the magenta phase
  excluded, then buttons brought back into the rain/fade phases (only
  still excluded from the magenta pulse) after that turned out to be an
  overcorrection. This latest version hasn't itself been flashed and
  watched yet, so every duration/edge-width constant is again a first
  guess pending that. The Hall baseline re-capture at the end is a real
  mechanism (reuses `hall.c`'s existing per-pad read path), but whether
  ~4 seconds is actually enough settling time to matter is unverified.
- `services/octave_control.c`: real hardware testing already found and
  fixed one genuine bug, not just an untuned constant --
  `buttons.c`'s `set_button_led_level()` had its active-low endpoints
  backwards (see `services/README.md`'s `buttons.h` entry). The pulse
  pattern has since been reworked three times from real feedback (see
  `services/README.md`'s entry for the full history) and every timing
  constant is still a first guess. Transpose mode (SW1+SW2 held together,
  key-center display via the new `services/pixel_font.h`/`.c`) is brand
  new and has not been hardware-tested at all -- the combo-hold
  threshold, flash timings, and font legibility at actual LED
  brightness are all unverified. Also newly gates on
  `tiles_game_mode_is_active()` so it stops consuming SW1/SW2 presses
  while a game owns them -- see `services/game_mode.c`'s entry below for
  the bug this fixes; the gate itself hasn't been hardware-tested.
- `services/game_mode.c` has not been hardware-tested at all -- the
  entry-gesture hold duration, every step-timing constant for all four
  games, and the round-end flash timing are first attempts. Whether
  snake's edge-wrap (chosen over instant wall-death as friendlier on
  such a small board) actually feels right in practice, and whether
  holding 4 buttons simultaneously is comfortable/reliable to do on the
  real hardware at all, are both open questions this hasn't been able
  to answer yet. Tetris is brand new: its simplified 2-state rotation
  (no wall kicks), SW1-4 control mapping, colors, and its white
  line-clear / red topping-out flashes are all unverified. Pong is also
  brand new and carries the most speculative control mapping of any
  game here -- SW5/SW6 for the right paddle is a guess at what the user
  meant by "circle and the other button next to it"; if wrong, it's a
  one-line fix in `gp_handle_input()`/`gm_handle_menu_selection()`'s
  pad-4 wiring, not a structural change.
  Building Tetris also surfaced a real, previously-unnoticed bug
  predating it -- `octave_control.c`'s SW1/SW2 handling ran
  unconditionally regardless of game mode, so every in-game left/right
  press (already true for Snake and Brick Breaker, just far more
  noticeable once Tetris made SW1/SW2 the primary controls) was also
  silently stepping the octave/transpose key underneath the game. Now
  fixed (see `services/octave_control.c`'s entry above), but the fix
  itself hasn't been seen on real hardware.
- `services/haptics.c`: the pre-boost kick was tried on real hardware --
  it fired and felt like the intended single click -- but *not
  reliably*: real feedback was it didn't "activate always." Every
  duty/timing constant (kick duration, overdrive duration, gap
  duration, min kick duty, max sustain duty, stagger gap, and the new
  sustain-mix/slew constants) is still an unmeasured placeholder, and
  the intermittent-activation root cause is unconfirmed -- prime
  suspect is `power.c`'s untested `TILES_POWER_MODE_FAULT` silently
  zeroing the voice ceiling if it flickers in transiently; a `printf` on
  every dropped kick should confirm or rule this out next session (see
  `services/README.md`'s `haptics.h` entry). The kick has since been
  boosted a lot (real feedback it was too soft) and SUSTAIN re-enabled
  as a velocity+pressure mix with its own attack/release feel -- neither
  of those changes has been tried on real hardware at all yet. Voice
  stealing (a new kick past the ceiling now steals the oldest active
  pad's haptic voice, FIFO oldest-first, instead of being dropped -- see
  `services/README.md`'s `haptics.h` entry) is likewise brand new and
  untested; whether stealing a still-held note's haptic feedback mid-hold
  actually feels acceptable in practice, versus jarring, hasn't been
  observed yet. Real feedback since reported haptic feedback missing
  entirely, raised three times with increasingly specific wording
  ("haptic pulse on touch without pressure") until it became clear this
  meant a distinct touch-only tick was wanted, separate from the
  note-strike kick -- not a report that the existing kick mechanism was
  broken (review of the voice-ceiling path and shared PCA9685 wiring
  against three clean debug captures never found a code-level cause for
  that). A new `HAPTIC_PHASE_TOUCH_PULSE`, fired the instant capacitive
  touch is detected regardless of whether a real press follows, is now
  in place (see `services/README.md`'s `haptics.h` entry) -- untested on
  real hardware; the duty (0.35, the same value already proven too weak
  for the kick before it was raised to 0.65+) that duty had at first was
  a real, confirmed-in-a-debug-capture bug, since raised to 0.6, but
  that fix itself hasn't been re-verified yet. A new global intensity
  scalar (`tiles_haptics_adjust_intensity()`, driven by
  `services/expression_control.c`'s square-button "shift" gesture)
  applies uniformly to every effect -- also untested. True active braking isn't
  physically possible on this board (single low-side NMOS per motor, no
  H-bridge, no haptic driver IC) -- the GAP phase's hard cutoff is the
  closest achievable substitute for stopping quickly, and the overdrive
  spike at the start of every kick is the closest achievable substitute
  for starting quickly; neither is a compromise made for convenience,
  both are the real limits and real techniques of this specific drive
  circuit.
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
- `services/expression.c`'s strike detection went through eight rounds
  of real-hardware feedback, each catching a genuine bug or forcing a
  real architectural change rather than a constant tweak -- see
  `services/README.md`'s `expression.h`/`.c` entry for the full blow-by-
  blow (a stale-reference bug, a "check the peak not the instant" bug
  plus new retrigger-without-lifting, a capacitive touch-bounce fix, and
  a reported haptic-feedback loss that review couldn't trace to a code
  cause). Round 5 replaced the velocity model entirely: "max sudden push
  does not trigger notes properly, light presses trigger randomly hard,
  the logic and measurement method is not working" was a verdict on
  acceleration itself, not its constants -- a double-difference over
  only 3 Hall samples is too sensitive to exact sample timing and
  depth's own coarse quantization. Velocity is now elapsed TIME between
  two fixed points of travel, the same technique real weighted-action
  keyboards and drum pads use. Rounds 6 and 7 then found two more real
  bugs in how the reference point for "how far pressed" was captured --
  both traced to the same root mistake (a *delta from a per-touch
  reference*, rather than the raw depth `hall.c` already provides
  baseline-relative), and both hitting hardest exactly on the fastest,
  hardest strikes: "if I press really fast and hard nothing happens,"
  then, after round 6's partial fix, "sudden full force press is not
  triggering the notes... touch is detected... just no midi." A
  debug capture logging depth on *every* cancelled release (not just
  successful commits) caught it directly: the reference depth at
  touch-down was sometimes already 880-1040 -- essentially full
  mechanical compression -- because a hard enough strike can finish
  faster than capacitive touch detection catches up. Fixed by dropping
  the per-touch reference entirely: raw depth's own running peak is now
  compared straight against the actuation threshold, so an
  already-fully-compressed first reading correctly registers as an
  instantaneous, max-velocity strike instead of starting its own delta
  at 0. The same stretch also deepened `MIN_STRIKE_DEPTH_DELTA`
  (150 -> 300) since a shallow checkpoint let a fast-but-light flick
  read as a hard strike, and added a genuinely new capability -- a
  light touch-only haptic pulse, independent of the note-strike kick,
  after "haptic pulse on touch without pressure" was raised three times
  with increasingly specific wording (see `services/README.md`'s
  `haptics.h` entry).
  Once strike detection and velocity felt solid ("it all feels fine for
  now"), round 8 added pitch bend from sideways Hall motion while a note
  is held -- real feedback explicitly asked that the math be solid
  before implementing: "should compensate for vertical movement in
  magnet and drift from aftertouch." A naive raw-X measurement would
  fail that directly, since a magnetic dipole's field strength changes
  with press depth even with zero real lateral motion; fixed by working
  with the field's direction (X divided by total magnitude, a distance-
  invariant direction cosine) instead of its raw magnitude -- the same
  principle real 3-axis Hall-effect joysticks use. Toggled via a genuine
  square-button ("sentia") short click (an earlier pass wired this to
  circle by mistake before real feedback corrected which physical button
  "sentia" is -- see `services/expression_control.h`), with only one pad
  ever "owning" the shared MIDI channel's bend at a time (this project
  has no MPE yet, so pitch bend is unavoidably channel-wide -- see
  `midi/README.md`). Not yet
  hardware-verified at all -- see `services/README.md`'s `expression.h`
  entry for the full physics reasoning.
- MPR121 touch thresholds started at Freescale's generic quickstart
  defaults (12/6), not tuned for this board's actual electrode/keycap/
  acrylic stack. The release side was since narrowed to 9 -- real
  feedback with the assembly now seated: "release is sticking... should
  release as fast as a keyboard piano" (`services/expression.c` sends
  MIDI note-off the same tick touch clears, so the sluggishness traced
  back to the raw touch/release hysteresis itself, not anything
  downstream -- see `drivers/README.md`'s `mpr121.h`/`.c` entry). Touch
  works and feels responsive on hardware as of the latency fix in
  `services/lighting.c`, but this narrower release threshold hasn't
  itself been tried on real hardware yet, and full per-electrode
  sensitivity tuning is still open.
- No MPE (per-note channel allocation) yet -- V1 MIDI is single
  channel. Velocity and aftertouch are now real (Hall-derived), not
  fixed, but unverified against actual playing since USB MIDI itself
  hasn't been hardware-tested.
