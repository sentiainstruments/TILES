#include "fx_memory.h"

#include "fx.h"

#include <stddef.h>

#ifdef PICO_ON_DEVICE
#include "pico/time.h"
#endif

static uint32_t s_memory[TILES_FX_MEMORY_BYTES / 4u];
static volatile bool s_active; /* core 1's effects run: it must let go */
static volatile bool s_want;   /* core 0 wants the area */
static volatile bool s_lent;   /* core 1 has let go of it */

void *tiles_fx_memory(void) {
    return s_memory;
}

void tiles_fx_memory_activate(void) {
    s_active = true;
}

void *tiles_fx_memory_borrow(void) {
    s_want = true;
    if (!s_active) {
        s_lent = true;
        return s_memory;
    }
#ifdef PICO_ON_DEVICE
    uint32_t t0 = time_us_32();
    while (!s_lent && time_us_32() - t0 < 20000u) {
        tight_loop_contents();
    }
#endif
    if (!s_lent) {
        s_want = false; /* core 1 takes it back if it lets go later */
        return NULL;
    }
    return s_memory;
}

void tiles_fx_memory_return(void) {
    s_want = false;
    if (!s_active) {
        s_lent = false;
    }
}

bool tiles_fx_memory_lent(void) {
    return s_lent;
}

void tiles_fx_memory_service(void) {
    if (s_want && !s_lent) {
        tiles_fx_set_bypass(true);
        s_lent = true;
    } else if (!s_want && s_lent) {
        tiles_fx_init(s_memory);
        s_lent = false;
    }
}
