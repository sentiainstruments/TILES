#pragma once

/* Composite USB bring-up: calls tusb_init() and turns off pico_stdio_usb's
 * background task, so main.c must call tud_task() every pass. */

/* Starts TinyUSB with our descriptors. Call before stdio_init_all(), which
 * asserts TinyUSB is already running when tinyusb_device is linked. */
void tiles_usb_device_init(void);
