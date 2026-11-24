#pragma once

/* One RAM area for the effects' delay lines (audio/fx.c), the biggest user
 * of memory on the board. The pad capture (diagnostics/capture.c) borrows
 * it while the delay and reverb go quiet, and gives it back on CAPTURE
 * RELEASE; the granular tape borrows the delay's line (audio_engine.c). */

#include <stdbool.h>
#include <stdint.h>

/* The plate (40,960) and the ping-pong delay (48,016), both at 24 kHz,
 * and the ensemble (4,096). */
#define TILES_FX_MEMORY_BYTES 93072u

/* The area. */
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

/* Core 1, just before the area is lent: whatever still writes into it
 * stops (the granular tape's copy, audio_engine.c). */
void tiles_fx_memory_set_lend_hook(void (*hook)(void));
