#pragma once

/*
 * DAC80502 -- dual-channel, 16-bit, SPI DAC driving services/cv_gate.c's
 * pitch and pressure CV outputs (see board_pins.h "CV / gate" and
 * docs/hardware/SENTIA_TILES_FIRMWARE_HANDOFF.md's own "CV" line for the
 * full signal chain: DAC channel -> 10k series -> OPA2990 gain-of-4 ->
 * 1k output -> jack, nominal 0-10V at the jack from a 0-2.5V DAC swing).
 *
 * Real feedback: "lets implement cv/gate functionality." This is the
 * first driver written for this chip in this codebase -- unlike
 * drivers/hall.c's TMAG5273 or drivers/mpr121.c, which both got their
 * protocol details confirmed against real hardware over many rounds
 * this session, this one is implemented from the DAC8050x family's own
 * documented 24-bit SPI register protocol as understood at write time,
 * with NO real hardware bring-up behind it yet. Worst-case failure mode
 * if a register value or timing detail is wrong is "outputs nothing, or
 * the wrong voltage within the DAC's own bounded 0-2.5V rail" -- a
 * garbled SPI frame can't exceed the chip's own supply rails -- but the
 * exact protocol still needs confirming against real hardware (a
 * multimeter on the CV jack, ideally a logic analyzer on SCLK/MOSI/SYNC)
 * before this is trusted the way this codebase's other drivers now are.
 *
 * SPI1 (GP10 SCLK, GP11 MOSI -- both fall in the RP2350's SPI1 pin
 * group), write-only: no MISO pin is wired on this board (board_pins.h
 * has none), so this driver never reads the DAC back (DEVICEID/STATUS
 * registers exist on the real chip but are unreachable from this
 * board's wiring). Chip select (GP13/SYNC-N) is plain bit-banged GPIO,
 * not the RP2350's hardware SPI CS -- board_init.c already sets it up
 * that way (deselected/high at boot) for exactly this reason.
 */

#include <stdint.h>

typedef enum {
    TILES_DAC80502_CHANNEL_A = 0u, /* VOUTA -- services/cv_gate.c's pitch CV */
    TILES_DAC80502_CHANNEL_B = 1u, /* VOUTB -- services/cv_gate.c's pressure CV */
} tiles_dac80502_channel_t;

/* Claims SPI1 on GP10/GP11 and configures the GAIN register for both
 * channels to use the internal 2.5V reference undivided at 1x output
 * buffer gain (REF-DIV=0, BUFF-GAIN=1x for A and B) -- the 0-2.5V DAC
 * swing docs/hardware/SENTIA_TILES_FIRMWARE_HANDOFF.md's own "CV" line
 * assumes going into the external OPA2990 gain-of-4 stage, rather than
 * trusting whatever the chip's power-on-reset default happens to be.
 * Also writes both channels to mid-scale... no -- to 0, matching
 * board_init.c's own "gate low, DAC deselected until explicitly driven"
 * fail-safe posture; services/cv_gate.c's own init immediately follows
 * with whatever real zero-point trim is configured. Must run after
 * board_init() (GP13 already configured as a plain GPIO output there). */
void tiles_dac80502_init(void);

/* Writes a 16-bit code (0-65535, full DAC scale) to one channel.
 * Callers are responsible for whatever volts-to-code conversion and
 * calibration trim applies -- this driver has no notion of volts,
 * calibration, or musical notes at all, only raw codes. */
void tiles_dac80502_write(tiles_dac80502_channel_t channel, uint16_t code);
