# Audio output (TILES v1)

**On TILES v1 (First Edition), not on Rev A0** (`tiles-v1.md`). Decided
2026-11-06: TILES plays its own sound on headphones or into a mixer,
without a computer. On Rev A0 the same sound goes to the computer as a
USB audio input (`firmware/src/audio/README.md`). What the sound is:
`../architecture/audio-engine.md`.

Rev A0 has no audio path: the DAC80502 is a CV DAC (0-10 V through a
x4 stage and 1 kOhm, unipolar, 12 V only), not suitable for headphones.

Considered and dropped: USB-C headphone dongles. The USB-C port is
device-only and can't power or host one; a second (host) port would
need a connector, a 5 V source switch and PIO-USB on core 1, and many
dongles are passive with no DAC. The built-in jack is simpler, cheaper
and works with any headphones.

## Parts

| Part | Job | Why |
|---|---|---|
| **TI PCM5102A** | Stereo I2S DAC | 112 dB dynamic range, ground-centred 2.1 Vrms output (no coupling capacitors, no DC), internal PLL so it needs no master clock (3-wire I2S), single 3.3 V supply, configured by pins (no I2C). Widely used with the Pico. |
| **TI TPA6132A2** | Stereo headphone amplifier | Ground-centred (DirectPath) output, built-in pop suppression, 25 mW into 16 Ohm, gain set by two pins, an enable pin. Drives headphones and line inputs from one jack. |
| 3.5 mm TRS jack | PHONES / LINE (decided: one jack) | On the back panel with the others. A switched jack lets GP19 sense headphones. |
| Low-noise 3.3 V LDO | Analog supply for both | Keeps the LED and motor PWM off the audio rail. |

Alternative if a single chip is preferred: TI TLV320DAC3100 (DAC,
headphone and speaker amplifiers in one), at the cost of I2C setup and a
1.8 V supply. A higher-end option is Cirrus CS43131 (DAC with headphone
amplifier), at higher cost and with a master clock.

## Pins

| Signal | GPIO | Note |
|---|---|---|
| BCK (bit clock) | GP27 | PIO side-set base |
| LRCK (word clock) | GP28 | PIO side-set base + 1: must be the next GPIO after BCK |
| DIN (data) | GP9 | |
| SCK (PCM5102A) | GND | internal PLL from BCK |
| Mute: PCM5102A XSMT + TPA6132A2 EN | GP18 | freed by the button move; pull-down, so the output is muted through boot and reset |
| Headphone detect (optional) | GP19 | the jack's switch contact |

PCM5102A pins: FMT low (I2S), FLT low (normal latency filter), DEMP low.

## Board notes

- **Noise is the risk, not the DAC.** The haptic motors run on 1 kHz PWM
  and the LEDs draw pulsed current. Give the audio section its own LDO
  from the 5 V rail, a ground return that doesn't share the motor or LED
  current path, and keep I2S and the jack traces away from the motor
  drivers.
- **Match the levels.** The DAC's 2.1 Vrms full scale is more than the
  amplifier can swing on 3.3 V: put a divider (with TI's recommended RC
  filter) between them so full scale can't clip, and pick the gain pins
  for a safe headphone maximum. Values from the two datasheets.
- Works on USB power (the DAC and amp draw tens of mA), unlike CV and
  gate, which need 12 V.
- The jack's sleeve to audio ground at the amplifier; follow TI's
  layout guides for both parts.

## Firmware

- PIO I2S (BCK/LRCK on side-set, DATA out) fed by DMA, from the same
  engine on core 1 that feeds USB audio.
- Unmute (GP18 high) only once core 1 is rendering, after a short fade
  in; mute before any reboot or BOOTSEL.
- Clocking, volume and the USB clock change: `../architecture/audio-engine.md`.
