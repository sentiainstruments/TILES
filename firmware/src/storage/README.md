# storage/

Versioned, CRC-protected configuration storage in Pico flash, with two
alternating slots so a failed/interrupted write never bricks the active
profile. Persists: the active profile, per-pad calibration (rest/half/
bottom/tilt XYZ, offsets, signs, cross-axis compensation, dead zones,
curves), pedal calibration, CV zero/gain, LED current coefficients, motor
current model, USB profile validation state, DIN MIDI OUT polarity.
