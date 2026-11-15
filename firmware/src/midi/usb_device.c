#include "usb_device.h"

#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/stdio.h"
#include "pico/stdio/driver.h"
#include "pico/stdio_usb.h"
#include "tusb.h"

static bool s_audio;
static volatile bool s_task_on_core1;

/* picotool's reset requests run in tud_task(): on core 1 in standalone
 * mode, where a watchdog reboot never fires because core 0 keeps feeding
 * it. Core 1 hands them to core 0 (linked with --wrap, CMakeLists.txt). */
static volatile bool s_bootsel_requested, s_reboot_requested;
static int s_bootsel_gpio;
static uint32_t s_bootsel_mask, s_reboot_delay_ms;
static bool s_bootsel_low;

void __attribute__((noreturn)) __real_rom_reset_usb_boot_extra(int gpio, uint32_t disable_interface_mask,
                                                               bool active_low);
void __real_watchdog_reboot(uint32_t pc, uint32_t sp, uint32_t delay_ms);

void __attribute__((noreturn)) __wrap_rom_reset_usb_boot_extra(int gpio, uint32_t disable_interface_mask,
                                                               bool active_low) {
    if (get_core_num() == 0u) {
        __real_rom_reset_usb_boot_extra(gpio, disable_interface_mask, active_low);
    }
    s_bootsel_gpio = gpio;
    s_bootsel_mask = disable_interface_mask;
    s_bootsel_low = active_low;
    __dmb();
    s_bootsel_requested = true;
    for (;;) {
        tight_loop_contents();
    }
}

/* A delay of 0 fires before any feed: a fault on core 1 restarts as is. */
void __wrap_watchdog_reboot(uint32_t pc, uint32_t sp, uint32_t delay_ms) {
    if (get_core_num() == 0u || delay_ms == 0u || pc != 0u) {
        __real_watchdog_reboot(pc, sp, delay_ms);
        return;
    }
    s_reboot_delay_ms = delay_ms;
    __dmb();
    s_reboot_requested = true;
}

static void take_reboot_requests(void) {
    if (s_bootsel_requested) {
        __real_rom_reset_usb_boot_extra(s_bootsel_gpio, s_bootsel_mask, s_bootsel_low);
    }
    if (s_reboot_requested) {
        __real_watchdog_reboot(0u, 0u, s_reboot_delay_ms);
        for (;;) {
            tight_loop_contents(); /* not fed */
        }
    }
}

void tiles_usb_device_init(bool audio) {
    s_audio = audio;
    tusb_init();
}

bool tiles_usb_device_has_audio(void) {
    return s_audio;
}

void tiles_usb_task(void) {
    if (!s_task_on_core1) {
        tud_task();
    } else {
        take_reboot_requests();
    }
}

void tiles_usb_task_on_core1(void) {
    s_task_on_core1 = true;
}

static void console_out(const char *buf, int len) {
    if (!tud_cdc_connected()) {
        return;
    }
    uint32_t room = tud_cdc_write_available();
    (void)tud_cdc_write(buf, (uint32_t)len < room ? (uint32_t)len : room);
    (void)tud_cdc_write_flush();
}

static void console_flush(void) {
    (void)tud_cdc_write_flush();
}

static int console_in(char *buf, int len) {
    if (!tud_cdc_connected() || !tud_cdc_available()) {
        return PICO_ERROR_NO_DATA;
    }
    int n = (int)tud_cdc_read(buf, (uint32_t)len);
    return n > 0 ? n : PICO_ERROR_NO_DATA;
}

static stdio_driver_t s_console = {
    .out_chars = console_out,
    .out_flush = console_flush,
    .in_chars = console_in,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
    .crlf_enabled = PICO_STDIO_DEFAULT_CRLF,
#endif
};

void tiles_usb_console_without_task(void) {
    stdio_set_driver_enabled(&stdio_usb, false);
    stdio_set_driver_enabled(&s_console, true);
}
