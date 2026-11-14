#include "boot_stage.h"

#include "hardware/regs/addressmap.h"
#include "pico/platform/sections.h"

#include <stdint.h>

#define STAGE_MAGIC 0xB0075A6Eu

/* RAM a watchdog reset leaves alone; random after power-up, hence the
 * magic and the check that the name is in this image's flash. */
static uint32_t __uninitialized_ram(s_stage_magic);
static const char *__uninitialized_ram(s_stage_name);
static const char *s_last_hang;

extern char __flash_binary_end;

void tiles_boot_stage_start(bool crash_recovered) {
    uintptr_t p = (uintptr_t)s_stage_name;
    if (crash_recovered && s_stage_magic == STAGE_MAGIC && p >= XIP_BASE &&
        p < (uintptr_t)&__flash_binary_end) {
        s_last_hang = s_stage_name;
    }
    s_stage_magic = STAGE_MAGIC;
    s_stage_name = "start";
}

void tiles_boot_stage(const char *name) {
    s_stage_name = name;
}

const char *tiles_boot_stage_last_hang(void) {
    return s_last_hang;
}
