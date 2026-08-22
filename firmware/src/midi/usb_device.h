#pragma once

/*
 * Composite USB device (CDC diagnostics console + MIDI) bring-up.
 *
 * Because firmware/src/CMakeLists.txt links tinyusb_device explicitly
 * (not just the transitive copy pico_stdio_usb pulls in), pico-sdk's
 * pico_stdio_usb defers both calling tusb_init() and providing USB
 * descriptors to the application -- see midi/tusb_config.h and
 * midi/usb_descriptors.c. This module owns the "call tusb_init()"
 * half of that deferral.
 */

/* Initializes the TinyUSB device stack using this module's composite
 * CDC+MIDI descriptors (midi/usb_descriptors.c). Must run before
 * stdio_init_all() -- pico_stdio_usb's stdio_usb_init() (invoked from
 * within stdio_init_all()) asserts TinyUSB is already initialized
 * rather than initializing it itself, once tinyusb_device is linked. */
void tiles_usb_device_init(void);
