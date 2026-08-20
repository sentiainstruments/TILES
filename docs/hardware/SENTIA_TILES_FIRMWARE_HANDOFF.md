# SENTIA TILES Core — firmware/hardware handoff

**Canonical hardware:** TILES PRE-PRODUCTION Rev A0 / production export 2026-08-01  
**Purpose:** starting point for Pico 2 firmware, bring-up, demo profiles, calibration, and manufacturing tests.  
**Machine-readable companion:** `sentia_tiles_board_map_v1.json`

> Treat the pad table and GPIO table below as the hardware truth for this PCB revision. Do not copy mappings from the older design chat. If hardware changes, change the JSON first and regenerate/replace this handoff.

## Non-negotiable rules

- GP20 is electrically connected to the PCA9685 A5 address strap. Configure it as input/high-impedance and never drive it.
- Pico VBUS pin 40 is not connected. USB data still uses TP2/TP3; use TinyUSB mounted state for a real USB connection, not GPIO24.
- Only one TMAG5273 channel across all three TCA9548A muxes may be enabled at once; every sensor has address 0x35.
- Disable all three LED mux banks before changing their select bits, and enable exactly one bank for each one-pixel update.
- PCA9685 OE is tied low. Motors must fail off through all-zero PWM plus their hardware 100k gate pulldowns.
- CV and gate require external 12V power. They must remain off in USB-only mode.
- The USB eFuse limit is not the USB operating budget. Unknown USB sources start in the 500mA-safe profile.
- No module may own raw pins directly. A board/HAL layer owns pins and the single 24-entry pad table owns every physical mapping.

## Pico 2 GPIO map

| GPIO | Pico pin | Net | Function | Required boot state |
|---:|---:|---|---|---|
| GP0 | 1 | `NET_57` | DIN MIDI OUT line A | drive high before enabling MIDI OUT |
| GP1 | 2 | `MIDI IN RX` | DIN MIDI IN | input |
| GP2 | 4 | `NET_58` | DIN MIDI OUT line B | drive high before enabling MIDI OUT |
| GP3 | 5 | `NET_50` | pad LED serialized data | low |
| GP4 | 6 | `I2C0_SDA` | I2C0 Hall muxes + touch | peripheral/input |
| GP5 | 7 | `I2C0_SCL` | I2C0 Hall muxes + touch | peripheral/input |
| GP6 | 9 | `I2C1_SDA` | I2C1 haptics + LED mux control | peripheral/input |
| GP7 | 10 | `I2C1_SCL` | I2C1 haptics + LED mux control | peripheral/input |
| GP8 | 11 | `UNDERGLOW` | four-LED underglow chain | low |
| GP9 | 12 | `NET_59` | unused | input/no-pull |
| GP10 | 14 | `SCLK` | DAC80502 SPI clock | peripheral/input |
| GP11 | 15 | `SDIN` | DAC80502 SPI MOSI | peripheral/input |
| GP12 | 16 | `GATE_PWM` | gate output driver | low |
| GP13 | 17 | `SYNC` | DAC80502 active-low chip select | high |
| GP14 | 19 | `NET_56` | SW6 circle | input; active low |
| GP15 | 20 | `NET_55` | SW5 square | input; active low |
| GP16 | 21 | `NET_53` | SW3 triangle | input; active low |
| GP17 | 22 | `NET_54` | SW4 diamond | input; active low |
| GP18 | 24 | `NET_52` | SW2 right capsule | input; active low |
| GP19 | 25 | `NET_51` | SW1 left capsule | input; active low |
| GP20 | 26 | `NET_25` | PCA9685 A5 address strap | input/high-Z; **NEVER DRIVE** |
| GP21 | 27 | `TOUCH_IRQ` | shared MPR121 active-low IRQ | input; active low |
| GP22 | 29 | `GP22` | TPS2121 ST power-source status | input; low means external IN2 selected |
| GP26 | 31 | `PEDAL_ADC` | pedal analog input ADC0 | input |
| GP27 | 32 | `NET_61` | unused | input/no-pull |
| GP28 | 34 | `NET_62` | unused | input/no-pull |

Other module pins: VSYS pin 39 is the selected +5V rail; VBUS pin 40 is NC; 3V3_OUT pin 36 powers board logic; ADC_VREF pin 35 is externally NC; TP2 is USB D− and TP3 is USB D+; pins 41/43 are SWDIO/SWCLK debug pads.

## Power/connection states

