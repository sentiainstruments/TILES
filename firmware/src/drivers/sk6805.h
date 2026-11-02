#pragma once

/* SK6805-EC15 RGB driver (PIO), timing only: the underglow is a 4-pixel
 * chain on GP8; pads are one pixel at a time on GP3 through the caller's
 * mux. Pixels latch, so pads only need writing when they change. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hardware/pio.h"

/* Data held low this long latches the pixel. The SK6805-EC15's minimum is
 * 80 us (docs/hardware board map led_systems); 120 leaves margin. (300 was
 * the WS2812B-V5's 280 us rule, which this part doesn't need.) */
#define TILES_SK6805_RESET_LOW_US 120u

typedef struct {
    PIO pio;
    uint sm;
    uint gpio;
    uint program_offset;
} tiles_sk6805_chain_t;

/* Claims a free state machine on `pio`, loads its own copy of the
 * 5-instruction program and configures `gpio`. Returns false (chain left
 * zeroed) if there is no room for the program or no free state machine. */
bool tiles_sk6805_init(tiles_sk6805_chain_t *chain, PIO pio, uint gpio);

/* Releases the state machine claimed by tiles_sk6805_init(). */
void tiles_sk6805_deinit(tiles_sk6805_chain_t *chain);

/* Packs 8-bit R/G/B into the 0x00GGRRBB word tiles_sk6805_write() expects
 * (the wire order is GRB). */
uint32_t tiles_sk6805_pack_rgb(uint8_t r, uint8_t g, uint8_t b);

/* Clocks `count` pixels down the chain, then waits out the reset/latch time,
 * so the caller can switch the mux or write another chain right away. */
void tiles_sk6805_write(const tiles_sk6805_chain_t *chain, const uint32_t *pixels, size_t count);
