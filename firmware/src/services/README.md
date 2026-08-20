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
