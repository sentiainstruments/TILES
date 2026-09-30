#include "board_init.h"
#include "board_pins.h"

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/time.h"

static void init_output(uint gpio, bool initial_high) {
    gpio_init(gpio);
    gpio_set_dir(gpio, GPIO_OUT);
    gpio_put(gpio, initial_high);
}

static void init_input(uint gpio, bool pull_up) {
    gpio_init(gpio);
    gpio_set_dir(gpio, GPIO_IN);
    gpio_disable_pulls(gpio);
    if (pull_up) {
        gpio_pull_up(gpio);
    }
}

void board_gpio_init(void) {
    /* DIN MIDI OUT: both lines high before any stream starts. midi/din_midi.c
     * later hands one to a PIO UART and keeps the other parked high. */
    init_output(TILES_GPIO_DIN_MIDI_OUT_A, true);
    init_output(TILES_GPIO_DIN_MIDI_OUT_B, true);

    /* Serialized data lines: idle low. */
    init_output(TILES_GPIO_PAD_LED_DATA, false);
    init_output(TILES_GPIO_UNDERGLOW_DATA, false);

    /* Gate: low (no note) until the CV/gate service drives it. */
    init_output(TILES_GPIO_GATE_PWM, false);

    /* DAC80502 chip select, active low: high = deselected. */
    init_output(TILES_GPIO_DAC_SYNC_N, true);

    /* PCA9685 shared OE (see board_pins.h): high-Z at boot, so outputs stay
     * off until board_pca9685_enable_outputs(). */
    init_input(TILES_GPIO_PCA9685_OE, false);

    /* DIN MIDI IN RX: plain input; midi/din_midi.c switches it to UART0 RX. */
    init_input(TILES_GPIO_DIN_MIDI_IN_RX, false);

    /* Function buttons: active low with hardware pull-ups; the internal
     * pull-up only adds margin. */
    init_input(TILES_GPIO_SW1_LEFT_CAPSULE, true);
    init_input(TILES_GPIO_SW2_RIGHT_CAPSULE, true);
    init_input(TILES_GPIO_SW3_TRIANGLE, true);
    init_input(TILES_GPIO_SW4_DIAMOND, true);
    init_input(TILES_GPIO_SW5_SQUARE, true);
    init_input(TILES_GPIO_SW6_CIRCLE, true);

    /* Shared MPR121 IRQ: active low, open drain; internal pull-up as margin. */
    init_input(TILES_GPIO_TOUCH_IRQ_N, true);

    /* TPS2121 ST: push-pull output from the power mux, no pull needed. */
    init_input(TILES_GPIO_POWER_SOURCE_STATUS, false);

    /* Pedal: plain input until services/pedal.c calls adc_gpio_init(). */
    init_input(TILES_GPIO_PEDAL_ADC, false);

    /* Unused pins: input, no pull, per the board map. */
    init_input(TILES_GPIO_UNUSED_9, false);
    init_input(TILES_GPIO_UNUSED_27, false);
    init_input(TILES_GPIO_UNUSED_28, false);

    /* SPI (DAC) and I2C pins stay plain GPIO here: the DAC driver claims
     * GP10/GP11, board_i2c_init() claims GP4-GP7. */
}

void board_i2c_init(void) {
    i2c_init(i2c0, TILES_I2C_DETECT_HZ);
    gpio_set_function(TILES_GPIO_I2C0_SDA, GPIO_FUNC_I2C);
    gpio_set_function(TILES_GPIO_I2C0_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(TILES_GPIO_I2C0_SDA);
    gpio_pull_up(TILES_GPIO_I2C0_SCL);

    i2c_init(i2c1, TILES_I2C_DETECT_HZ);
    gpio_set_function(TILES_GPIO_I2C1_SDA, GPIO_FUNC_I2C);
    gpio_set_function(TILES_GPIO_I2C1_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(TILES_GPIO_I2C1_SDA);
    gpio_pull_up(TILES_GPIO_I2C1_SCL);
}

void board_i2c_set_run_speed(void) {
    i2c_set_baudrate(i2c0, TILES_I2C_RUN_HZ);
    i2c_set_baudrate(i2c1, TILES_I2C_RUN_HZ);
}

/* Recovery clock: 5 us half-period (~100 kHz). No data moves, so slow and
 * safe is fine. */
#define RECOVERY_HALF_PERIOD_US 5u
#define RECOVERY_MAX_CLOCK_PULSES 9u

void board_i2c_recover_bus(i2c_inst_t *bus) {
    uint sda_pin = (bus == i2c0) ? TILES_GPIO_I2C0_SDA : TILES_GPIO_I2C1_SDA;
    uint scl_pin = (bus == i2c0) ? TILES_GPIO_I2C0_SCL : TILES_GPIO_I2C1_SCL;

    gpio_set_function(sda_pin, GPIO_FUNC_SIO);
    gpio_set_function(scl_pin, GPIO_FUNC_SIO);
    /* Output value preset low once: from here "drive" = set as output, and
     * "release" = set as input (the pull-up brings it high), exactly like
     * open-drain I2C, so a line is never driven hard high. */
    gpio_put(sda_pin, false);
    gpio_put(scl_pin, false);
    gpio_set_dir(sda_pin, GPIO_IN);
    gpio_set_dir(scl_pin, GPIO_IN);

    for (uint pulse = 0; pulse < RECOVERY_MAX_CLOCK_PULSES && !gpio_get(sda_pin); pulse++) {
        gpio_set_dir(scl_pin, GPIO_OUT);
        sleep_us(RECOVERY_HALF_PERIOD_US);
        gpio_set_dir(scl_pin, GPIO_IN);
        sleep_us(RECOVERY_HALF_PERIOD_US);
    }

    /* Manual STOP whether or not SDA freed up (SDA rises while SCL is high),
     * so any device left mid-transaction sees it end. */
    gpio_set_dir(sda_pin, GPIO_OUT);
    sleep_us(RECOVERY_HALF_PERIOD_US);
    gpio_set_dir(scl_pin, GPIO_IN);
    sleep_us(RECOVERY_HALF_PERIOD_US);
    gpio_set_dir(sda_pin, GPIO_IN);
    sleep_us(RECOVERY_HALF_PERIOD_US);

    gpio_set_function(sda_pin, GPIO_FUNC_I2C);
    gpio_set_function(scl_pin, GPIO_FUNC_I2C);
    i2c_init(bus, TILES_I2C_RUN_HZ);
}

void board_pca9685_enable_outputs(void) {
    /* Input -> output once, after the caller configured every channel. Nothing
     * else drives this net (board_pins.h). */
    gpio_set_dir(TILES_GPIO_PCA9685_OE, GPIO_OUT);
    gpio_put(TILES_GPIO_PCA9685_OE, false);
}

void board_init(void) {
    board_gpio_init();
    board_i2c_init();
}
