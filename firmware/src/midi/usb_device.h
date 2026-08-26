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
 *
 * That same deferral also disables pico_stdio_usb's automatic
 * background-IRQ tud_task() servicing (pico-sdk's
 * PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK defaults to 0 whenever
 * LIB_TINYUSB_DEVICE is set, specifically so the application services
 * it instead) -- confirmed a real, previously-missing piece: nothing in
 * this firmware called tud_task() anywhere, which meant the USB stack
 * was never actually serviced past whatever the low-level enumeration
 * ISR handles on its own. main.c now calls tud_task() at the top of
 * every main-loop iteration; see its call site for the full story
 * (real symptom that led to finding this: the USB-CDC debug/calibration
 * console printed nothing at all on real hardware).
 */

/* Initializes the TinyUSB device stack using this module's composite
 * CDC+MIDI descriptors (midi/usb_descriptors.c). Must run before
 * stdio_init_all() -- pico_stdio_usb's stdio_usb_init() (invoked from
 * within stdio_init_all()) asserts TinyUSB is already initialized
 * rather than initializing it itself, once tinyusb_device is linked. */
void tiles_usb_device_init(void);
