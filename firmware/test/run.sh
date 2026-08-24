#!/usr/bin/env bash
# Builds and runs the host-buildable firmware tests. No Pico SDK needed.
set -euo pipefail
cd "$(dirname "$0")/.."

cc -std=c11 -Wall -Wextra -Isrc/board test/test_pad_config.c src/board/pad_config.c -o /tmp/sentia_tiles_test_pad_config
/tmp/sentia_tiles_test_pad_config

cc -std=c11 -Wall -Wextra -Isrc/board -Isrc/services test/test_note_map.c src/board/pad_config.c src/services/note_map.c -o /tmp/sentia_tiles_test_note_map
/tmp/sentia_tiles_test_note_map
