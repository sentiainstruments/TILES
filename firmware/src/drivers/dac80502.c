#include "dac80502.h"

#include "board_pins.h"

#include "hardware/gpio.h"
#include "hardware/spi.h"

/* DAC8050x family's own documented 24-bit SPI frame: byte 0 is R/W (bit
 * 7, 0=write) + 3 reserved bits + 4-bit register address; bytes 1-2 are
 * the 16-bit register value, MSB first. SCLK idle low, data valid on
 * the falling edge (SPI mode 1: CPOL=0, CPHA=1), per the family's own
 * documented timing -- not yet confirmed against a logic analyzer on
 * this specific board, see this file's own header comment. */
#define DAC80502_REG_GAIN 0x04u
#define DAC80502_REG_DAC_A 0x08u
#define DAC80502_REG_DAC_B 0x09u

/* GAIN register bit layout: bit 8 = REF-DIV (0 = use the internal 2.5V
 * reference undivided), bit 1 = BUFF-GAIN-B, bit 0 = BUFF-GAIN-A (each
 * 0 = 1x output buffer gain). All zero is exactly the "0-2.5V straight
 * off the internal reference, no extra DAC-side gain" configuration
 * this board's external OPA2990 gain-of-4 stage assumes. */
#define DAC80502_GAIN_REF_DIV_0_BUFF_1X 0x0000u

#define DAC80502_SPI_HZ 1000000u /* first-attempt guess, well under the family's ~50MHz ceiling -- not yet tuned against real hardware */

static void write_register(uint8_t address, uint16_t value) {
    uint8_t frame[3] = {
        (uint8_t)(address & 0x0Fu),
        (uint8_t)(value >> 8),
        (uint8_t)(value & 0xFFu),
    };
    gpio_put(TILES_GPIO_DAC_SYNC_N, false); /* select -- active-low */
    spi_write_blocking(spi1, frame, sizeof(frame));
    gpio_put(TILES_GPIO_DAC_SYNC_N, true); /* deselect */
}

void tiles_dac80502_init(void) {
    spi_init(spi1, DAC80502_SPI_HZ);
    spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST);
    gpio_set_function(TILES_GPIO_DAC_SCLK, GPIO_FUNC_SPI);
    gpio_set_function(TILES_GPIO_DAC_MOSI, GPIO_FUNC_SPI);
    /* TILES_GPIO_DAC_SYNC_N is deliberately NOT reconfigured here -- it
     * stays the plain GPIO output board_init.c already set up
     * (deselected/high), since this driver bit-bangs chip select
     * itself rather than using the RP2350's hardware SPI CS. */

    write_register(DAC80502_REG_GAIN, DAC80502_GAIN_REF_DIV_0_BUFF_1X);
    write_register(DAC80502_REG_DAC_A, 0u);
    write_register(DAC80502_REG_DAC_B, 0u);
}

void tiles_dac80502_write(tiles_dac80502_channel_t channel, uint16_t code) {
    write_register(channel == TILES_DAC80502_CHANNEL_A ? DAC80502_REG_DAC_A : DAC80502_REG_DAC_B, code);
}
