# Audio output (next board revision)

**Proposal, not on Rev A0.** Decided 2026-11-06: the first release gets
an audio DAC so TILES plays its own sound on headphones or into a mixer,
without a computer. Until then the same sound goes to the computer as a
USB audio input (`firmware/src/audio/README.md`).

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
| 3.5 mm TRS jack | PHONES / LINE out | One stereo jack on the back panel with the others. |
| Low-noise 3.3 V LDO | Analog supply for both | Keeps the LED and motor PWM off the audio rail. |

Alternative if a single chip is preferred: TI TLV320DAC3100 (DAC,
headphone and speaker amplifiers in one), at the cost of I2C setup and a
1.8 V supply. A higher-end option is Cirrus CS43131 (DAC with headphone
amplifier), at higher cost and with a master clock.

## Pins

Rev A0 leaves exactly three Pico 2 GPIOs unused: GP9, GP27, GP28.

| Signal | GPIO | Note |
|---|---|---|
| BCK (bit clock) | GP27 | PIO side-set base |
| LRCK (word clock) | GP28 | PIO side-set base + 1: must be the next GPIO after BCK |
| DIN (data) | GP9 | any free GPIO |
| SCK (PCM5102A) | GND | internal PLL from BCK |
| Mute / amp enable | a spare PCA9685 channel | 30 of the 32 channels are used; outputs stay off until OE goes low, so the output is muted through boot |

PCM5102A pins: FMT low (I2S), FLT low (normal latency filter), DEMP
low, XSMT driven with the amp enable (soft mute until the firmware is
running).

## Board notes

- **Noise is the risk, not the DAC.** The haptic motors run on 1 kHz PWM
  and the LEDs draw pulsed current. Give the audio section its own LDO
  from the 5 V rail, a ground return that doesn't share the motor or LED
  current path, and keep I2S and the jack traces away from the motor
  drivers.
- Works on USB power (the DAC and amp draw tens of mA), unlike CV and
  gate, which need 12 V.
- The jack's sleeve to audio ground at the amplifier; follow TI's
  layout guides for both parts.

## Firmware when the board exists

- PIO I2S (BCK/LRCK on side-set, DATA out) fed by DMA, from the same
  engine on core 1 that feeds USB audio.
- Clocking: 48 kHz from the system clock needs a fractional PIO divider
  at 150 MHz (bit clock jitter). Running the system clock at 153.6 MHz
  (48 kHz x 3200) gives an integer divider; everything timed from
  `clock_get_hz()` follows, but check DIN MIDI and the SK6805 timing.
- With a DAC the crystal becomes the audio clock: USB audio switches to
  true asynchronous packets (48 +- 1 frames by the ring level) instead of
  following the host, so the DAC and the recording stay in step.
- Volume in firmware (the Feel menu or a setting), applied before I2S.
