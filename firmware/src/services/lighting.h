#pragma once

/* Pad LEDs and underglow. Pad brightness is a flat fraction of power.h's
 * ceiling (50% USB, 90% external), never load-aware (that looked like a
 * brownout). The underglow has its own fixed level. Other modules take
 * over through the standby hooks below. */

#include <stdbool.h>
#include <stdint.h>

/* Claims the underglow (GP8) and pad (GP3) SK6805 chains and the TCA9554
 * mux, and writes every pixel once. After board_i2c_init(). */
bool tiles_lighting_init(void);

/* Pad 1-24's press level (0 = idle look, 1 = ceiling), applied on its next
 * service. Ignored during standby, so callers needn't check. */
void tiles_lighting_set_pad_press(uint8_t logical_pad, float press_0_to_1);

/* The tunable look (look.* settings): resting tiers, echo colours, pressure
 * hue and bend dimming, in percent of the ceiling (echo flash ms, hue
 * degrees). None can raise the ceiling. */
typedef enum {
    TILES_LOOK_IDLE_BASELINE_PERCENT = 0, /* "visible but not active": pressed floor, bass frets, chord strip */
    TILES_LOOK_ECHO_SUSTAIN_TINT_PERCENT, /* echo (primary, green): white kept in the sustain */
    TILES_LOOK_ECHO_SECONDARY_G_PERCENT,  /* second TILES DISPLAY (soft red): settled green */
    TILES_LOOK_ECHO_SECONDARY_B_PERCENT,  /* second TILES DISPLAY (soft red): settled blue */
    TILES_LOOK_ECHO_FLASH_MS,             /* echo onset flash; 0 = none */
    TILES_LOOK_PRESSURE_HUE_DEGREES,      /* hue turn at full pressure, degrees; 0 = off */
    TILES_LOOK_BEND_DIM_PERCENT,          /* brightness taken off at full bend; 0 = off */
    TILES_LOOK_LED_INTENSITY_PERCENT,     /* pads, underglow, buttons; % of the ceiling; 0 = off (Feel menu row 4) */
    TILES_LOOK_COUNT
} tiles_look_param_t;

uint16_t tiles_lighting_get_look(tiles_look_param_t param);
void tiles_lighting_set_look(tiles_look_param_t param, uint16_t value);

/* Colour scheme: each note role's idle colour (0xRRGGBB, brightness in the
 * value). COLOR_NONE = no highlight. Priority: root > fifth > third >
 * accidental > note. Settings color.*. */
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

/* Custom pad colours: in melodic mode a pad with its own colour shows it;
 * pressed pads, the echo and Song capture still draw over. Pad 1-24. */
void tiles_lighting_set_custom_pads_enabled(bool enabled);
bool tiles_lighting_get_custom_pads_enabled(void);
void tiles_lighting_set_pad_color(uint8_t logical_pad, uint32_t rgb_or_none);
uint32_t tiles_lighting_get_pad_color(uint8_t logical_pad);

/* Rewrites pads round-robin through the mux sequence, plus pads that just
 * changed. Every main-loop pass. */
void tiles_lighting_service(void);

/* ---- Standby / takeover hooks ---------------------------------------------
 * Let another module drive the LEDs without fighting touch's updates. */

/* true: press updates are ignored and only the standby setters below reach
 * the LEDs. false: the underglow returns to its default at once; pads
 * repaint on their next press update (touch calls every scan). */
void tiles_lighting_set_standby_active(bool active);

/* The standby frame is a step grid: the light effect draws its sequencer
 * version over it. op_mode.c sets it every scan. */
void tiles_lighting_set_grid_effects(bool on);

/* The underglow's colour outside standby: the active mode's colour (op_mode.c
 * sets it on every mode change). Modes that draw through standby set their
 * own, the same colour. */
void tiles_lighting_set_mode_underglow(float r, float g, float b);

/* Sets a pad's color, each channel 0.0-1.0 scaled by the ceiling. No idle
 * floor: {0,0,0} is black. No-op unless standby is active. */
void tiles_lighting_set_standby_pad_rgb(uint8_t logical_pad, float r, float g, float b);

/* Sends the whole frame now (~0.3 ms a pad), for callers before the main
 * loop (the boot animation). */
void tiles_lighting_flush_standby(void);

/* A melodic pad's resting look under the current note map and colour
 * scheme (no note: dark; a quarter tone: dim blue): the Layouts menu's
 * preview. */
void tiles_lighting_melodic_rest_rgb(uint8_t logical_pad, float rgb[3]);

/* Bass guitar mode's colour: its fretboard, menu pad and underglow. Lilac,
 * with melodic's magenta and Duet's pink: the modes played as an
 * instrument. */
#define TILES_LIGHTING_BASS_R 0.75f
#define TILES_LIGHTING_BASS_G 0.3f
#define TILES_LIGHTING_BASS_B 1.0f

/* Under a light effect the pads' resting look dims to this, for more range
 * between it and the effect's light (first guess). */
#define TILES_LIGHTING_FX_REST_LEVEL 0.3f

/* A whole standby frame (index 0 = pad 1), drawn a few pads at a time: the
 * `max_writes` pads that differ most from what they show. A pad write is
 * ~0.3 ms, so an animated frame sent whole would hold the main loop. */
void tiles_lighting_set_standby_frame(const float rgb[][3], uint8_t max_writes);

/* Sets underglow pixel 0-3 (chain order), each channel 0.0-1.0 scaled by
 * TILES_LIGHTING_UNDERGLOW_LEVEL. No-op unless standby is active. */
void tiles_lighting_set_standby_underglow_rgb(uint8_t pixel_index, float r, float g, float b);

/* Underglow as light from under the pads (moving modes): each LED takes
 * the brightest nearby light over a dim floor, fading over ~120 ms, sent
 * at most every 16 ms. `rgb[0]` = pad 1. Every scan, in standby. */
void tiles_lighting_set_standby_underglow_from_pads(const float rgb[][3], float base_r, float base_g, float base_b);

/* Bench test (TEST LEDS): everything white at percent of full scale, not
 * the ceiling, for measuring current. 0 ends it. */
void tiles_lighting_set_test_white(uint8_t percent_0_to_100);
uint8_t tiles_lighting_get_test_white(void);
