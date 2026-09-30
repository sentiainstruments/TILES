#include "dac80502.h"

#include "board_pins.h"

#include "hardware/gpio.h"
#include "hardware/spi.h"

/* 24-bit SPI frame: byte 0 = R/W (bit 7, 0 = write) + register address
 * (low 4 bits); bytes 1-2 = value, MSB first. SPI mode 1 (CPOL=0,
 * CPHA=1). From the family datasheet; not yet checked on a logic analyzer. */
#define DAC80502_REG_GAIN 0x04u
#define DAC80502_REG_DAC_A 0x08u
#define DAC80502_REG_DAC_B 0x09u

/* GAIN register: bit 8 REF-DIV, bits 1/0 BUFF-GAIN-B/A. All zero = 0-2.5 V
 * from the internal reference, which the external x4 stage assumes. */
#define DAC80502_GAIN_REF_DIV_0_BUFF_1X 0x0000u

#define DAC80502_SPI_HZ 1000000u /* conservative first value (chip allows ~50 MHz); untuned */

static void write_register(uint8_t address, uint16_t value) {
    uint8_t frame[3] = {
        (uint8_t)(address & 0x0Fu),
        (uint8_t)(value >> 8),
        (uint8_t)(value & 0xFFu),
    };
    gpio_put(TILES_GPIO_DAC_SYNC_N, false); /* select (active low) */
    spi_write_blocking(spi1, frame, sizeof(frame));
    gpio_put(TILES_GPIO_DAC_SYNC_N, true); /* deselect */
}

void tiles_dac80502_init(void) {
    spi_init(spi1, DAC80502_SPI_HZ);
    spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST);
    gpio_set_function(TILES_GPIO_DAC_SCLK, GPIO_FUNC_SPI);
    gpio_set_function(TILES_GPIO_DAC_MOSI, GPIO_FUNC_SPI);
    /* SYNC-N stays the GPIO board_init.c set up; chip select is bit-banged. */

    write_register(DAC80502_REG_GAIN, DAC80502_GAIN_REF_DIV_0_BUFF_1X);
    write_register(DAC80502_REG_DAC_A, 0u);
    write_register(DAC80502_REG_DAC_B, 0u);
}

void tiles_dac80502_write(tiles_dac80502_channel_t channel, uint16_t code) {
    write_register(channel == TILES_DAC80502_CHANNEL_A ? DAC80502_REG_DAC_A : DAC80502_REG_DAC_B, code);
}
