#pragma once

/* What a system clock above the RP2350's 150 MHz rating is qualified on
 * (docs/architecture/synth-engines.md, "Next"): the clock, the core
 * voltage, the chip's temperature (INFO) and a read-back of the program
 * in flash (FLASH CRC). */

#include <stdint.h>

uint32_t tiles_chip_clk_khz(void);
/* The core regulator's setting in mV (0: a code past the table). */
uint32_t tiles_chip_vreg_mv(void);
/* The on-chip sensor, about +-2 degrees once settled. Borrows the ADC
 * from the pedal for a moment (same core). */
float tiles_chip_temp_c(void);
/* CRC-32 (zlib's) of the program image read through the uncached window,
 * so every word comes from the flash at its current timing; ~0.1 s. */
uint32_t tiles_chip_image_crc(uint32_t *bytes);
