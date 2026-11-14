#pragma once

/* Where boot got to, kept across a watchdog reset: after a hang during
 * boot (or in the loop), INFO says which step it was in (boot.hang). */

#include <stdbool.h>

/* First thing in main(), with watchdog_enable_caused_reboot(). */
void tiles_boot_stage_start(bool crash_recovered);

/* Before each boot step; `name` is a string literal. */
void tiles_boot_stage(const char *name);

/* The step the last boot hung in, or NULL if it didn't. */
const char *tiles_boot_stage_last_hang(void);