| External selected (GP22/ST) | TinyUSB mounted | Actual mode | Firmware policy |
|---|---|---|---|
| High | Yes | USB only | Use USB-safe or explicit validated-USB profile; CV/gate off |
| Low | No | External 12V only | Full local/DIN/CV operation; no USB MIDI |
| Low | Yes | USB data + external 12V power | Full external-power profile and USB MIDI |
| High | No | Invalid/transient while code is running | Fail outputs off and report a power fault |

TPS2121 ST is high for IN1/USB or Hi-Z and low for IN2/external. The board cannot measure the Type-C source-current advertisement, so any USB budget above 500mA is a manual profile chosen only after validating the exact computer, hub, cable and startup behavior.

Critical power values are fixed in hardware: TPS259470 OVLO 402k/100k (~6.02V), ILM 1.13k (~2.95A), DVDT 3.3nF; external buck 54.9k/10k (~4.984V); motor buck 33k/10k (~3.302V); TPS2121 PR1 USB divider 10.2k/5k (~1.645V), CP2 external divider 10k/10k (2.5V), and ILM 29.8k (~3.5A typical). Because CP2 is higher than PR1 with both rails present, external IN2 wins.

## Buses and addresses

### I2C0 — GP4 SDA / GP5 SCL, 400kHz

- Hall muxes: U3 `0x70`, U4 `0x71`, U5 `0x72`.
- All TMAG5273A1 sensors behind the muxes: `0x35`.
- Touch: U_TOUCH1 `0x5A`, U_TOUCH2 `0x5B`; shared active-low IRQ on GP21.
- TCA_RESET is pulled high in hardware; there is no firmware reset GPIO.

A full 24-pad XYZ Hall scan at 400kHz has a practical ceiling around 120–150 frames/s after mux and protocol overhead. Start at a 120Hz target; do not promise a 500Hz frame rate without measured optimization.

### I2C1 — GP6 SDA / GP7 SCL, 400kHz

- U_HAPTIC1 PCA9685 `0x60`; U_HAPTIC2 `0x61`.
- U12 TCA9554 LED-mux controller `0x20`.
- Set PCA9685 MODE2.OUTDRV=1/totem-pole. Motor outputs are active-high; function LEDs are active-low.

## Major components

| Group | Qty | Part | Firmware/electrical role |
|---|---:|---|---|
| controller | 1 | `Raspberry Pi Pico 2` | main MCU / USB device |
| sensing | 24 | `TMAG5273A1QDBVR` | 3-axis Hall sensors |
| sensing | 3 | `TCA9548APWR` | Hall I2C channel muxes |
| sensing | 2 | `MPR121QR2` | 24 capacitive electrodes |
| outputs | 2 | `PCA9685PW,118` | 24 motor PWM + 6 function-button LED PWM channels |
| outputs | 24 | `AO3400A` | motor low-side switching |
| outputs | 1 | `TCA9554PWR` | pad-LED mux selector/enable control |
| outputs | 3 | `CD74HCT4051M96` | route serialized data to one of 24 pad LEDs |
| outputs | 2 | `74AHCT1G126GV` | 5V level buffers for pad LEDs and underglow |
| outputs | 28 | `SK6805-EC15` | 24 daughterboard pixels + 4 motherboard underglow pixels |
| power | 1 | `TPS259470ARPWR` | USB eFuse/OV protection |
| power | 1 | `TPS2121RUXR` | USB/external 5V power mux |
| power | 2 | `TPS563201DDCR` | 12V-to-5V and 5V-to-motor-rail bucks |
| interfaces | 1 | `DAC80502DRXR` | dual 16-bit CV DAC |
| interfaces | 1 | `OPA2990IDR` | dual CV output amplifier |
| interfaces | 1 | `H11L1M` | DIN MIDI IN isolation |
| interfaces | 1 | `74AHCT2G125DC,125` | polarity-selectable DIN MIDI OUT buffer |
| interfaces | 1 | `TPD2EUSB30DRTR` | USB D+/D- ESD protection |
| interconnect | 48 | `FH34SRJ-6S-0.5SH(50)` | 24 motherboard + 24 daughterboard flex connectors |

## Canonical 24-pad map

Pads are row-major: 1–6 top row, then 7–12, 13–18, and 19–24 bottom row.

