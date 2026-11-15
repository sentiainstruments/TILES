#include "fault.h"

#include "hardware/structs/scb.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/platform/sections.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define FAULT_MAGIC 0xFA017EC0u

static uint32_t __uninitialized_ram(s_magic);
static tiles_fault_t __uninitialized_ram(s_record);
static tiles_fault_t s_last;
static bool s_have_last;

void tiles_fault_boot(void) {
    s_have_last = s_magic == FAULT_MAGIC;
    if (s_have_last) {
        s_last = s_record;
    }
    s_magic = 0u;
}

bool tiles_fault_last(tiles_fault_t *out) {
    if (s_have_last) {
        *out = s_last;
    }
    return s_have_last;
}

/* Return addresses (odd, in the program's flash or RAM code) above sp. */
static void __not_in_flash_func(find_callers)(const uint32_t *sp, uint32_t words) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < words && n < 4u; i++) {
        uint32_t v = sp[i];
        if ((v & 1u) && ((v >= 0x10000000u && v < 0x10400000u) || (v >= 0x20000000u && v < 0x20082000u))) {
            s_record.callers[n++] = v & ~1u;
        }
    }
    for (; n < 4u; n++) {
        s_record.callers[n] = 0u;
    }
}

/* The SDK's panic() lands here (PICO_PANIC_FUNCTION): its words kept. */
void __attribute__((noreturn)) tiles_panic(const char *fmt, ...) {
    uint32_t here;
    memset(&s_record, 0, sizeof(s_record));
    s_record.core = *(volatile uint32_t *)0xD0000000u;
    s_record.sp = (uint32_t)(uintptr_t)&here;
    find_callers(&here, 48u);
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_record.message, sizeof(s_record.message), fmt, args);
    va_end(args);
    s_magic = FAULT_MAGIC;
    watchdog_reboot(0, 0, 0);
    for (;;) {
        __wfi();
    }
}

/* frame: r0-r3, r12, lr, pc, xpsr as the exception pushed them. */
void __used __not_in_flash_func(tiles_fault_record)(const uint32_t *frame) {
    s_record.core = *(volatile uint32_t *)0xD0000000u; /* SIO CPUID */
    s_record.lr = frame[5];
    s_record.pc = frame[6];
    s_record.psr = frame[7];
    s_record.sp = (uint32_t)(uintptr_t)frame;
    s_record.cfsr = *(volatile uint32_t *)0xE000ED28u;
    s_record.hfsr = *(volatile uint32_t *)0xE000ED2Cu;
    s_record.mmfar = *(volatile uint32_t *)0xE000ED34u;
    s_record.bfar = *(volatile uint32_t *)0xE000ED38u;
    s_record.message[0] = '\0';
    find_callers(frame + 8, 32u);
    s_magic = FAULT_MAGIC;
    watchdog_reboot(0, 0, 0);
    for (;;) {
        __wfi();
    }
}

/* The stack the faulting code used: MSP or PSP by EXC_RETURN bit 2. */
void __attribute__((naked)) __not_in_flash_func(isr_hardfault)(void) {
    __asm volatile(
        "movs r0, #4\n"
        "mov r1, lr\n"
        "tst r0, r1\n"
        "beq 1f\n"
        "mrs r0, psp\n"
        "b tiles_fault_record\n"
        "1:\n"
        "mrs r0, msp\n"
        "b tiles_fault_record\n");
}
