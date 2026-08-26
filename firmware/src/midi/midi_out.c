#include "midi_out.h"

#include "tusb.h"

#define TILES_MIDI_V1_CHANNEL 0u /* status-byte channel nibble; 0 = MIDI channel 1 */
#define TILES_MIDI_CABLE_NUM 0u

void tiles_midi_note_on(uint8_t note, uint8_t velocity) {
    if (!tud_midi_mounted()) {
        return;
    }
    uint8_t msg[3] = {(uint8_t)(0x90u | TILES_MIDI_V1_CHANNEL), note, velocity};
    tud_midi_stream_write(TILES_MIDI_CABLE_NUM, msg, sizeof(msg));
}

void tiles_midi_note_off(uint8_t note) {
    if (!tud_midi_mounted()) {
        return;
    }
    uint8_t msg[3] = {(uint8_t)(0x80u | TILES_MIDI_V1_CHANNEL), note, 0u};
    tud_midi_stream_write(TILES_MIDI_CABLE_NUM, msg, sizeof(msg));
}

void tiles_midi_send_poly_aftertouch(uint8_t note, uint8_t pressure) {
    if (!tud_midi_mounted()) {
        return;
    }
    uint8_t msg[3] = {(uint8_t)(0xA0u | TILES_MIDI_V1_CHANNEL), note, pressure};
    tud_midi_stream_write(TILES_MIDI_CABLE_NUM, msg, sizeof(msg));
}

void tiles_midi_send_cc(uint8_t controller, uint8_t value) {
    if (!tud_midi_mounted()) {
        return;
    }
    uint8_t msg[3] = {(uint8_t)(0xB0u | TILES_MIDI_V1_CHANNEL), controller, value};
    tud_midi_stream_write(TILES_MIDI_CABLE_NUM, msg, sizeof(msg));
}

void tiles_midi_send_pitch_bend(uint16_t bend_14bit) {
    if (!tud_midi_mounted()) {
        return;
    }
    uint8_t lsb = (uint8_t)(bend_14bit & 0x7Fu);
    uint8_t msb = (uint8_t)((bend_14bit >> 7) & 0x7Fu);
    uint8_t msg[3] = {(uint8_t)(0xE0u | TILES_MIDI_V1_CHANNEL), lsb, msb};
    tud_midi_stream_write(TILES_MIDI_CABLE_NUM, msg, sizeof(msg));
}