| Pad | Grid | Center mm (X,Y) | Touch | Hall mux/ch | LED mux/ch | Haptic PCA/ch | FPC |
|---:|---|---|---|---|---|---|---:|
| 1 | R1C1 | -65.000, 38.886 | 0x5A/ELE11 | 0x70/CH4 | MUX1/CH4 | 0x60/CH3 | 1 |
| 2 | R1C2 | -39.000, 38.886 | 0x5A/ELE10 | 0x70/CH3 | MUX1/CH2 | 0x60/CH4 | 2 |
| 3 | R1C3 | -13.000, 38.886 | 0x5A/ELE8 | 0x71/CH4 | MUX2/CH4 | 0x60/CH5 | 3 |
| 4 | R1C4 | 13.000, 38.886 | 0x5B/ELE3 | 0x71/CH3 | MUX2/CH2 | 0x61/CH0 | 4 |
| 5 | R1C5 | 39.000, 38.886 | 0x5B/ELE2 | 0x72/CH4 | MUX3/CH4 | 0x61/CH1 | 5 |
| 6 | R1C6 | 65.000, 38.886 | 0x5B/ELE1 | 0x72/CH3 | MUX3/CH2 | 0x61/CH6 | 6 |
| 7 | R2C1 | -65.000, 12.886 | 0x5A/ELE9 | 0x70/CH5 | MUX1/CH6 | 0x60/CH2 | 7 |
| 8 | R2C2 | -39.000, 12.886 | 0x5A/ELE6 | 0x70/CH2 | MUX1/CH1 | 0x60/CH15 | 8 |
| 9 | R2C3 | -13.000, 12.886 | 0x5A/ELE7 | 0x71/CH5 | MUX2/CH6 | 0x60/CH7 | 9 |
| 10 | R2C4 | 13.000, 12.886 | 0x5B/ELE5 | 0x71/CH2 | MUX2/CH1 | 0x61/CH15 | 10 |
| 11 | R2C5 | 39.000, 12.886 | 0x5B/ELE4 | 0x72/CH5 | MUX3/CH6 | 0x61/CH14 | 11 |
| 12 | R2C6 | 65.000, 12.886 | 0x5B/ELE0 | 0x72/CH2 | MUX3/CH1 | 0x61/CH7 | 12 |
| 13 | R3C1 | -65.000, -13.114 | 0x5A/ELE3 | 0x70/CH6 | MUX1/CH7 | 0x60/CH14 | 13 |
| 14 | R3C2 | -39.000, -13.114 | 0x5A/ELE4 | 0x70/CH1 | MUX1/CH0 | 0x60/CH13 | 14 |
| 15 | R3C3 | -13.000, -13.114 | 0x5A/ELE5 | 0x71/CH6 | MUX2/CH7 | 0x60/CH8 | 15 |
| 16 | R3C4 | 13.000, -13.114 | 0x5B/ELE6 | 0x71/CH1 | MUX2/CH0 | 0x61/CH13 | 16 |
| 17 | R3C5 | 39.000, -13.114 | 0x5B/ELE8 | 0x72/CH6 | MUX3/CH7 | 0x61/CH11 | 17 |
| 18 | R3C6 | 65.000, -13.114 | 0x5B/ELE10 | 0x72/CH1 | MUX3/CH0 | 0x61/CH9 | 18 |
| 19 | R4C1 | -65.000, -39.114 | 0x5A/ELE0 | 0x70/CH7 | MUX1/CH5 | 0x60/CH12 | 19 |
| 20 | R4C2 | -39.000, -39.114 | 0x5A/ELE1 | 0x70/CH0 | MUX1/CH3 | 0x60/CH11 | 20 |
| 21 | R4C3 | -13.000, -39.114 | 0x5A/ELE2 | 0x71/CH7 | MUX2/CH5 | 0x60/CH9 | 21 |
| 22 | R4C4 | 13.000, -39.114 | 0x5B/ELE7 | 0x71/CH0 | MUX2/CH3 | 0x61/CH12 | 22 |
| 23 | R4C5 | 39.000, -39.114 | 0x5B/ELE9 | 0x72/CH7 | MUX3/CH5 | 0x61/CH10 | 23 |
| 24 | R4C6 | 65.000, -39.114 | 0x5B/ELE11 | 0x72/CH0 | MUX3/CH3 | 0x61/CH8 | 24 |

Each FPC uses: pin 1 TOUCH, pin 2 GND, pin 3 pad LED data, pin 4 +5V, pin 5 MOTOR_V+ (~3.30V), pin 6 switched MOTOR−, shells 7/8 GND. The daughterboard adds 1k in the touch path, 150Ω in LED DIN, a 10k DIN pulldown, and one 100nF +5V bypass capacitor.

## Button order

| Button | Physical shape/order | Input | LED PWM | Polarity |
|---:|---|---|---|---|
| SW1 | left capsule | GP19 | 0x60/CH0 | input low; LED low = on |
| SW2 | right capsule | GP18 | 0x60/CH1 | input low; LED low = on |
| SW3 | triangle | GP16 | 0x61/CH2 | input low; LED low = on |
| SW4 | diamond | GP17 | 0x61/CH3 | input low; LED low = on |
| SW5 | square | GP15 | 0x61/CH4 | input low; LED low = on |
| SW6 | circle | GP14 | 0x61/CH5 | input low; LED low = on |

