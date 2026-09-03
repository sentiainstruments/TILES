# diagnostics/

Command handlers exposed over `usb_vendor/`: list I2C devices, read power
source/USB connection state, select one logical pad and stream raw or
calibrated data, set one pad LED, pulse one motor at bounded duration/duty,
read buttons/pedal, save/inspect/invalidate/erase calibration, run
manufacturing per-key tests. Never gated by whether MIDI or other
higher-level services are running — a failed sensor module must not take
diagnostics down with it.

## Status

- `i2c_scan.h`/`.c` — probes every expected I2C0/I2C1 device address and
  prints pass/fail per device. This is the "I2C discovery" bring-up
  phase. Currently printed over the temporary USB-CDC stdio enabled in
  `src/CMakeLists.txt`; rewire to `usb_vendor/` once that exists instead
  of adding a second output path.
- `calibration.h`/`.c` — a capture-only stand-in for the real
  save/inspect/invalidate/erase calibration flow above, over the same
  temporary stdio channel as `i2c_scan`: single-character serial
  commands ('r' recapture rest baseline, 'f'/'m' snapshot regular-full-
  press/max-press depth against that baseline) print per-pad tables +
  averages for a human to read and hand-pick real constants/defaults from
  (e.g. `services/expression.c`'s aftertouch full-scale depth default,
  `services/standby.c`'s `TILES_STANDBY_HALL_WAKE_DEPTH`). Doesn't
  persist anything or derive/apply a calibration curve itself --
  `storage/` and a real per-pad curve don't exist yet.
  First real capture session with all 24 magnets seated (this console
  only became usable at all once `main.c` started calling `tud_task()`
  every loop -- see `midi/usb_device.h`'s entry, without it the
  USB-CDC channel this tool runs over produced no output at all): 'r'
  gave a clean rest baseline for every pad, 'f' (done row-by-row rather
  than all 24 at once -- one person can't press all 24 simultaneously)
  measured 784-1184 raw depth across all 24 pads, average 918, at what
  turned out to already be each pad's mechanical bottom-out (no
  meaningfully-different "harder" position past a normal full press, so
  the 'm' max-press step wasn't needed this session). Used to set
  `expression.c`'s aftertouch full-scale depth default to 900 (now a
  runtime value, adjustable live via `services/expression_control.h`'s
  sub-menu row 4 -- see `services/README.md`'s `expression.h` entry for
  the reasoning). `TILES_STANDBY_HALL_WAKE_DEPTH`
  has NOT been picked from this data yet and Hall-wake stays disabled --
  a wake threshold needs a light-touch data point this session didn't
  capture (only rest and full-press), not just re-running the same two
  numbers.
  **Standing rule since unit 2's own capture session, real feedback:
  "calculate two values. no press and strong strke... add this as
  calibration rules on github":** only rest baseline ('r') + one strong
  strike ('m') per pad from here on, not a separate "regular full press"
  ('f') step -- see `calibration.h`'s own header for the full reasoning
  (unit 2's sampled pad 1 showed a huge 33-vs-1697 gap between a normal
  press and a real hard strike, confirming "regular" isn't a
  standardizable target the way "as hard as it goes" is). Unit 2's own
  first sampled pad this way: pad 1, baseline z=384, strong-strike
  depth=1697 -- one pad so far, session moved on before covering the
  rest of the originally-planned 4-corner sample (1, 6, 19, 24); pick
  this back up before trusting any constant derived from it.
- Everything else in this list (per-pad commands beyond calibration
  capture, calibration save/erase, manufacturing tests) is not built
  yet.
