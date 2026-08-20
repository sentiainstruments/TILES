# drivers/

Small, independently testable drivers for each chip on the board. Each
driver knows its own register map and protocol; it does not know about
logical pads, muxes' *meaning*, or musical semantics — that translation
lives in `board/` (which mux/channel a pad uses) and `services/` (what to
do with the data).

Planned drivers: `tca9548a` (Hall I2C mux), `tmag5273` (3-axis Hall
sensor), `mpr121` (capacitive touch), `pca9685` (PWM: motors + button
LEDs), `tca9554` (LED mux selector/enable), `sk6805` (addressable RGB
bit-banged over PIO), `dac80502` (CV DAC).
