#include "chip.h"

#include "board_pins.h"
#include "kv_store.h"

#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/regs/addressmap.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "pico/time.h"

extern char __flash_binary_end;

uint32_t tiles_chip_clk_khz(void) {
    return clock_get_hz(clk_sys) / 1000u;
}

uint32_t tiles_chip_vreg_mv(void) {
    uint32_t code = (uint32_t)vreg_get_voltage();
    return code <= (uint32_t)VREG_VOLTAGE_1_40 ? 550u + 50u * code : 0u; /* 50 mV steps up to 1.40 V */
}

float tiles_chip_temp_c(void) {
    static bool on;
    if (!on) {
        adc_set_temp_sensor_enabled(true);
        sleep_us(50); /* the sensor's bias settling */
        on = true;
    }
    adc_select_input(ADC_TEMPERATURE_CHANNEL_NUM);
    (void)adc_read(); /* the mux settling */
    uint16_t raw = adc_read();
    adc_select_input(TILES_PEDAL_ADC_CHANNEL);
    float v = (float)raw * (3.3f / 4096.0f);
    return 27.0f - (v - 0.706f) / 0.001721f; /* the datasheet's line */
}

uint32_t tiles_chip_image_crc(uint32_t *bytes) {
    uint32_t n = (uint32_t)((uintptr_t)&__flash_binary_end - XIP_BASE);
    const volatile uint32_t *flash = (const volatile uint32_t *)XIP_NOCACHE_NOALLOC_BASE;
    uint32_t crc = 0u;
    for (uint32_t i = 0; i < n; i += 4u) {
        uint32_t w = flash[i / 4u];
        uint8_t b[4] = {(uint8_t)w, (uint8_t)(w >> 8), (uint8_t)(w >> 16), (uint8_t)(w >> 24)};
        crc = tiles_kv_crc32(crc, b, n - i < 4u ? n - i : 4u);
        if ((i & 0xFFFFu) == 0u) {
            watchdog_update();
        }
    }
    *bytes = n;
    return crc;
}
