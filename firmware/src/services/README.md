# services/

The behavioral layer: turns raw driver data into musical/expressive
events, and turns high-level intent into driver commands. Depends on
`drivers/` and `board/`; knows nothing about USB/MIDI transport.

Built: Hall scan, touch, lighting, buttons, pedal, note mapping, and
touch+Hall expression fusion (velocity/aftertouch) -- see Status below.
Still planned: per-pad Hall calibration, X/Y tilt -> pitch/timbre,
haptics (voice/duty allocation + current governor), power/current
governance across profiles, storage glue.

This is also where the legacy prototype's *behaviors* (scale modes, voice
stealing, standby animation, haptic confirm clicks — see
`../../../docs/reference/legacy-prototype-v1/`) get redesigned for 24 pads,
not its code.

## Status

- `lighting.h`/`.c` — done for the V1 default behavior in
  `docs/architecture/defaults-and-safeguards.md` ("LED color and
  brightness"): underglow solid white, brighter than pad idle baseline
  (separately tunable, both confirmed working and legible on real
  hardware), all 24 pads solid white at idle baseline by default,
  brightening toward the ceiling when `touch.c` reports that pad
  touched -- written immediately on a press-value change rather than
  waiting for the round-robin, so touch reads as responsive rather than
  laggy. Brightness hard-clamped to a hardcoded USB-only ceiling (no
  power-profile governor yet).
  **Not done:** standby animations (needs its own design pass — see the
  defaults doc), any power-profile awareness beyond the hardcoded
  ceiling, Hall-driven (as opposed to touch-driven) brightness.
- `buttons.h`/`.c` — done for V1: reads all 6 function buttons
  (debounced, 10ms), lights each one's PCA9685-driven LED while (and
  only while) it's held. Owns both physical PCA9685 chips; see the file
  header for the ownership question this raises once haptics needs the
  same two chips for the 24 motor channels.
- `touch.h`/`.c` — done: reads both MPR121 controllers, derives each
  pad's touched state from its board-map touch route, pushes that into
  `lighting.c`'s per-pad brightness (touched = full ceiling, untouched
  = idle baseline). Touch state + lighting only -- MIDI now lives in
  `expression.c`, which reads `tiles_touch_is_touched()` itself rather
  than touch.c reaching into MIDI.
- `hall.h`/`.c` — done for V1: scans all 24 pads' TMAG5273 sensors
  through their Hall mux channels, storing raw XYZ plus a per-pad rest-Z
  baseline (captured once at init) and a derived depth magnitude
  (`tiles_hall_get_depth()`, `|z - baseline|`, sign-agnostic since the
  actual sensor polarity per pad is still unknown). Scan priority: a
  touched pad is read every `tiles_hall_scan()` call; untouched pads
  round-robin in the background -- a pure round-robin only reaches a
  given pad every ~24 calls, nowhere near fast enough to catch a 30-80ms
  strike, which `expression.c` needs. Structurally enforces "only one
  Hall mux channel across all three TCA9548A devices at a time." A pad
  whose sensor fails identify/init at boot is skipped by future scans
  rather than blocking the other 23.
  **Not done:** deciding which raw axis is actually vertical press depth
  per pad (Z is assumed for all pads, unverified), per-pad calibration
  curve (offsets, dead zones, saturation margin), any use of X/Y
  (tilt/lateral). Actual achieved scan rate for touched pads is
  unmeasured -- see `firmware/README.md`'s known gaps.
- `note_map.h`/`.c` — done: maps a logical pad to a MIDI note. Physical
  layout confirmed against real hardware: pad 19 (bottom-left) is the
  lowest note, ascending left-to-right along the bottom row, then
  continuing (not restarting) up each row above -- see the file header
  for the full walk-through. Scale-mode architecture in place
  (`tiles_note_map_set_scale()`) with only chromatic implemented; adding
  a scale later means adding an interval table here, not touching
  `pad_config.c` or this layout logic. Verified by
  `firmware/test/test_note_map.c` against the exact examples given when
  the layout was specified.
- `expression.h`/`.c` — done for V1: touch+Hall fusion. Touch remains
  the authoritative note on/off timing gate (more reliable to detect
  than inferring press/release from Hall depth alone); Hall supplies
  velocity (from peak acceleration observed during a short
  strike-detection window right after touch-down) and ongoing
  aftertouch (from press depth while held), sent as MIDI poly key
  pressure. Per-pad state machine: IDLE -> AWAITING_STRIKE (touched, not
  yet committed) -> NOTE_ON, with AWAITING_STRIKE cancelling back to
  IDLE (no note sent) if released before enough samples arrive.
  **Explicitly unmeasured, needs real-hardware tuning:** the
  acceleration->velocity scale, the depth->aftertouch full-scale range,
  and the strike-detection window durations -- all flagged as
  placeholders in `expression.c`, none derived from a calibrated
  mT/LSB relationship since that doesn't exist yet.
- `pedal.h`/`.c` — done: sustain (MIDI CC64) on by default, debounced
  with hysteresis, polarity defaults to the usual normally-open
  footswitch convention and is switchable at runtime
  (`tiles_pedal_set_polarity()`). Expression (CC11, continuous) is
  built but **disabled by default** -- `tiles_pedal_set_expression_enabled()`
  is the runtime toggle, meant as the hook the companion app will
  eventually control once `usb_vendor/` exists. Real auto-sensing of
  polarity/disconnected-pedal state is still a later layer, see
  `docs/architecture/defaults-and-safeguards.md` "Pedal polarity".
- Everything else (haptics, power governance, per-pad Hall calibration)
  is not built yet.