The symbols do not hard-code behavior. A reasonable demo assignment is previous, next, haptics toggle, pad-LED toggle, underglow toggle, and panic/all-notes-off, but keep this in a profile table so it can be changed without touching drivers.

## LEDs

Pad LED path: GP3 → 5V AHCT buffer → 330Ω → three CD74HCT4051 common inputs. U12 P0/P1/P2 are S0/S1/S2; P3/P4/P5 are active-low enables for muxes 1/2/3. Each selected path ends at one SK6805-EC15; DOUT is NC.

Underglow: GP8 → AHCT buffer → 330Ω → four cascaded SK6805-EC15 pixels.

Use GRB order, nominal 800kb/s, and a conservative 300µs low latch/reset. The LED is the 5mA-per-color version: budget about 16mA for one full-white pixel including ~1mA IC current. Twenty-four pad pixels plus four underglow pixels can therefore approach ~448mA at full white before the six function-button LEDs and the rest of the board.

## Haptics

The two PCA9685 devices drive 24 AO3400A low-side stages. Use 1kHz as the initial PWM frequency, zero every motor channel at initialization, and stagger starts by at least 15ms. USB and external voice counts are allocation ceilings—not current guarantees. A current governor must estimate active duty and reject or scale commands that exceed the selected profile.

Before enabling the 12-voice external profile, measure one motor's running and stall/start current through the real flex and daughterboard. The FH34 contact is limited to 0.35A continuous by the chosen derating, and the flex adds approximately 0.665Ω copper loop resistance before connector resistance.

## MIDI, pedal and CV

- **USB MIDI/MPE:** TinyUSB through TP2/TP3. Initialize USB even when externally powered; `tud_mounted()`/suspend state supplies the useful connection status.
- **DIN MIDI IN:** GP1, 31,250 baud, optoisolated and input-polarity tolerant.
- **DIN MIDI OUT:** GP0 and GP2 feed the two buffer channels. Implement a PIO/software UART on the chosen signal pin while holding the other pin high; swap roles for the alternate TRS polarity. Start with both pins high.
- **Pedal:** GP26/ADC0 through 1k + 100nF with 100k pull-up. Calibrate min, max, polarity and disconnected threshold.
- **CV:** DAC80502 on GP10 SCLK, GP11 MOSI, GP13 /CS. Both 0–2.5V channels are amplified 4× to nominal 0–10V.
- **Gate:** GP12, active high, through the two-MOSFET driver. CV/gate are unavailable without external 12V.
- **MPE allocation:** the lower zone has only 15 member channels (2–16), so 24 physical pads require dynamic channel allocation. On a 16th simultaneous note, use a deterministic policy such as stealing the oldest released/quietest voice and always send note-off/reset messages before reusing its channel.

## Mechanical and magnetic constants

- Main PCB: 171.577 × 143.828mm, 4 layers; product plan: 183.0 × 149.8mm.
- Grid: 6 × 4 at 26.0mm pitch. Daughterboard: 17.6 × 17.6 × 1.6mm. Keycap: 20 × 20mm.
- Switch: Gateron KS-20 Dual-rail Magnetic Orange, exact part KS-20UO10B045NW-X14; N pole faces the PCB; nominal initial force 30±10gf; total travel 4.1±0.2mm.
- Gateron quotes 75±15G at rest and 700±80G at bottom in its own reference geometry. SENTIA's geometry is different, so these are not firmware ADC endpoints.
- Current stack estimate: 3mm PCB-to-acrylic clearance + 5mm acrylic + 1.2mm foam (about 0.2mm compressed). With the ~1.4mm TMAG package, switch-bottom to sensor-top gap is approximately 7.8mm at rest and 6.8mm with compressed foam, before accounting for magnet position inside the switch.
- Flex: 2-layer, 0.1mm body, 18µm RA copper, 0.15mm traces, ~1.4mm neck, 56.657mm developed tip-to-tip centerline. Keep its 0.35A/contact continuous limit in the current governor.

All 24 Hall parts share the same board rotation, but the assembled magnet/sensor axis signs still must be discovered. Begin with the TMAG5273A1 ±80mT range, capture raw XYZ at rest/half/bottom/four tilts for every pad, and persist per-pad offsets, signs, cross-axis compensation, dead zones and curves. Only move to ±40mT if all assembled keys remain safely below saturation.

