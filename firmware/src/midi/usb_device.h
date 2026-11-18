#pragma once

/* Composite USB bring-up: calls tusb_init() and turns off pico_stdio_usb's
 * background task, so tud_task() must run every pass: on core 0
 * (tiles_usb_task() in the main loop) in controller mode, or on core 1 in
 * standalone mode (audio/usb_audio.c), which starts it there too (the RP2350
 * USB driver wants its interrupt and tud_task() on one core). */

#include <stdbool.h>

/* Starts TinyUSB with our descriptors; `audio` adds the USB audio input.
 * Call before stdio_init_all(), which asserts TinyUSB is already running
 * when tinyusb_device is linked. In standalone mode core 1 calls it. */
void tiles_usb_device_init(bool audio);

/* Whether the descriptor has the USB audio input. */
bool tiles_usb_device_has_audio(void);

/* Core 0's tud_task(): runs it in controller mode; in standalone mode,
 * where core 1 owns it, carries out the reboots core 1 was asked for over
 * USB. Every core 0 caller goes through this. */
void tiles_usb_task(void);
void tiles_usb_task_on_core1(void);

/* Standalone mode, after stdio_init_all(): the console without pico_stdio_usb,
 * whose printf() runs tud_task() on core 0. Writes what fits, never
 * waits. */
void tiles_usb_console_without_task(void);
