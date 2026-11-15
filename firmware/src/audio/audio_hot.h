#pragma once

/* Audio code runs from RAM on the board (docs/architecture/audio-engine.md,
 * "Rules the test settled": from flash it is ~2x slower); plain on the
 * computer. */
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#include "pico/platform/sections.h"
#define TILES_AUDIO_HOT(f) __no_inline_not_in_flash_func(f)
#else
#define TILES_AUDIO_HOT(f) f
#endif
