#pragma once

/* Light effects while playing: each note a pad plays lights it and the pads
 * around it, the way a backlit mechanical keyboard answers each keystroke.
 * Picked in the Light effects menu (expression_control.h: Feel menu, then
 * circle + square), setting look.play_effect. Pure, no hardware:
 * services/lighting.c draws it over the pads' normal look, dimmed to
 * TILES_LIGHTING_FX_REST_LEVEL (the modes it lights itself), and over the
 * step grids in a quieter sequencer version (tiles_fx_set_sequencer());
 * expression.c, op_mode.c and drum_seq.c report the notes. Host-tested in
 * test/test_light_effects.c.
 *
 * Pads are logical 1-24 (1 = top-left, row-major), as everywhere. */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TILES_FX_OFF = 0,
    /* White (menu row 1, after Off) */
    TILES_FX_AFTERGLOW,  /* a released pad fades out over ~0.3 s */
    TILES_FX_RIPPLE,     /* a white ring spreads from each hit */
    TILES_FX_CROSS,      /* light runs along the hit's row and column */
    TILES_FX_HALO,       /* the neighbours glow, nearer brighter */
    TILES_FX_WARMTH,     /* played pads warm up and cool over seconds */
    /* Colour (menu row 2). Velocity picks the hue: soft a cool blue,
     * through violet and magenta, hard a hot pink. */
    TILES_FX_PRISM_RIPPLE, /* the ring starts at that hue, shifts as it travels */
    TILES_FX_TOUCH_COLOUR, /* blue soft .. violet .. magenta hard; held, then fades */
    TILES_FX_HEATMAP,      /* warmth as blue, violet, magenta, pale pink;
                            * a hard hit heats more */
    TILES_FX_FIREWORKS,    /* sparks onto nearby pads, hues around that one */
    TILES_FX_RISE,         /* a level-meter bar up the hit's column */
    TILES_FX_INK,          /* the hit's colour bleeds into its neighbours */
    TILES_FX_COUNT,
} tiles_fx_t;

void tiles_fx_set(tiles_fx_t fx);
tiles_fx_t tiles_fx_get(void);

/* A note struck on `pad` (1-24) with `velocity`; and its release. */
void tiles_fx_hit(uint8_t pad, uint8_t velocity, uint32_t now_ms);
void tiles_fx_release(uint8_t pad, uint32_t now_ms);

/* A step the playhead sounds: fainter than a pad pressed live, and
 * released at once (Afterglow and Touch colour fade from it, a trail
 * behind the playhead). */
void tiles_fx_playback_hit(uint8_t pad, uint8_t velocity, uint32_t now_ms);

/* Sequencer versions (the step grids: Sequencer, Drums, Volca), so the
 * steps stay readable: every effect fainter, and the travelling ones
 * stay near the hit (Ripple and Prism ripple a pad, Cross two, Fireworks
 * two sparks next to it, Rise two rows). Set by services/lighting.c with
 * the grid's effects (tiles_lighting_set_grid_effects()). */
void tiles_fx_set_sequencer(bool on);

/* Once per lighting pass: the slow fields (warmth, ink) move on. */
void tiles_fx_step(uint32_t now_ms);

/* The effect's light on `pad` now (0-1 RGB, to blend over its normal
 * look); false = none. */
bool tiles_fx_pad_light(uint8_t pad, uint32_t now_ms, float rgb[3]);

/* A held pad's colour when the effect replaces the pressed white (Touch
 * colour); false = the usual white. */
bool tiles_fx_pressed_colour(uint8_t pad, float rgb[3]);

/* True while anything is still animating (the lighting then spends extra
 * pad writes on it). */
bool tiles_fx_busy(uint32_t now_ms);
