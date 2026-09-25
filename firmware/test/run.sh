#!/usr/bin/env bash
# Builds and runs the host-buildable firmware tests. No Pico SDK needed.
set -euo pipefail
cd "$(dirname "$0")/.."

cc -std=c11 -Wall -Wextra -Isrc/board test/test_pad_config.c src/board/pad_config.c -o /tmp/sentia_tiles_test_pad_config
/tmp/sentia_tiles_test_pad_config

cc -std=c11 -Wall -Wextra -Itest/stubs -Isrc/board -Isrc/services test/test_note_map.c src/board/pad_config.c src/services/note_map.c -o /tmp/sentia_tiles_test_note_map
/tmp/sentia_tiles_test_note_map

# DIN MIDI: hardware-free queue logic, and the real midi_in.c parser (with tusb/pico time stubbed).
cc -std=c11 -Wall -Wextra -Wpedantic -Isrc/midi test/test_din_midi_queue.c src/midi/din_midi_queue.c -o /tmp/sentia_tiles_test_din_midi_queue
/tmp/sentia_tiles_test_din_midi_queue

cc -std=c11 -Wall -Wextra -Itest/stubs -Isrc/midi test/test_midi_in.c src/midi/midi_in.c -o /tmp/sentia_tiles_test_midi_in
/tmp/sentia_tiles_test_midi_in

# Flash store (power-cut simulation) and the settings table on top of it.
cc -std=c11 -Wall -Wextra -Wpedantic -Isrc/storage test/test_kv_store.c src/storage/kv_store.c -o /tmp/sentia_tiles_test_kv_store
/tmp/sentia_tiles_test_kv_store

cc -std=c11 -Wall -Wextra -Wpedantic -Isrc/profiles -Isrc/storage test/test_settings.c src/profiles/settings.c src/profiles/settings_persist.c src/storage/kv_store.c -lm -o /tmp/sentia_tiles_test_settings
/tmp/sentia_tiles_test_settings

# The assembled DIN OUT PIO program, decoded as 8N1 @ 31250 baud. Needs a firmware build first
# (pioasm produces the header it reads); skipped with a note if there isn't one yet.
if [ -n "$(find build -name din_midi_tx.pio.h 2>/dev/null | head -1)" ]; then
    python3 test/pio_sim_din_tx.py
else
    echo "(skipping pio_sim_din_tx.py: no firmware build yet -- see BUILD.md)"
fi
