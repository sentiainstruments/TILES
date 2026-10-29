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

cc -std=c11 -Wall -Wextra -Itest/stubs -Isrc/midi test/test_midi_in.c src/midi/midi_in.c src/midi/usb_midi_packet.c -o /tmp/sentia_tiles_test_midi_in
/tmp/sentia_tiles_test_midi_in

# USB-MIDI packets: the per-cable framing both USB ports ride on.
cc -std=gnu11 -Wall -Wextra -Isrc/midi test/test_usb_midi_packet.c src/midi/usb_midi_packet.c -o /tmp/sentia_tiles_test_usb_midi_packet
/tmp/sentia_tiles_test_usb_midi_packet

cc -std=c11 -Wall -Wextra -Wpedantic -Isrc/services test/test_midi_channels.c src/services/midi_channels.c -o /tmp/sentia_tiles_test_midi_channels
/tmp/sentia_tiles_test_midi_channels

cc -std=c11 -Wall -Wextra -Wpedantic -Isrc/services test/test_mpe_alloc.c src/services/mpe_alloc.c -o /tmp/sentia_tiles_test_mpe_alloc
/tmp/sentia_tiles_test_mpe_alloc

# Drum sequencer core: layout, Drum Rack notes, steps, repeats, chance, quantized start.
cc -std=c11 -Wall -Wextra -Wpedantic -Isrc/services test/test_drum_pattern.c src/services/drum_pattern.c -o /tmp/sentia_tiles_test_drum_pattern
/tmp/sentia_tiles_test_drum_pattern

# Harmony mode's chord colours, key fitting and glide voicing.
cc -std=c11 -Wall -Wextra -Wpedantic -Isrc/services test/test_harmony_core.c src/services/harmony_core.c -o /tmp/sentia_tiles_test_harmony_core
/tmp/sentia_tiles_test_harmony_core

# Bouncing notes' balls: drop, landings, polyrhythm.
cc -std=c11 -Wall -Wextra -Wpedantic -Isrc/services test/test_bounce_core.c src/services/bounce_core.c -lm -o /tmp/sentia_tiles_test_bounce_core
/tmp/sentia_tiles_test_bounce_core

# The physics modes: Ripples, Sand, Orbits, Newton's cradle.
cc -std=c11 -Wall -Wextra -Wpedantic -Isrc/services test/test_physics_core.c src/services/physics_core.c -lm -o /tmp/sentia_tiles_test_physics_core
/tmp/sentia_tiles_test_physics_core

# Strike settle: when a struck pad's note can go before the follow-through ends.
cc -std=c11 -Wall -Wextra -Wpedantic -Isrc/services test/test_strike_settle.c src/services/strike_settle.c -o /tmp/sentia_tiles_test_strike_settle
/tmp/sentia_tiles_test_strike_settle

# Sequencer autosave blob: round trip, empty patterns, truncation, damage.
cc -std=c11 -Wall -Wextra -Wpedantic -Isrc/services test/test_seq_pattern.c src/services/seq_pattern.c -o /tmp/sentia_tiles_test_seq_pattern
/tmp/sentia_tiles_test_seq_pattern

# Rest guard: a pad's rest position stays true (drift, re-zero, unconfirmed).
cc -std=c11 -Wall -Wextra -Wpedantic -Isrc/services test/test_rest_guard.c src/services/rest_guard.c -o /tmp/sentia_tiles_test_rest_guard
/tmp/sentia_tiles_test_rest_guard

# Light effects: each lights where it should and fades; Off does nothing.
cc -std=c11 -Wall -Wextra -Wpedantic -Isrc/services test/test_light_effects.c src/services/light_effects.c -lm -o /tmp/sentia_tiles_test_light_effects
/tmp/sentia_tiles_test_light_effects

# MIDI clock: an external Start begins on its first Clock, on a beat boundary.
cc -std=c11 -Wall -Wextra -Wpedantic -Itest/stubs -Isrc/services -Isrc/midi test/test_midi_clock.c src/services/midi_clock.c -lm -o /tmp/sentia_tiles_test_midi_clock
/tmp/sentia_tiles_test_midi_clock | grep -v '^\[midi_clock\]' # its own Start/Stop logging

# Flash store (power-cut simulation) and the settings table on top of it.
cc -std=c11 -Wall -Wextra -Wpedantic -Isrc/storage test/test_kv_store.c src/storage/kv_store.c -o /tmp/sentia_tiles_test_kv_store
/tmp/sentia_tiles_test_kv_store

# The append-only log under drum mode's pattern (saves without erasing while playing).
cc -std=c11 -Wall -Wextra -Wpedantic -Isrc/storage test/test_log_store.c src/storage/log_store.c src/storage/kv_store.c -o /tmp/sentia_tiles_test_log_store
/tmp/sentia_tiles_test_log_store

# Mode, scale, key and octave across a power cycle (on the log above, into the real note map).
cc -std=c11 -Wall -Wextra -Wpedantic -Itest/stubs -Isrc/storage -Isrc/services -Isrc/board test/test_play_state.c src/services/play_state.c src/services/note_map.c src/board/pad_config.c src/storage/log_store.c src/storage/kv_store.c -o /tmp/sentia_tiles_test_play_state
/tmp/sentia_tiles_test_play_state

cc -std=c11 -Wall -Wextra -Wpedantic -Isrc/profiles -Isrc/storage test/test_settings.c src/profiles/settings.c src/profiles/settings_persist.c src/storage/kv_store.c -lm -o /tmp/sentia_tiles_test_settings
/tmp/sentia_tiles_test_settings

# Content store (custom scales) on its own simulated flash region, into the real note map.
cc -std=c11 -Wall -Wextra -Wpedantic -Itest/stubs -Isrc/storage -Isrc/services -Isrc/board -Isrc/profiles test/test_content.c src/profiles/content.c src/storage/kv_store.c src/services/note_map.c src/board/pad_config.c -o /tmp/sentia_tiles_test_content
/tmp/sentia_tiles_test_content

cc -std=c11 -Wall -Wextra -Wpedantic -Isrc/midi test/test_identity.c -o /tmp/sentia_tiles_test_identity
/tmp/sentia_tiles_test_identity

# The assembled DIN OUT PIO program, decoded as 8N1 @ 31250 baud. Needs a firmware build first
# (pioasm produces the header it reads); skipped with a note if there isn't one yet.
if [ -n "$(find build -name din_midi_tx.pio.h 2>/dev/null | head -1)" ]; then
    python3 test/pio_sim_din_tx.py
else
    echo "(skipping pio_sim_din_tx.py: no firmware build yet -- see BUILD.md)"
fi
