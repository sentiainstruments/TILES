#include "midi_out.h"

#include "tusb.h"

#define TILES_MIDI_V1_VELOCITY 100u
#define TILES_MIDI_V1_CHANNEL 0u /* status-byte channel nibble; 0 = MIDI channel 1 */
#define TILES_MIDI_CABLE_NUM 0u

void tiles_midi_note_on(uint8_t note) {
    if (!tud_midi_mounted()) {
        return;
    }
    uint8_t msg[3] = {(uint8_t)(0x90u | TILES_MIDI_V1_CHANNEL), note, TILES_MIDI_V1_VELOCITY};
    tud_midi_stream_write(TILES_MIDI_CABLE_NUM, msg, sizeof(msg));
}

void tiles_midi_note_off(uint8_t note) {
    if (!tud_midi_mounted()) {
        return;
    }
    uint8_t msg[3] = {(uint8_t)(0x80u | TILES_MIDI_V1_CHANNEL), note, 0u};
    tud_midi_stream_write(TILES_MIDI_CABLE_NUM, msg, sizeof(msg));
}

void tiles_midi_send_cc(uint8_t controller, uint8_t value) {
    if (!tud_midi_mounted()) {
        return;
    }
    uint8_t msg[3] = {(uint8_t)(0xB0u | TILES_MIDI_V1_CHANNEL), controller, value};
    tud_midi_stream_write(TILES_MIDI_CABLE_NUM, msg, sizeof(msg));
}
