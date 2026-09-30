#pragma once

/* Composite USB device bring-up (CDC console, MIDI, vendor, reset).
 *
 * Because src/CMakeLists.txt links tinyusb_device explicitly,
 * pico_stdio_usb leaves tusb_init() and the descriptors to the application
 * (midi/tusb_config.h, midi/usb_descriptors.c); this module calls
 * tusb_init(). It also turns off pico_stdio_usb's background tud_task(),
 * so main.c must call tud_task() every loop pass (without it the stack is
 * never serviced; the symptom was a silent CDC console). */

/* Starts TinyUSB with our descriptors. Call before stdio_init_all(), which
 * asserts TinyUSB is already running when tinyusb_device is linked. */
void tiles_usb_device_init(void);
