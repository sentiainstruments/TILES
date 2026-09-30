#pragma once

/* DAC80502 dual 16-bit SPI DAC for the pitch and pressure CV outputs
 * (services/cv_gate.c). Signal chain: DAC 0-2.5 V -> 10k -> OPA2990 x4 ->
 * 1k -> jack, nominally 0-10 V (docs/hardware/SENTIA_TILES_FIRMWARE_HANDOFF.md).
 *
 * Written from the DAC8050x family's documented 24-bit SPI protocol; not
 * yet brought up on hardware. A wrong detail can only mean no output or a
 * wrong voltage inside the 0-2.5 V rail. Confirm with a meter (ideally a
 * logic analyzer on SCLK/MOSI/SYNC) before relying on it.
 *
 * SPI1 on GP10 (SCLK) / GP11 (MOSI), write-only (no MISO wired, so no
 * readback). Chip select (GP13, SYNC-N) is plain GPIO, set up deselected
 * by board_init.c. */

#include <stdint.h>

typedef enum {
    TILES_DAC80502_CHANNEL_A = 0u, /* VOUTA: pitch CV */
    TILES_DAC80502_CHANNEL_B = 1u, /* VOUTB: pressure CV */
} tiles_dac80502_channel_t;

/* Claims SPI1, sets both channels to the internal 2.5 V reference at 1x
 * buffer gain (the 0-2.5 V swing the x4 output stage expects) and writes
 * both to 0. services/cv_gate.c applies its trim right after. Call after
 * board_init(). */
void tiles_dac80502_init(void);

/* Writes a raw 16-bit code to one channel. Volts, trim and notes are the
 * caller's job. */
void tiles_dac80502_write(tiles_dac80502_channel_t channel, uint16_t code);
