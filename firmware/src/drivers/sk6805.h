#pragma once

/* SK6805-EC15 addressable RGB driver (PIO). Timing only; the caller decides
 * which pixel a write reaches:
 *   - underglow: a fixed 4-pixel chain on GP8.
 *   - pad LEDs: one pixel at a time on GP3, with the TCA9554/CD74HCT4051
 *     mux set by the caller around each write (board map
 *     led_systems.pad_leds.rules).
 * Pixels latch their color, so muxed pads don't dim and only need writing
 * when their color changes. */

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
