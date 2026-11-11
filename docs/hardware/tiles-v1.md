# TILES v1 (First Edition)

Internal name for production run 1, also called the First Edition. It is
Rev A0 (`SENTIA_TILES_FIRMWARE_HANDOFF.md`, `sentia_tiles_board_map_v1.json`)
with three changes; everything not listed here stays as on Rev A0.

1. **Stereo audio out:** a DAC, a headphone amplifier and one phones/line
   jack on the back panel (`audio-output.md`).
2. **Two rotary encoders.** No function yet; they get mapped later.
3. **The six function buttons move to an I2C expander,** freeing their
   GPIOs for the encoders. Rev A0 has only three unused GPIOs (GP9, GP27,
   GP28) and the DAC needs all three.

Status: decided 2026-11-11, not yet drawn. The board map JSON stays the
Rev A0 truth until v1 has its own.

## GPIO changes

| GPIO | Rev A0 | TILES v1 |
|---:|---|---|
| GP9 | unused | I2S data (PCM5102A DIN) |
| GP14 | SW6 circle | encoder 1 A |
| GP15 | SW5 square | encoder 1 B |
| GP16 | SW3 triangle | encoder 2 A |
| GP17 | SW4 diamond | encoder 2 B |
| GP18 | SW2 right capsule | audio mute (DAC XSMT + amp EN), pull-down: muted until the firmware unmutes |
| GP19 | SW1 left capsule | spare (suggested: headphone-jack detect) |
| GP27 | unused | I2S BCK |
| GP28 | unused | I2S LRCK (must be the GPIO after BCK) |

Each encoder's A and B sit on adjacent GPIOs so a PIO state machine
counts the steps by itself: no CPU, no missed steps however fast the
knob turns.

## The button expander

| | |
|---|---|
| Part | TCA9554 (the part U12 already is: one part, one driver) |
| Bus / address | I2C1, `0x21` (A0 high; U12 is `0x20`) |
| P0-P5 | SW1-SW6 in order (SW1 left capsule ... SW6 circle) |
| P6, P7 | encoder 1 push, encoder 2 push |
| INT | not connected: read once per main-loop pass |

- All eight pins are inputs, active low, with 10 k external pull-ups
  (the TCA9554's internal ones are weak for switch lines).
- One read per pass is a 1-byte I2C transfer, about 70 us at 400 kHz;
  buttons answer as fast as Rev A0's polled GPIOs.
- The buttons now share I2C1 with haptics and the LED mux. The bounded
  I2C layer and bus recovery cover them; the button LEDs stay on the
  PCA9685s.

## Encoders

- Any detented encoder with a push switch (EC11 class). 10 k pull-ups to
  3.3 V on A and B; the PIO counter tolerates contact bounce.
- No function yet: the firmware reports their counts and pushes (INFO,
  settings shell) until they are mapped.

## Firmware

- `board_pins.h`: a v1 map beside Rev A0's, chosen at build time. Both
  keep working: four Rev A0 units exist.
- `services/buttons.c`: reads the expander on v1.
- A PIO quadrature counter per encoder (pio1 or pio2; pio0 has the
  two LED chains).
- Audio: I2S out, the system clock and the mute pin
  (`../architecture/audio-engine.md`).
