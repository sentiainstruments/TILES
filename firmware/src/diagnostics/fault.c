#include "fault.h"

#include "hardware/irq.h"
#include "hardware/structs/scb.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/multicore.h"
#include "pico/platform/sections.h"
#include "pico/time.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define FAULT_MAGIC 0xFA017EC0u
/* Core 1's loop passes every ~0.2 ms; a whole second still is a hang. */
#define CORE1_STILL_US 1000000u
/* Core 0 away this long (a flash write pauses core 1): start over. */
#define WATCH_GAP_US 100000u
/* Core 1 not taking the doorbell by then: its interrupts are off. */
#define BELL_WAIT_US 20000u

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
static void __attribute__((noreturn)) __not_in_flash_func(record_frame)(const uint32_t *frame, const char *message) {
    s_record.core = *(volatile uint32_t *)0xD0000000u; /* SIO CPUID */
    s_record.lr = frame[5];
    s_record.pc = frame[6];
    s_record.psr = frame[7];
    s_record.sp = (uint32_t)(uintptr_t)frame;
    s_record.cfsr = *(volatile uint32_t *)0xE000ED28u;
    s_record.hfsr = *(volatile uint32_t *)0xE000ED2Cu;
    s_record.mmfar = *(volatile uint32_t *)0xE000ED34u;
    s_record.bfar = *(volatile uint32_t *)0xE000ED38u;
    uint32_t i = 0;
    for (; message[i] != '\0' && i < sizeof(s_record.message) - 1u; i++) {
        s_record.message[i] = message[i];
    }
    s_record.message[i] = '\0';
    find_callers(frame + 8, 32u);
    s_magic = FAULT_MAGIC;
    watchdog_reboot(0, 0, 0);
    for (;;) {
        __wfi();
    }
}

void __used __not_in_flash_func(tiles_fault_record)(const uint32_t *frame) {
    record_frame(frame, "");
}

void __used __not_in_flash_func(tiles_fault_bell_record)(const uint32_t *frame) {
    record_frame(frame, "core 1 stuck");
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

static void __attribute__((naked)) __not_in_flash_func(bell_isr)(void) {
    __asm volatile(
        "movs r0, #4\n"
        "mov r1, lr\n"
        "tst r0, r1\n"
        "beq 1f\n"
        "mrs r0, psp\n"
        "b tiles_fault_bell_record\n"
        "1:\n"
        "mrs r0, msp\n"
        "b tiles_fault_bell_record\n");
}

volatile uint32_t g_tiles_core1_beat;
static volatile bool s_core1_armed;
static int s_bell = -1;
static uint32_t s_seen_beat, s_seen_us, s_last_call_us, s_rang_us;
static bool s_rang;

void tiles_fault_core1_arm(void) {
    s_bell = multicore_doorbell_claim_unused(1u << 1, false);
    if (s_bell < 0) {
        return;
    }
    multicore_doorbell_clear_current_core((uint)s_bell);
    irq_set_exclusive_handler(SIO_IRQ_BELL, bell_isr);
    irq_set_priority(SIO_IRQ_BELL, 0u); /* above USB: catches a stuck handler too */
    irq_set_enabled(SIO_IRQ_BELL, true);
    s_core1_armed = true;
}

void tiles_fault_watch_core1(void) {
    if (!s_core1_armed) {
        return;
    }
    uint32_t now = time_us_32();
    uint32_t beat = g_tiles_core1_beat;
    if (beat != s_seen_beat || now - s_last_call_us > WATCH_GAP_US) {
        s_seen_beat = beat;
        s_seen_us = now;
        s_rang = false;
    }
    s_last_call_us = now;
    if (!s_rang) {
        if (now - s_seen_us >= CORE1_STILL_US) {
            multicore_doorbell_set_other_core((uint)s_bell);
            s_rang = true;
            s_rang_us = now;
        }
        return;
    }
    if (now - s_rang_us >= BELL_WAIT_US) {
        memset(&s_record, 0, sizeof(s_record));
        s_record.core = 1u;
        strncpy(s_record.message, "core 1 stuck, interrupts off", sizeof(s_record.message) - 1u);
        s_magic = FAULT_MAGIC;
        watchdog_reboot(0, 0, 0);
        for (;;) {
            __wfi();
        }
    }
}
