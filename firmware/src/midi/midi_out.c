#include "midi_out.h"

#include "tusb.h"

#define TILES_MIDI_CABLE_NUM 0u

static void send2(uint8_t status, uint8_t data1) {
    if (!tud_midi_mounted()) {
        return;
    }
    uint8_t msg[2] = {status, data1};
    tud_midi_stream_write(TILES_MIDI_CABLE_NUM, msg, sizeof(msg));
}

static void send3(uint8_t status, uint8_t data1, uint8_t data2) {
    if (!tud_midi_mounted()) {
        return;
    }
    uint8_t msg[3] = {status, data1, data2};
    tud_midi_stream_write(TILES_MIDI_CABLE_NUM, msg, sizeof(msg));
}

void tiles_midi_note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    send3((uint8_t)(0x90u | channel), note, velocity);
}

void tiles_midi_note_off(uint8_t channel, uint8_t note) {
    send3((uint8_t)(0x80u | channel), note, 0u);
}

void tiles_midi_send_channel_pressure(uint8_t channel, uint8_t pressure) {
    send2((uint8_t)(0xD0u | channel), pressure);
}

void tiles_midi_send_cc(uint8_t channel, uint8_t controller, uint8_t value) {
    send3((uint8_t)(0xB0u | channel), controller, value);
}

void tiles_midi_send_cc_broadcast(uint8_t controller, uint8_t value) {
    tiles_midi_send_cc(TILES_MIDI_MPE_MASTER_CHANNEL, controller, value);
    for (uint8_t i = 0; i < TILES_MIDI_MPE_NUM_MEMBER_CHANNELS; i++) {
        tiles_midi_send_cc((uint8_t)(TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL + i), controller, value);
    }
}

void tiles_midi_send_pitch_bend(uint8_t channel, uint16_t bend_14bit) {
    uint8_t lsb = (uint8_t)(bend_14bit & 0x7Fu);
    uint8_t msb = (uint8_t)((bend_14bit >> 7) & 0x7Fu);
    send3((uint8_t)(0xE0u | channel), lsb, msb);
}

/* RPN (Registered Parameter Number) messages are a 4-message CC
 * sequence -- select the parameter (CC101/100 = MSB/LSB), write its
 * value (CC6/38 = MSB/LSB) -- followed by a "null" RPN select (101/100
 * = 127/127) so this channel's Data Entry controllers don't stay
 * pointed at a live parameter, where a stray CC6/38 from anything else
 * later would silently rewrite it. Every MPE zone-setup RPN below
 * follows this same shape. */
static void send_rpn(uint8_t channel, uint8_t param_msb, uint8_t param_lsb, uint8_t value_msb, uint8_t value_lsb) {
    tiles_midi_send_cc(channel, 101u, param_msb);
    tiles_midi_send_cc(channel, 100u, param_lsb);
    tiles_midi_send_cc(channel, 6u, value_msb);
    tiles_midi_send_cc(channel, 38u, value_lsb);
    tiles_midi_send_cc(channel, 101u, 127u);
    tiles_midi_send_cc(channel, 100u, 127u);
}

void tiles_midi_mpe_init(void) {
    /* MPE Configuration Message: RPN 6 (param MSB=0x00, LSB=0x06), value
     * MSB = number of Member Channels, LSB unused (0). Sent on the Zone
     * Master Channel -- this is the message an MPE-aware receiver uses
     * to recognize this as an MPE Lower Zone at all. */
    send_rpn(TILES_MIDI_MPE_MASTER_CHANNEL, 0x00u, 0x06u, (uint8_t)TILES_MIDI_MPE_NUM_MEMBER_CHANNELS, 0x00u);

    /* Pitch Bend Sensitivity: RPN 0 (param MSB=0x00, LSB=0x00), value
     * MSB = semitones, LSB = cents (0 here -- whole-semitone range).
     * Sent on the Zone Master Channel, applying zone-wide per the MPE
     * specification's own convention. */
    send_rpn(TILES_MIDI_MPE_MASTER_CHANNEL, 0x00u, 0x00u, (uint8_t)TILES_MIDI_MPE_PITCH_BEND_RANGE_SEMITONES, 0x00u);
}
