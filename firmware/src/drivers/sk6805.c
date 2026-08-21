#include "sk6805.h"

#include "hardware/clocks.h"
#include "pico/time.h"

#include "sk6805.pio.h"

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
        /* Left-align the 24-bit GRB word in the 32-bit FIFO slot to
         * match the pull_threshold=24, shift-left OSR config above. */
        pio_sm_put_blocking(chain->pio, chain->sm, pixels[i] << 8u);
    }
    sleep_us(TILES_SK6805_RESET_LOW_US);
}
