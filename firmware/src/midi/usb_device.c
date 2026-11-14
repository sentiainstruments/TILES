#include "usb_device.h"

#include "pico/stdio.h"
#include "pico/stdio/driver.h"
#include "pico/stdio_usb.h"
#include "tusb.h"

static bool s_audio;
static volatile bool s_task_on_core1;

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
