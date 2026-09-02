#pragma once

/*
 * Per-unit identifier for pre-production hardware -- real feedback:
 * "were moving to have identifiers." Each physical board's own firmware
 * image gets its own TILES_UNIT_NUMBER/TILES_UNIT_COUNT baked in here
 * before that specific board's build+flash: edit these two lines, run a
 * normal incremental build (no reconfigure needed, this is a plain
 * header), flash just that board, then move to the next one.
 *
 * No persistent per-unit storage exists yet (see storage/README.md --
 * not built), and the RP2350's own unique silicon ID
 * (pico_get_unique_board_id(), already exposed as the USB serial number
 * descriptor in midi/usb_descriptors.c) identifies a CHIP, not a
 * human-assigned production sequence number -- it's not something you
 * could read off a box or a spreadsheet. This is deliberately simple:
 * a human-assigned "2 of 4" the pre-production run can track, not a
 * cryptographic or collision-proof ID.
 *
 * Surfaced two places so it's identifiable without needing a serial
 * terminal open: the USB product string (see usb_descriptors.c's own
 * STRID_PRODUCT handling), visible via `picotool info -a` or the host
 * OS's own USB device listing, and a one-line printf at boot (main.c)
 * for correlating a captured serial log back to a specific physical
 * board.
 */
#define TILES_UNIT_NUMBER 2u
#define TILES_UNIT_COUNT 4u
