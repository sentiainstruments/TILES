# services/

The behavioral layer: turns raw driver data into musical/expressive
events, and turns high-level intent into driver commands. Depends on
`drivers/` and `board/`; knows nothing about USB/MIDI transport.

Planned services: Hall scan + per-pad filtering/calibration, touch event
fusion (touch + Hall, not touch alone), expression mapping (press depth →
velocity/pressure, X/Y tilt → pitch/timbre), lighting (pad LED mux
sequencing + underglow), haptics (voice/duty allocation + current
governor), pedal input, power/current governance across profiles,
calibration capture and storage glue.

This is also where the legacy prototype's *behaviors* (scale modes, voice
stealing, standby animation, haptic confirm clicks — see
`../../../docs/reference/legacy-prototype-v1/`) get redesigned for 24 pads,
not its code.

## Status

- `lighting.h`/`.c` — done for the V1 default behavior in
  `docs/architecture/defaults-and-safeguards.md` ("LED color and
  brightness"): underglow solid white at a fixed idle baseline (written
  once at init, never changes), all 24 pads solid white at idle baseline
  by default, brightening toward the ceiling when `touch.c` reports that
  pad touched. Brightness hard-clamped to a hardcoded USB-only ceiling
  (no power-profile governor yet).
  **Not done:** standby animations (needs its own design pass — see the
  defaults doc), any power-profile awareness beyond the hardcoded
  ceiling, Hall-driven (as opposed to touch-driven) brightness.
- `buttons.h`/`.c` — done for V1: reads all 6 function buttons
  (debounced, 10ms), lights each one's PCA9685-driven LED while (and
  only while) it's held. Owns both physical PCA9685 chips; see the file
  header for the ownership question this raises once haptics needs the
  same two chips for the 24 motor channels.
- `touch.h`/`.c` — done for V1: reads both MPR121 controllers, derives
  each pad's touched state from its board-map touch route, and pushes
  that straight into `lighting.c`'s per-pad brightness (touched = full
  ceiling, untouched = idle baseline). No touch+Hall fusion into real
  velocity/pressure yet -- that's a later layer.
- `hall.h`/`.c` — done for V1: round-robins all 24 pads' TMAG5273
  sensors through their Hall mux channels (one pad serviced per
  `tiles_hall_scan()` call), storing raw uncalibrated X/Y/Z. Structurally
  enforces "only one Hall mux channel across all three TCA9548A devices
  at a time" (every selection disables all three first). A pad whose
  sensor fails identify/init at boot is skipped by future scans rather
  than blocking the other 23.
  **Not done:** deciding which raw axis is actually vertical press depth
  per pad, calibration (rest/half/bottom capture, offsets, dead zones),
  any filtering, and the ~120Hz full-sweep rate target isn't measured or
  tuned yet -- see `firmware/README.md`'s known gaps.
- Everything else (touch+Hall fusion, expression mapping, haptics,
  pedal, power governance, calibration) is not built yet.
