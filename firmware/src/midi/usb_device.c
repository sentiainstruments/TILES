#include "usb_device.h"

#include "tusb.h"

void tiles_usb_device_init(void) {
    tusb_init();
}
