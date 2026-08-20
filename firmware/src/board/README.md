# board/

Owns every raw GPIO, I2C/SPI bus handle, and PIO resource, plus the
canonical `PadConfig[24]` struct array generated/derived from
`../../../shared/board-map/`. Every other module addresses hardware only
through logical pad IDs (1–24) or named board functions this module
exposes — never a raw pin number or mux channel.

Planned contents: `board_pins.h` (GPIO map as constants), `board_init.c`
(safe boot sequence), `pad_config.h`/`.c` (the 24-pad table + accessors).
