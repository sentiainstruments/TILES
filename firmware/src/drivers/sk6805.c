#include "sk6805.h"

#include "hardware/clocks.h"
#include "pico/time.h"

#include "sk6805.pio.h"

/* Temporary, diagnostic-only dependency on services/ -- a real exception
 * to this codebase's own drivers-don't-depend-on-services layering
 * (drivers/i2c_bus.c, drivers/board_init.c etc. never do this). Real
 * hardware just proved this exact function is where the recurring
 * freeze lands (see this file's own TILES_PIO_TIMEOUT_US comment for
 * the finding), specifically INSIDE tiles_sk6805_write() despite its
 * own 5ms timeout -- the caller-side 'w'/'x' pair in services/lighting.c
 * bisects "before this call" from "after it," but not WHERE inside it.
 * Changing the caller to push one pixel at a time instead would corrupt
 * the reset/latch timing below (must run once, after the LAST pixel,
 * not after each one) -- reaching in here directly is the only way to
 * get per-pixel resolution without that side effect. */
#include "../services/debug_mode.h"

/* Real feedback: a second real-hardware freeze persisted even after
 * every i2c_write_blocking()/i2c_read_blocking() call in this codebase
 * got a timeout bound (see drivers/pca9685.c's own identical rationale)
 * -- meaning the actual cause was never I2C at all. pio_sm_put_blocking()
 * (pico-sdk's hardware/pio.h) is a raw `while (fifo_full) tight_loop_
 * contents();` spin with NO timeout whatsoever, same failure class, and
 * this file's tiles_sk6805_write() calls it in a plain loop with zero
 * protection -- called on literally every tiles_lighting_service() pass
 * (every single main-loop iteration, unconditionally), once per pixel
 * (per services/lighting.c's own header comment: pad LEDs are written
 * ONE AT A TIME, muxed, so a full pass is ~24 separate blocking pushes
 * for pads alone, plus 4 more for underglow). If this state machine ever
 * stops draining its FIFO for any reason (disabled, wedged, a clock/
 * program-counter fault), the ENTIRE main loop hangs forever right here
 * -- unlike I2C's occasional per-pad reads, this is the single highest-
 * frequency unguarded wait in the whole firmware, and was never checked
 * during the I2C-focused round specifically because it isn't I2C. Same
 * fix, same 5000us-generous-headroom philosophy: at this driver's own
 * configured 800kHz bit rate (sk6805_T1+T2+T3 cycles per bit, see
 * tiles_sk6805_init()'s own clkdiv math), one 24-bit pixel takes ~30us
 * to shift out and the TX FIFO is 8 words deep (PIO_FIFO_JOIN_TX docked
 * from RX), so a genuinely working state machine should never wait more
 * than a fraction of that -- 5ms is ~150x headroom, never expected to
 * trip under real operation. */
#define TILES_PIO_TIMEOUT_US 5000u

/* pio_sm_put_blocking(), bounded -- returns false (word NOT written)
 * instead of spinning forever once `timeout_us` elapses waiting for TX
 * FIFO room. */
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
        /* One 'p' per pixel INDEX attempted, emitted before the
         * potentially-hanging call itself -- see this file's own include
         * comment above. Counting 'p's in the next crash report pins down
         * exactly which pixel the freeze lands on, distinct from whether
         * it happens at all (services/lighting.c's own 'w'/'x' pair). */
        tiles_debug_trace('p');
        /* Left-align the 24-bit GRB word in the 32-bit FIFO slot to
         * match the pull_threshold=24, shift-left OSR config above. */
        if (!sk6805_put_blocking_with_timeout(chain->pio, chain->sm, pixels[i] << 8u, TILES_PIO_TIMEOUT_US)) {
            /* State machine isn't draining -- bail on the rest of THIS
             * write rather than spin forever (see this file's own header
             * comment). Still fall through to the reset/latch wait below
             * rather than returning early, so timing stays consistent
             * for whatever DID get written and the next call starts
             * clean. The caller's next tiles_lighting_service() pass
             * naturally retries with fresh, current pixel data -- no
             * partial-frame state to reconcile here. */
            break;
        }
    }
    /* Marks "the per-pixel loop above fully exited" -- the very next
     * real crash report after 'q' shipped cut off RIGHT HERE: all 4
     * underglow 'p's present, 'q' present, then nothing -- no caller-
     * side 'x' ever fired. That bisects the freeze to this exact call,
     * previously sleep_us(), which for anything above PICO_TIME_SLEEP_
     * OVERHEAD_ADJUST_US (6us, pico-sdk default -- 300us here is nowhere
     * close) does NOT busy-wait despite this file's own header comment
     * saying otherwise: pico_time/time.c's sleep_until() calls
     * add_alarm_at() to schedule a hardware alarm IRQ, then blocks on a
     * spinlock waiting for that alarm's callback to notify it -- a real
     * dependency on the timer/alarm-pool interrupt subsystem actually
     * firing, not a plain register poll. If anything about that
     * notification is ever missed (a lost wake-up, an alarm queued
     * during some other code's interrupt-disabled window -- see this
     * codebase's own flash-save entry on disabling interrupts for
     * "tens of milliseconds" -- or any other alarm-pool-level fault),
     * this call blocks forever with no timeout of its own, and nothing
     * upstream (the 5ms-bounded PIO wait above, already returned by
     * this point) can catch it. busy_wait_us() (hardware/timer.h, also
     * already pulled in transitively via pico/time.h) is what this
     * file's own header comment already claimed this did -- a genuine
     * `while (raw timer register < target) tight_loop_contents();`
     * spin against the hardware counter directly, no IRQ, no alarm
     * pool, nothing else that could go missing. For a fixed 300us
     * latch delay this isn't a workaround, it's the more correct
     * primitive to begin with -- sleep_us() exists for cases that
     * actually want to yield/save power over a longer wait, neither of
     * which applies to a delay this short and this timing-critical. */
    tiles_debug_trace('q');
    busy_wait_us(TILES_SK6805_RESET_LOW_US);
}
