#pragma once

/* Standalone mode's sound and effects menus (README, "Sound menus"), opened
 * by op_mode.c's diamond outside the sequencer: diamond = the sound menu,
 * circle + diamond = the effects menu, for one of TILES's four synth
 * tracks at a time, starting at the mode's own. The sounds come a page of
 * 24 at a time (the 96 preset slots), each page in its own colours:
 * "-"/"+" step through the pages holding any (the top row shows the
 * page's number for a moment), circle + "-"/"+" through the tracks (in
 * the effects menu "-"/"+" alone do); a track's own page comes up with
 * it. While one is open it owns the pads and "-"/"+". */

#include <stdbool.h>
#include <stdint.h>

/* Opens the sound menu (effects false) or the effects menu on track 0-3. */
void tiles_sound_menu_open(bool effects, uint8_t track);
void tiles_sound_menu_close(void);
bool tiles_sound_menu_is_open(void);
bool tiles_sound_menu_is_effects(void);

/* Every op_mode scan while open: input, then the pads, keys and underglow. */
void tiles_sound_menu_scan(uint32_t now_ms);
