#pragma once

/*
 * SK6805-EC15 one-wire addressable RGB driver, PIO-based bit-banging.
 * Timing/protocol only -- knows nothing about which physical pixel a
 * write reaches or why. That's the caller's job:
 *   - underglow (services/lighting): a fixed 4-pixel daisy chain on GP8.
 *   - pad LEDs (services/lighting): ONE pixel at a time on GP3, with the
 *     TCA9554/CD74HCT4051 mux sequencing done by the caller before and
 *     after each single-pixel write, per the board map's
 *     led_systems.pad_leds.rules.
 *
 * SK6805 pixels latch: once written, a pixel holds its color with no
 * further refresh needed. Muxing between 24 pads to write one at a time
 * does not dim them or require a fast repeat rate -- it only needs to
 * happen once per actual color change.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hardware/pio.h"

#define TILES_SK6805_RESET_LOW_US 300u

typedef struct {
    PIO pio;
    uint sm;
    uint gpio;
    uint program_offset;
} tiles_sk6805_chain_t;

/* Claims an unused state machine on `pio`, loads its own copy of the
 * sk6805 PIO program (5 instructions -- cheap enough that two chains on
 * the same `pio` block, e.g. underglow + pad LEDs both on pio0, each
 * loading their own copy is not worth the complexity of sharing one
 * copy), and configures `gpio` for SK6805 output. Returns false (chain
 * left zeroed) if the program can't be loaded or no state machine is
 * free. */
bool tiles_sk6805_init(tiles_sk6805_chain_t *chain, PIO pio, uint gpio);

/* Releases the state machine claimed by tiles_sk6805_init(). */
void tiles_sk6805_deinit(tiles_sk6805_chain_t *chain);

/* Packs 8-bit R/G/B into the 0x00GGRRBB word tiles_sk6805_write()
 * expects (SK6805 wire order is GRB, not RGB). */
uint32_t tiles_sk6805_pack_rgb(uint8_t r, uint8_t g, uint8_t b);

/* Clocks `count` pixels out back-to-back down the chain, then
 * busy-waits for the reset/latch interval (TILES_SK6805_RESET_LOW_US)
 * before returning, so the caller can safely change mux state or start
 * another chain's write immediately after this returns. */
void tiles_sk6805_write(const tiles_sk6805_chain_t *chain, const uint32_t *pixels, size_t count);
