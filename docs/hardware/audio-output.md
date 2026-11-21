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

**Decided 2026-11-17 (the plan for the First Edition):** PCM5102A +
TPA6132A2, configured by pins, the audio clock from the RP2350 at a
system clock that divides exactly (230.4 MHz, 153.6 MHz the fallback).
The goals, in order: no noise from the rest of the board, high-quality
stereo on one jack, and a way of getting the sound out that can't fail
(USB audio stays: digital, immune to the board's noise; the jack is for
headphones and mixers).

## Parts

| Part | Job | Why |
|---|---|---|
| **TI PCM5102A** | Stereo I2S DAC | 112 dB dynamic range, ground-centred 2.1 Vrms output (no coupling capacitors, no DC), internal PLL so it needs no master clock (3-wire I2S), single 3.3 V supply, configured by pins (no I2C). Widely used with the Pico. |
| **TI TPA6132A2** | Stereo headphone amplifier | Ground-centred (DirectPath) output, built-in pop suppression, 25 mW into 16 Ohm, gain set by two pins, an enable pin. Drives headphones and line inputs from one jack. |
| 3.5 mm TRS jack | PHONES / LINE (decided: one jack) | In the right side, where the pedal jack was, beside the volume knob (2026-11-17, `mockups/`). A switched jack lets GP19 sense headphones. |
| Low-noise 3.3 V LDO | Analog supply for both | Keeps the LED and motor PWM off the audio rail. |

About 23 placements: the three chips, the jack and about 19 small
capacitors and resistors from the two datasheets' typical circuits (the
DAC's supplies, charge pump and output RC filter; the amplifier's charge
pump and inputs; the regulator's two). The amplifier's -6 dB gain setting
replaces a divider between the two: full scale stays under the 3.3 V
amplifier's swing, with digital headroom set in firmware.

### Considered and not chosen

Read against the same goals (2026-11-17; sources in the commit message):

| Option | At the headphones | Why not |
|---|---|---|
| TI TLV320AIC3206 (codec, ground-centred headphone driver, PLL) | 95 dB SNR into 16 Ohm, THD+N -70 dB typical (-55 worst) | ~5 parts fewer, but noisier and more distortion than the plan; needs a 1.8 V rail anyway and I2C setup at every boot, on a bus the Hall sensors or haptics keep busy |
| NXP SGTL5000, Cirrus WM8960 | 95-98 dB, 90 dB | their capacitor-free headphone modes float the sleeve at mid-supply: a mixer's grounded line input on the same jack shorts it |
| TI PCM5122 (the PCM5102A with I2C, can make its own clock) | same DAC | an oscillator and a bus device more, for a clock the plan doesn't need |
| TI TLV320DAC3100; Cirrus CS43131; AKM AK4376A | 95 dB; high; high | headphone outputs not ground-centred, or cost and a master clock, or 1.8 V and a 0.4 mm CSP |

I2C control doesn't reduce noise; the supply, the grounding, the layout
and the clock do (below).

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

- **Noise is the risk, not the DAC.** In order of what matters:
  1. Its own low-noise LDO from the 5 V rail, never the Pico's switching
     3.3 V.
  2. Ground: the audio return goes to the jack and shares no path with
     the motor or LED currents.
  3. Layout: DAC, amplifier and jack away from the motor drivers (1 kHz
     PWM) and the LED chains; short analog traces; I2S clear of them too.
  4. Ground-centred outputs (both parts): no coupling capacitors, no
     thump.
  5. A clean audio clock: an exact divider (below).
  6. Muted until the clock is stable; ESD and RF filtering at the jack.
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
- **The clock:** the RP2350 makes BCK, so the system clock must divide
  into it exactly (a fractional PIO divider wobbles BCK by a clock cycle,
  ~4 ns: measured by others on the RP2040, its effect on the PCM5102A's
  output never published). Of every clock the PLL can make from 150 to
  300 MHz, only two divide evenly at 48 kHz: **153.6 MHz** (BCK = 64 fs,
  sys / 50) and **230.4 MHz** (32 fs: sys / 150; 48 fs: sys / 100; 64 fs:
  sys / 75, an odd count, so BCK isn't 50 % duty). 200 and 250 MHz have no
  exact divider. Target 230.4 MHz once qualified on Rev A0
  (`../architecture/synth-engines.md`, "Next"); to confirm in the
  datasheet: the PCM5102A's 48 fs BCK.
- Unmute (GP18 high) only once core 1 is rendering, after a short fade
  in; mute before any reboot or BOOTSEL.
- Clocking, volume and the USB clock change: `../architecture/audio-engine.md`.