## Firmware structure

Recommended module boundary:

```text
board/        only raw pins, I2C/SPI/PIO and the canonical map
drivers/      tmag5273, tca9548a, mpr121, pca9685, tca9554, sk6805, dac80502
services/     scan, touch, haptics, lighting, power governor, calibration
midi/         USB MIDI/MPE, DIN IN, polarity-selectable DIN OUT
profiles/     feature flags, current budgets, button bindings, note layouts
diagnostics/  command shell, per-pad test, fault/status reporting
storage/      versioned + CRC-protected dual-slot flash configuration
```

Start with a deterministic single-core cooperative loop. Move scanning to core 1 only after the single-core system is measurable and stable; hidden multicore races are worse than a slightly lower scan rate during prototype bring-up.

### Runtime feature flags

`usb_midi`, `din_midi_in`, `din_midi_out`, `hall`, `touch`, `haptics`, `pad_leds`, `underglow`, `buttons`, `pedal`, `cv`, `gate`, `diagnostics`.

Every module must be independently startable/stoppable. Disabling a module first forces its outputs to a safe state; an I2C or calibration failure disables that module but leaves USB diagnostics alive.

## Demo/bring-up profiles

| Profile | Purpose | Haptics | Power rule |
|---|---|---:|---|
| SAFE_BRINGUP | USB diagnostics, buttons and firmware updates | 0 | 500mA operating budget |
| SENSOR_TEST | Hall/touch/pedal calibration, no outputs | 0 | works on USB or external |
| USB_DEMO_SAFE | MIDI + sensors + current-capped lighting/haptics | max 5 allocated | 500mA governor; duty may reduce usable voices |
| USB_DEMO_VALIDATED_1P5A | Known demo computer/hub only | max 5 | manual override; never auto-select |
| FULL_DEMO_EXTERNAL | Complete demo including CV/gate | max 12 | GP22 must be low; 2.5A 5V operating target |
| MANUFACTURING_TEST | Mapping and assembly test | 1 at a time | never exercise high-current outputs together |

## Safe boot sequence

1. Set GP0/GP2 high; GP3/GP8/GP12 low; GP13 high; GP20 input/high-Z.
2. Start watchdog and diagnostics transport.
3. Initialize I2C buses at 100kHz for detection, then raise to 400kHz after all expected devices ACK.
4. Disable every TCA9548A Hall channel.
5. Set TCA9554 P3/P4/P5 high so all LED mux banks are disabled, then set selector bits.
6. Initialize both PCA9685 devices with all channels at zero/off and MODE2.OUTDRV=1.
7. Read GP22 and TinyUSB state; select the safe profile unless a stored, CRC-valid explicit override exists.
8. Initialize sensors and outputs module-by-module; a failed module remains disabled without blocking USB diagnostics.

## Measurements still required before full demo power

- Motor running and stall current through one real daughterboard + flex.
- 3.3V rail current with every sensor and controller active.
- 5V input current for worst-case LED colors/brightness.
- USB-only brownout margin on each intended demo computer/cable.
- Hall raw XYZ ranges at rest, half press, bottom and maximum tilt for all 24 assembled keys.

These are calibration/characterization tasks, not missing PCB mappings. The firmware should expose commands to inspect I2C devices, select one pad, stream raw Hall XYZ/touch/ADC values, pulse one motor at a bounded duty, set one LED, read power state, and save/erase calibration.

## Source authority

The net mappings were reconstructed from the final 2026-08-01 motherboard and daughterboard flying-probe netlists and reconciled with their BOM/PnP exports. Manufacturer behavior/rating sources to keep with the firmware repository:

- Raspberry Pi Pico 2 datasheet: https://datasheets.raspberrypi.com/pico/pico-2-datasheet.pdf
- TI TMAG5273 datasheet: local `/Users/matiascevallos/Downloads/tmag5273.pdf`
- TI TPS2121 datasheet: local `/Users/matiascevallos/Downloads/tps2121.pdf`
- NXP MPR121: https://www.nxp.com/docs/en/data-sheet/MPR121.pdf
- NXP PCA9685: https://www.nxp.com/docs/en/data-sheet/PCA9685.pdf
- Gateron KS-20UO10B045NW-X14 specification: https://www.gateron.com/u_file/2506/10/file/GATERONDual-railMagneticOrangeSwitchSPEC-KS-20U-005KS-20UO10B045NW-X14.pdf
- Hirose FH34SRJ: https://www.hirose.com/en/product/p/CL0580-1236-1-50
- SK6805-EC15 manufacturer page/datasheet: https://www.normandled.com/Product/view/id/842.html

