# Legacy prototype v1 (reference only — not reused)

This is the firmware for the *first* SENTIA TILES prototype: a 16-pad
(4×4) board built on an Arduino/Teensy-class board (`usbMIDI`,
`analogRead`/`analogReadAveraging`, `Adafruit_MPR121`,
`Adafruit_PWMServoDriver`), with an analog Hall matrix read through a 4-bit
mux and a 16-bit shift-register LED bar.

None of this code carries forward to the Pico 2 firmware in `firmware/`.
The hardware is entirely different: 24 pads instead of 16, per-key
addressable RGB instead of a shift-register LED bar, TMAG5273 3-axis Hall
sensors behind I2C muxes instead of a single analog mux, per-key haptic
motors via PCA9685 instead of the same PWM chip double-booked for pads, and
TinyUSB on RP2350 instead of the Teensy USB MIDI stack.

It's kept here purely so the *behavioral* ideas it explored aren't lost
when redesigning the real thing:

- Scale-mode selection via a long-press "select" gesture, with a
  double-click haptic confirm.
- Octave shift with pulsing LED feedback for ±2 and beyond.
- Voice allocation with oldest-voice stealing and proper note-off/reset on
  steal.
- Standby/idle animations, with wake-on-touch-and-pressure (not touch
  alone, to avoid waking on a light brush).
- A serial debug heatmap of touch/note/pressure state per pad.

Treat these as prior-art notes when we design the Pico 2 firmware's
`services/` layer (scale/note mapping, voice allocation, haptics,
standby), not as code to port.
