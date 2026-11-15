#pragma once

/* A hard fault or a panic() on either core is recorded in RAM a reset keeps, and the
 * chip restarts (the watchdog only sees core 0, so a fault on core 1 used
 * to leave USB dead with core 0 running). The next boot reports it in
 * INFO (fault.*). */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t core, pc, lr, psr, cfsr, hfsr, mmfar, bfar, sp;
    uint32_t callers[4]; /* code addresses found up the stack, newest first */
    char message[80];    /* a panic's own words ("" for a fault) */
} tiles_fault_t;

/* First thing in main: keeps the last boot's record, if any. */
void tiles_fault_boot(void);

/* The fault that ended the last boot; false if it ended cleanly. */
bool tiles_fault_last(tiles_fault_t *out);

/* Core 1's pulse, bumped by its loop every pass. */
extern volatile uint32_t g_tiles_core1_beat;

/* Core 1, once at its start: lets core 0 interrupt it when it stops. */
void tiles_fault_core1_arm(void);

/* Core 0's loop: core 1 still for a second is recorded as a fault (where
 * it was stuck, if it takes the interrupt) and the chip restarts. */
void tiles_fault_watch_core1(void);
