#pragma once

/* Serial Hall calibration capture over the USB-CDC console, until the
 * companion app does it. Two values per pad (a normal press isn't
 * repeatable): hands off, send 'r' (recapture rest); strike hard, send 'm'
 * (print depths). Nothing is saved: the numbers are for picking constants. */

#include <stdint.h>

/* Prints the command summary once. Call after tiles_hall_init(). */
void tiles_calibration_init(void);

/* Non-blocking poll for one command character (zero-timeout getchar), so
 * it's cheap every main-loop pass. Call after tiles_hall_scan(). */
void tiles_calibration_scan(void);
