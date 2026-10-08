#pragma once

/* Pad LEDs and underglow.
 *
 * Pad brightness is a flat fraction of services/power.h's
 * led_brightness_ceiling_percent (50% USB-only, 90% with external power;
 * budget in power.c). Deliberately NOT load-aware: a ceiling that rose when
 * fewer pads were lit made the whole board's brightness shift with
 * playing, which looked like a brownout. A pad's brightness for a given
 * state only changes when the power mode changes. Never raise pad
 * brightness around tiles_power_get_state().
 *
 * Underglow is always on at its own fixed, high level
 * (TILES_LIGHTING_UNDERGLOW_LEVEL), independent of the ceiling: 4 LEDs are
 * negligible current.
 *
 * Other modules take over the LEDs through the standby hooks below
 * (standby, boot animation, menus, games). */

#include <stdbool.h>
#include <stdint.h>

/* Claims the underglow (GP8) and pad (GP3) SK6805 chains and the TCA9554
 * LED mux, lights the underglow, and writes all 24 pads once (pixels
 * latch). Call after board_i2c_init(). False if a PIO or I2C resource is
 * unavailable. */
bool tiles_lighting_init(void);

/* Sets pad 1-24's press level: 0.0 = its idle look, 1.0 = the ceiling.
 * Applied when tiles_lighting_service() next reaches the pad. Ignored while
 * standby is active, so touch/expression can call it every scan without
 * knowing about standby; the next call after standby restores the pad. */
void tiles_lighting_set_pad_press(uint8_t logical_pad, float press_0_to_1);

/* The tunable "look" of the pads: resting brightness tiers and echo
 * colors, each a saved setting (look.* in profiles/settings_table.c).
 * Values are whole percent of the ceiling (tints: percent of full), except
 * the echo flash in ms. None can raise the ceiling. Clamped to <= 100% and
 * <= 2000 ms. Defaults are the constants at the top of lighting.c. */
typedef enum {
    TILES_LOOK_IDLE_BASELINE_PERCENT = 0, /* "visible but not active": pressed floor, bass frets, chord strip */
    TILES_LOOK_ECHO_SUSTAIN_TINT_PERCENT, /* echo (primary, green): white kept in the sustain */
    TILES_LOOK_ECHO_SECONDARY_G_PERCENT,  /* second TILES DISPLAY (soft red): settled green */
    TILES_LOOK_ECHO_SECONDARY_B_PERCENT,  /* second TILES DISPLAY (soft red): settled blue */
    TILES_LOOK_ECHO_FLASH_MS,             /* echo onset flash; 0 = none */
    TILES_LOOK_COUNT
} tiles_look_param_t;

uint16_t tiles_lighting_get_look(tiles_look_param_t param);
void tiles_lighting_set_look(tiles_look_param_t param, uint16_t value);

/* Colour scheme: the idle colour of each note role in melodic mode and
 * chord mode's melody grid, as 0xRRGGBB -- what the pad shows before the
 * power ceiling, so brightness is in the value (0x363636 is a dim white).
 * TILES_LIGHTING_COLOR_NONE means "no highlight for this role": the pad
 * falls through to the next one. Priority: root > fifth > third >
 * accidental (a sharp/flat pitch class) > note. Each is a saved setting
 * (color.* in profiles/settings_table.c); the companion app edits schemes
 * through those. Defaults are the device's original look. */
#define TILES_LIGHTING_COLOR_NONE 0x01000000u
typedef enum {
    TILES_SCHEME_ROOT = 0,
    TILES_SCHEME_THIRD,
    TILES_SCHEME_FIFTH,
    TILES_SCHEME_NOTE,
    TILES_SCHEME_ACCIDENTAL,
    TILES_SCHEME_COUNT
} tiles_scheme_role_t;
void tiles_lighting_set_scheme_color(tiles_scheme_role_t role, uint32_t rgb_or_none);
uint32_t tiles_lighting_get_scheme_color(tiles_scheme_role_t role);

/* Custom pad colours (advanced): with custom pads on, a pad that has its
 * own colour shows it in melodic mode instead of its role colour; pads set
 * to TILES_LIGHTING_COLOR_NONE keep the scheme. Pressed pads, the echo and
 * Song capture still draw over it. logical_pad 1-24. */
void tiles_lighting_set_custom_pads_enabled(bool enabled);
bool tiles_lighting_get_custom_pads_enabled(void);
void tiles_lighting_set_pad_color(uint8_t logical_pad, uint32_t rgb_or_none);
uint32_t tiles_lighting_get_pad_color(uint8_t logical_pad);

/* Rewrites pads round-robin via the required mux sequence (disable all ->
 * select -> enable one bank -> send one pixel -> latch -> disable), plus
 * immediate writes for pads whose state just changed. Call every main-loop
 * pass. */
void tiles_lighting_service(void);

/* ---- Standby / takeover hooks ---------------------------------------------
 * Let another module (standby, boot animation, menus, games) drive the
 * pads and underglow without fighting touch's per-scan press updates, and
 * without its own copy of the ceiling math. */

/* true: press updates are ignored and only the standby setters below reach
 * the LEDs. false: the underglow returns to its default at once; pads
 * repaint on their next press update (touch calls every scan). */
void tiles_lighting_set_standby_active(bool active);

/* Sets a pad's color, each channel 0.0-1.0 scaled by the ceiling. No idle
 * floor: {0,0,0} is black. No-op unless standby is active. */
void tiles_lighting_set_standby_pad_rgb(uint8_t logical_pad, float r, float g, float b);

/* Sets underglow pixel 0-3 (chain order), each channel 0.0-1.0 scaled by
 * TILES_LIGHTING_UNDERGLOW_LEVEL. No-op unless standby is active. */
void tiles_lighting_set_standby_underglow_rgb(uint8_t pixel_index, float r, float g, float b);

/* Bench test (settings shell `TEST LEDS`): every pad and underglow pixel
 * plain white at percent_0_to_100 of full scale, NOT the power ceiling, for
 * measuring LED current. 0 ends it and normal rendering resumes. Bypasses
 * standby and every override while on. */
void tiles_lighting_set_test_white(uint8_t percent_0_to_100);
uint8_t tiles_lighting_get_test_white(void);
