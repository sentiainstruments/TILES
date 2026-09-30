#pragma once

/* Pico 2 GPIO map and bus/device addresses for SENTIA TILES Rev A0.
 *
 * Source of truth: docs/hardware/sentia_tiles_board_map_v1.json and
 * SENTIA_TILES_FIRMWARE_HANDOFF.md. This file is a transcription: change
 * those first. */

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

/* PCA9685 shared OE (pin 23 on both chips, active low), per the fab
 * flying-probe netlist (NET_25). NOT an address strap, despite the
 * original handoff doc; don't bring back the old name
 * TILES_GPIO_PCA9685_ADDR_STRAP. The net has only a 10k pull-up (R66) and
 * the two OE inputs, so driving it low is uncontended. Left high-Z at boot
 * (all PCA9685 outputs off) until board_pca9685_enable_outputs(), which
 * must only run once every channel is configured: OE makes the registers
 * (or the POR pin-low state, if a chip failed init) live at once. */
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

/* 0x40/0x41, not the 0x60/0x61 first documented: the flying-probe netlist
 * shows A1-A5 grounded on both chips and only U_HAPTIC2's A0 high.
 * Confirmed on hardware 2026-08-21. */
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
