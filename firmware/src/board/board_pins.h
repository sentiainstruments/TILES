#pragma once

/*
 * Pico 2 GPIO map and bus/device addresses for SENTIA TILES Rev A0.
 *
 * Source of truth: docs/hardware/sentia_tiles_board_map_v1.json and
 * SENTIA_TILES_FIRMWARE_HANDOFF.md. Do not edit a value here without
 * updating those first — this file is a transcription, not a design.
 */

#include <stdint.h>

/* ---- DIN MIDI --------------------------------------------------------- */

#define TILES_GPIO_DIN_MIDI_OUT_A 0u    /* drive high before enabling MIDI OUT */
#define TILES_GPIO_DIN_MIDI_IN_RX 1u
#define TILES_GPIO_DIN_MIDI_OUT_B 2u    /* drive high before enabling MIDI OUT */

/* ---- Pad LEDs / underglow --------------------------------------------- */

#define TILES_GPIO_PAD_LED_DATA 3u      /* boot: low */
#define TILES_GPIO_UNDERGLOW_DATA 8u    /* boot: low */

/* ---- I2C buses ---------------------------------------------------------
 * I2C0: Hall muxes + touch controllers.
 * I2C1: haptic PWM controllers + LED mux controller.
 */

#define TILES_GPIO_I2C0_SDA 4u
#define TILES_GPIO_I2C0_SCL 5u
#define TILES_GPIO_I2C1_SDA 6u
#define TILES_GPIO_I2C1_SCL 7u

#define TILES_I2C_DETECT_HZ 100000u
#define TILES_I2C_RUN_HZ 400000u

/* ---- CV / gate (DAC80502 over SPI) ------------------------------------- */

#define TILES_GPIO_DAC_SCLK 10u
#define TILES_GPIO_DAC_MOSI 11u
#define TILES_GPIO_GATE_PWM 12u         /* boot: low */
#define TILES_GPIO_DAC_SYNC_N 13u       /* active-low chip select; boot: high (deselected) */

/* ---- Function buttons (active low, hardware pullups) ------------------- */

#define TILES_GPIO_SW6_CIRCLE 14u
#define TILES_GPIO_SW5_SQUARE 15u
#define TILES_GPIO_SW3_TRIANGLE 16u
#define TILES_GPIO_SW4_DIAMOND 17u
#define TILES_GPIO_SW2_RIGHT_CAPSULE 18u
#define TILES_GPIO_SW1_LEFT_CAPSULE 19u

/* ---- Hazard / status pins ----------------------------------------------- */

/* CORRECTED 2026-08-21: NOT an A5 address strap, despite the original
 * hardware handoff doc's claim (and this constant's old name --
 * TILES_GPIO_PCA9685_ADDR_STRAP -- kept as a historical note, don't
 * reintroduce it). The real fabricated board's flying-probe netlist
 * (Gerber_PCB1_2026-08-21.zip/FlyingProbeTesting.json) shows this net
 * (NET_25) lands on pin 23 of both PCA9685 chips, which per the real
 * TSSOP-28 datasheet pinout is OE (active-low output enable), not A5
 * (pin 24, tied to GND on both chips per the same netlist -- the real
 * address pins are fixed by hardwired GND/3V3_OUT straps unrelated to
 * this net, giving addresses 0x40/0x41, confirmed against real hardware).
 *
 * NET_25 also has a 10k pull-up to 3V3_OUT (R66) and nothing else on it
 * besides these two OE *inputs* -- so driving this pin low only has to
 * overcome a weak 10k pull (~0.33mA), with no other active driver to
 * contend with. Left as input/high-Z at boot (OE sits disabled, every
 * PCA9685 output on both chips forced off) until
 * board_pca9685_enable_outputs() is called -- only safe to call after
 * every PCA9685 channel has already been configured to its intended
 * state, since enabling OE makes each chip's current register content
 * (or its POR default, LEDn=0/pin-low, if a chip's I2C init failed)
 * immediately live on the physical pins. */
#define TILES_GPIO_PCA9685_OE 20u

#define TILES_GPIO_TOUCH_IRQ_N 21u      /* shared MPR121 IRQ, active low */

/* TPS2121 ST power-source status. Low means external 12V (IN2) is
 * selected; high means USB (IN1) or the mux output is high-impedance. */
#define TILES_GPIO_POWER_SOURCE_STATUS 22u

/* ---- Pedal --------------------------------------------------------------- */

#define TILES_GPIO_PEDAL_ADC 26u
#define TILES_PEDAL_ADC_CHANNEL 0u

/* ---- Unused pins (input, no pull) ---------------------------------------- */

#define TILES_GPIO_UNUSED_9 9u
#define TILES_GPIO_UNUSED_27 27u
#define TILES_GPIO_UNUSED_28 28u

/* ---- I2C0 device addresses (Hall muxes + touch) -------------------------- */

#define TILES_I2C0_ADDR_HALL_MUX1 0x70u
#define TILES_I2C0_ADDR_HALL_MUX2 0x71u
#define TILES_I2C0_ADDR_HALL_MUX3 0x72u

/* All 24 TMAG5273A1 sensors share this address behind their mux channel;
 * only one mux channel across all three muxes may be enabled at a time. */
#define TILES_I2C0_ADDR_HALL_SENSOR 0x35u

#define TILES_I2C0_ADDR_TOUCH1 0x5Au
#define TILES_I2C0_ADDR_TOUCH2 0x5Bu

/* ---- I2C1 device addresses (haptics + LED mux control) -------------------- */

/* Corrected from the originally-documented 0x60/0x61. The real fabricated
 * board's flying-probe netlist (docs/hardware/ didn't have this level of
 * detail; see Gerber_PCB1_2026-08-21.zip/FlyingProbeTesting.json) shows
 * both PCA9685 chips' A1-A5 address pins tied to GND and only U_HAPTIC2's
 * A0 tied to 3V3_OUT -- giving 0x40 and 0x41, which is exactly what the
 * real board ACKs at. Confirmed against real hardware on 2026-08-21. */
#define TILES_I2C1_ADDR_HAPTIC_PCA9685_1 0x40u
#define TILES_I2C1_ADDR_HAPTIC_PCA9685_2 0x41u
#define TILES_I2C1_ADDR_LED_MUX_TCA9554 0x20u

/* ---- Counts ---------------------------------------------------------------- */

#define TILES_NUM_PADS 24u
#define TILES_NUM_HALL_MUXES 3u
#define TILES_NUM_TOUCH_CONTROLLERS 2u
#define TILES_NUM_HAPTIC_PWM_CONTROLLERS 2u
#define TILES_NUM_LED_MUXES 3u
#define TILES_NUM_FUNCTION_BUTTONS 6u
