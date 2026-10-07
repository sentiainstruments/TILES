#include "sk6805.h"

#include "hardware/clocks.h"
#include "pico/time.h"

#include "sk6805.pio.h"

/* Layering exception: a driver calling services/, for the crash recorder's
 * per-pixel trace ('p'/'q'). The trace must live inside the write loop;
 * splitting the write per pixel in the caller would break latch timing. */
#include "../services/debug_mode.h"

/* Bound on each TX-FIFO wait. pio_sm_put_blocking() spins with no timeout
 * and runs ~28 times per lighting pass, so a stalled state machine would
 * hang the whole main loop. One pixel shifts out in ~30 us and the FIFO is
 * 8 deep, so 5 ms is ~150x headroom. */
#define TILES_PIO_TIMEOUT_US 5000u

/* pio_sm_put_blocking() with a timeout: false (word not written) once
 * `timeout_us` passes without TX FIFO room. */
static bool sk6805_put_blocking_with_timeout(PIO pio, uint sm, uint32_t data, uint32_t timeout_us) {
    absolute_time_t deadline = make_timeout_time_us(timeout_us);
    while (pio_sm_is_tx_fifo_full(pio, sm)) {
        if (time_reached(deadline)) {
            return false;
        }
        tight_loop_contents();
    }
    pio_sm_put(pio, sm, data);
    return true;
}

bool tiles_sk6805_init(tiles_sk6805_chain_t *chain, PIO pio, uint gpio) {
    if (!pio_can_add_program(pio, &sk6805_program)) {
        *chain = (tiles_sk6805_chain_t){0};
        return false;
    }
    uint offset = pio_add_program(pio, &sk6805_program);

    int sm = pio_claim_unused_sm(pio, false);
    if (sm < 0) {
        pio_remove_program(pio, &sk6805_program, offset);
        *chain = (tiles_sk6805_chain_t){0};
        return false;
    }

    chain->pio = pio;
    chain->sm = (uint)sm;
    chain->gpio = gpio;
    chain->program_offset = offset;

    pio_gpio_init(pio, gpio);
    pio_sm_set_consecutive_pindirs(pio, chain->sm, gpio, 1, true);

    pio_sm_config c = sk6805_program_get_default_config(offset);
    sm_config_set_sideset_pins(&c, gpio);
    sm_config_set_out_shift(&c, false, true, 24);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);

    const float bit_hz = 800000.0f;
    const int cycles_per_bit = sk6805_T1 + sk6805_T2 + sk6805_T3;
    float div = (float)clock_get_hz(clk_sys) / (bit_hz * (float)cycles_per_bit);
    sm_config_set_clkdiv(&c, div);

    pio_sm_init(pio, chain->sm, offset, &c);
    pio_sm_set_enabled(pio, chain->sm, true);

    return true;
}

void tiles_sk6805_deinit(tiles_sk6805_chain_t *chain) {
    pio_sm_set_enabled(chain->pio, chain->sm, false);
    pio_sm_unclaim(chain->pio, chain->sm);
    pio_remove_program(chain->pio, &sk6805_program, chain->program_offset);
    *chain = (tiles_sk6805_chain_t){0};
}

uint32_t tiles_sk6805_pack_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)g << 16) | ((uint32_t)r << 8) | (uint32_t)b;
}

void tiles_sk6805_write(const tiles_sk6805_chain_t *chain, const uint32_t *pixels, size_t count) {
    for (size_t i = 0; i < count; i++) {
        /* One 'p' per pixel attempted, before the call that could hang, so a crash
         * report shows which pixel it stopped on. */
        tiles_debug_trace('p');
        /* Left-align the 24-bit GRB word (pull_threshold=24, shift-left OSR). */
        if (!sk6805_put_blocking_with_timeout(chain->pio, chain->sm, pixels[i] << 8u, TILES_PIO_TIMEOUT_US)) {
            /* State machine not draining: drop the rest of this frame but still do the
             * latch wait below. The next lighting pass rewrites current data. */
            break;
        }
    }
    /* 'q' = pixel loop finished. busy_wait_us, not sleep_us: above 6 us,
     * sleep_us() blocks on an alarm IRQ, and a missed wake-up there was a real
     * main-loop freeze. A plain timer poll cannot be lost, and it is the right
     * primitive for a short, timing-critical latch delay anyway. */
    tiles_debug_trace('q');
    busy_wait_us(TILES_SK6805_RESET_LOW_US);
}
