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
  brightness"): underglow solid white at a fixed idle baseline, all 24
  pads solid white at the same idle baseline, brightness hard-clamped to
  a hardcoded USB-only ceiling (no power-profile governor yet).
  `tiles_lighting_set_pad_press()` is the hook for touch/Hall to drive
  per-pad brightness once those exist — nothing calls it yet, so all
  pads currently sit at idle baseline permanently.
  **Not done:** standby animations (needs its own design pass — see the
  defaults doc), any power-profile awareness beyond the hardcoded
  ceiling.
- Everything else (Hall scan, touch fusion, expression mapping, haptics,
  pedal, power governance, calibration) is not built yet.
