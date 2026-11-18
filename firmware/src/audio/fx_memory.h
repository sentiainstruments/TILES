#pragma once

/* One RAM area for the effects' delay lines (audio/fx.c), the biggest user
 * of memory on the board. Two others use it when the effects don't: the
 * wavetable build at boot, before core 1 starts, and the pad capture
 * (diagnostics/capture.c), which borrows it while the delay and reverb go
 * quiet, and gives it back on CAPTURE RELEASE. */

#include <stdbool.h>
#include <stdint.h>

/* The plate (81,920), the ping-pong delay (96,016), the ensemble (4,096). */
#define TILES_FX_MEMORY_BYTES 182032u

/* The area, for the effects and the boot-time wavetable build. */
void *tiles_fx_memory(void);

/* Core 1, once its effects run: from then on a borrow waits for it. */
void tiles_fx_memory_activate(void);

/* Core 0: the area for a while, the effects going quiet first (core 1 lets
 * go between two render blocks); NULL if it didn't within 20 ms. */
void *tiles_fx_memory_borrow(void);

/* Core 0: back to the effects, which clear it before using it again. */
void tiles_fx_memory_return(void);

bool tiles_fx_memory_lent(void);

/* Core 1, every pass: lets go of the area, or takes it back cleared. */
void tiles_fx_memory_service(void);
