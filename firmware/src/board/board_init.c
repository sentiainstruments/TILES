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
    /* DIN MIDI OUT: both lines high before the MIDI OUT service ever
     * enables a stream on either one. (midi/din_midi.c later hands ONE of
     * these to a PIO UART, already high, and keeps the other parked high.) */
    init_output(TILES_GPIO_DIN_MIDI_OUT_A, true);
    init_output(TILES_GPIO_DIN_MIDI_OUT_B, true);

    /* Serialized data lines: idle low. */
    init_output(TILES_GPIO_PAD_LED_DATA, false);
    init_output(TILES_GPIO_UNDERGLOW_DATA, false);

    /* Gate output: low (no note) until the gate service explicitly
     * drives it, and only ever with external power confirmed. */
    init_output(TILES_GPIO_GATE_PWM, false);

    /* DAC80502 chip select: active-low, so "high" means deselected. */
    init_output(TILES_GPIO_DAC_SYNC_N, true);

    /* PCA9685 shared OE (see board_pins.h -- NOT an address strap).
     * Input/high-Z at boot so both chips' outputs stay disabled until
     * board_pca9685_enable_outputs() is called once channels are
     * actually configured. */
    init_input(TILES_GPIO_PCA9685_OE, false);

    /* DIN MIDI IN RX: plain input at boot. midi/din_midi.c reconfigures
     * this pin's function to UART0 RX when it starts. */
    init_input(TILES_GPIO_DIN_MIDI_IN_RX, false);

    /* Function buttons: active-low, hardware pullups already present;
     * internal pull-up adds margin, does not fight the hardware pull. */
    init_input(TILES_GPIO_SW1_LEFT_CAPSULE, true);
    init_input(TILES_GPIO_SW2_RIGHT_CAPSULE, true);
    init_input(TILES_GPIO_SW3_TRIANGLE, true);
    init_input(TILES_GPIO_SW4_DIAMOND, true);
    init_input(TILES_GPIO_SW5_SQUARE, true);
    init_input(TILES_GPIO_SW6_CIRCLE, true);

    /* Shared MPR121 interrupt: active-low, open-drain-style output on
     * the touch controllers; internal pull-up as a safety margin. */
    init_input(TILES_GPIO_TOUCH_IRQ_N, true);

    /* TPS2121 ST: push-pull output from the power mux, no pull needed. */
    init_input(TILES_GPIO_POWER_SOURCE_STATUS, false);

    /* Pedal ADC: plain digital input for now. The pedal service calls
     * adc_gpio_init() on this pin when it starts reading. */
    init_input(TILES_GPIO_PEDAL_ADC, false);

    /* Unused pins: input, no pull, per the board map. */
    init_input(TILES_GPIO_UNUSED_9, false);
    init_input(TILES_GPIO_UNUSED_27, false);
    init_input(TILES_GPIO_UNUSED_28, false);

    /* SPI (DAC) and I2C pins: leave as plain GPIO here. The DAC driver
     * claims GP10/GP11 for SPI when it initializes; board_i2c_init()
     * below claims GP4/GP5/GP6/GP7 for I2C. */
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

/* See board_init.h for the full rationale. Bit-bang recovery timing:
 * 5us per half-period (~100kHz recovery clock) -- not timing-critical
 * since no real data is being transferred, just nudging a stuck slave,
 * so this is deliberately slower/safer than either bus's actual run
 * speed rather than tuned to it. */
#define RECOVERY_HALF_PERIOD_US 5u
#define RECOVERY_MAX_CLOCK_PULSES 9u

void board_i2c_recover_bus(i2c_inst_t *bus) {
    uint sda_pin = (bus == i2c0) ? TILES_GPIO_I2C0_SDA : TILES_GPIO_I2C1_SDA;
    uint scl_pin = (bus == i2c0) ? TILES_GPIO_I2C0_SCL : TILES_GPIO_I2C1_SCL;

    gpio_set_function(sda_pin, GPIO_FUNC_SIO);
    gpio_set_function(scl_pin, GPIO_FUNC_SIO);
    /* Pre-arm the output value both lines will drive once switched to
     * GPIO_OUT below to false, once, up front -- from here on, "drive"
     * means gpio_set_dir(..., GPIO_OUT) and "release" means
     * gpio_set_dir(..., GPIO_IN) (the pull-ups board_i2c_init() already
     * enabled bring it back high), exactly matching real open-drain I2C
     * electrical behavior so this can never drive a hard high against
     * another device. */
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

    /* Manual STOP condition regardless of whether SDA ever freed up
     * above -- SDA released (high) while SCL is high -- so a downstream
     * device left mid-transaction sees a clean end to it either way. */
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
    /* Only ever transitions this pin from input to output here, once,
     * after the caller has already configured every PCA9685 channel.
     * See board_pins.h for why this is safe: the only other thing on
     * this net is a 10k pull-up to 3V3_OUT, no other active driver. */
    gpio_set_dir(TILES_GPIO_PCA9685_OE, GPIO_OUT);
    gpio_put(TILES_GPIO_PCA9685_OE, false);
}

void board_init(void) {
    board_gpio_init();
    board_i2c_init();
}
