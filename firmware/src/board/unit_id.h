#pragma once

/* Pre-production unit number ("2 of 4"), set per board before flashing, or
 * with cmake -DTILES_UNIT=N. Shown in the USB product name (so units stay
 * apart in a DAW), the CDC name, INFO and the boot print. */
#ifndef TILES_UNIT_NUMBER
#define TILES_UNIT_NUMBER 2u
#endif
#define TILES_UNIT_COUNT 4u
