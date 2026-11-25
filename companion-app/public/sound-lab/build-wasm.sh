#!/bin/sh
set -eu
cd "$(dirname "$0")"
clang --target=wasm32 -O3 -ffreestanding -fno-builtin -nostdlib -Iwasm-include src/modal.c src/studio.c -Wl,--no-entry -Wl,--export-memory -Wl,--allow-undefined -Wl,--export=studio_init -Wl,--export=studio_params -Wl,--export=studio_expression -Wl,--export=studio_gain -Wl,--export=studio_note_on -Wl,--export=studio_note_off -Wl,--export=studio_panic -Wl,--export=studio_render -Wl,--export=studio_left -Wl,--export=studio_right -Wl,--export=modal_ratio -Wl,--export=modal_t60 -o resonance.wasm
