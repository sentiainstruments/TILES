#pragma once

/* Audio code runs from RAM on the board (docs/architecture/audio-engine.md,
 * "Rules the test settled": from flash it is ~2x slower); plain on the
 * computer. */
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#include "pico/platform/sections.h"
#define TILES_AUDIO_HOT(f) __no_inline_not_in_flash_func(f)
/* Rare code (setup, settings) in a file the link places in RAM
 * (src/link/default_text_excludes.incl): left in flash. */
#define TILES_AUDIO_COLD __attribute__((noinline, section(".coldtext")))
#else
#define TILES_AUDIO_HOT(f) f
#define TILES_AUDIO_COLD
#endif
