#include "midi_out.h"

#include "din_midi.h"
#include "usb_midi_packet.h"

#include "tusb.h"

#include "pico/time.h"

#include <stdio.h>

/* USB writes wait for FIFO room up to MIDI_SEND_RETRY_TIMEOUT_MS, pumping
 * tud_task() (the only thing that drains the FIFO to the host). Without
 * the retry, a burst (e.g. a 16-channel CC broadcast right after a
 * Note-Off) overflowed the 64-byte FIFO and dropped messages, which showed
 * up as stuck sustain. The bound keeps a stalled or absent host from
 * hanging the main loop; a message that still doesn't fit is logged.
 *
 * Whole USB-MIDI packets (midi/usb_midi_packet.h), not TinyUSB's byte
 * stream: a packet is queued complete or not at all, so a message can be
 * dropped but never split. The stream API also shares one partial-message
 * state across ports, which would mix the two cables. */
#define MIDI_SEND_RETRY_TIMEOUT_MS 5u

/* Headroom over the largest SysEx this firmware sends (receive side:
 * midi_in.h's MIDI_IN_SYSEX_MAX). Longer input is clamped; it would be a
 * bug in the caller. */
#define SYSEX_SEND_BUF_MAX 32u

static void usb_write(uint8_t cable, const char *what, const uint8_t *msg, uint32_t len) {
    if (!tud_midi_mounted()) {
        return;
    }
    uint8_t packets[TILES_USB_MIDI_MAX_PACKETS][4];
    size_t count = tiles_usb_midi_pack(cable, msg, len, packets, TILES_USB_MIDI_MAX_PACKETS);
    uint32_t deadline_ms = to_ms_since_boot(get_absolute_time()) + MIDI_SEND_RETRY_TIMEOUT_MS;
    for (size_t i = 0; i < count; i++) {
        while (!tud_midi_n_packet_write(0, packets[i])) {
            if (to_ms_since_boot(get_absolute_time()) >= deadline_ms) {
                printf("[midi_out] %s dropped at packet %u/%u (host still not draining after %ums retry)\n", what,
                       (unsigned)i, (unsigned)count, (unsigned)MIDI_SEND_RETRY_TIMEOUT_MS);
                return;
            }
            tud_task();
        }
    }
}

/* Everything goes to two sinks: USB MAIN (while mounted) and DIN (whenever
 * initialized, host or not; external-power-only setups have no USB host).
 * DIN goes FIRST because it only queues (midi/din_midi_queue.c), while the
 * USB write may wait up to MIDI_SEND_RETRY_TIMEOUT_MS.
 * tiles_midi_send_daw_cc() is the exception: DAW port only. */
static void send1(uint8_t status) {
    uint8_t msg[1] = {status};
    tiles_din_midi_send(msg, sizeof(msg));
    usb_write(TILES_USB_MIDI_CABLE_MAIN, "send1", msg, sizeof(msg));
}

static void send2(uint8_t status, uint8_t data1) {
    uint8_t msg[2] = {status, data1};
    tiles_din_midi_send(msg, sizeof(msg));
    usb_write(TILES_USB_MIDI_CABLE_MAIN, "send2", msg, sizeof(msg));
}

static void send3(uint8_t status, uint8_t data1, uint8_t data2) {
    uint8_t msg[3] = {status, data1, data2};
    tiles_din_midi_send(msg, sizeof(msg));
    usb_write(TILES_USB_MIDI_CABLE_MAIN, "send3", msg, sizeof(msg));
}

void tiles_midi_note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    send3((uint8_t)(0x90u | channel), note, velocity);
}

void tiles_midi_note_off(uint8_t channel, uint8_t note, uint8_t release_velocity) {
    send3((uint8_t)(0x80u | channel), note, release_velocity);
}

/* Last Pitch Bend / Channel Pressure sent per channel, so
 * tiles_midi_send_note_setup() skips what's already at its default (each
 * skipped message is ~1 ms sooner on DIN). UNKNOWN until first sent, and
 * after every zone declaration (receivers reset controllers on zone
 * changes). */
#define LAST_BEND_UNKNOWN 0xFFFFu
#define LAST_PRESSURE_UNKNOWN 0xFFu
#define PITCH_BEND_CENTER_14BIT 8192u
static uint16_t s_last_bend[16];
static uint8_t s_last_pressure[16];

static void forget_channel_state(void) {
    for (uint8_t i = 0; i < 16u; i++) {
        s_last_bend[i] = LAST_BEND_UNKNOWN;
        s_last_pressure[i] = LAST_PRESSURE_UNKNOWN;
    }
}

void tiles_midi_send_channel_pressure(uint8_t channel, uint8_t pressure) {
    s_last_pressure[channel & 0x0Fu] = pressure;
    send2((uint8_t)(0xD0u | channel), pressure);
}

void tiles_midi_send_note_setup(uint8_t channel) {
    if (s_last_bend[channel & 0x0Fu] != PITCH_BEND_CENTER_14BIT) {
        tiles_midi_send_pitch_bend(channel, PITCH_BEND_CENTER_14BIT);
    }
    if (s_last_pressure[channel & 0x0Fu] != 0u) {
        tiles_midi_send_channel_pressure(channel, 0u);
    }
}

void tiles_midi_send_cc(uint8_t channel, uint8_t controller, uint8_t value) {
    send3((uint8_t)(0xB0u | channel), controller, value);
}

void tiles_midi_send_daw_cc(uint8_t channel, uint8_t controller, uint8_t value) {
    uint8_t msg[3] = {(uint8_t)(0xB0u | channel), controller, value};
    usb_write(TILES_USB_MIDI_CABLE_DAW, "send_daw_cc", msg, sizeof(msg));
}

/* Channels 2-16: fixed by the MIDI spec, deliberately not the live zone
 * size from services/midi_channels.h (this file doesn't know the layout). */
#define MIDI_BROADCAST_CHANNEL_COUNT 15u

void tiles_midi_send_cc_broadcast(uint8_t controller, uint8_t value) {
    tiles_midi_send_cc(TILES_MIDI_MPE_MASTER_CHANNEL, controller, value);
    for (uint8_t i = 0; i < MIDI_BROADCAST_CHANNEL_COUNT; i++) {
        tiles_midi_send_cc((uint8_t)(TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL + i), controller, value);
    }
}

/* Standard hardware panic: CC 123 (All Notes Off) is the usual cure,
 * CC 120 (All Sound Off) the backup for receivers that don't fully honor
 * it. On every channel, since a stuck note can be on any part. Independent
 * of internal note tracking on purpose: a panic must work even when that
 * tracking is wrong (unlike tiles_expression_force_release_all()). */
void tiles_midi_send_panic(void) {
    /* Sustain off first: a receiver that still thinks the pedal is down can
     * keep notes through 123/120 (seen on real hardware: the plugin's own
     * panic worked, ours didn't until this was added). */
    tiles_midi_send_cc_broadcast(64u, 0u); /* Damper Pedal (Sustain) off */
    tiles_midi_send_cc_broadcast(123u, 0u); /* All Notes Off */
    tiles_midi_send_cc_broadcast(120u, 0u); /* All Sound Off */
}

void tiles_midi_send_pitch_bend(uint8_t channel, uint16_t bend_14bit) {
    s_last_bend[channel & 0x0Fu] = bend_14bit;
    uint8_t lsb = (uint8_t)(bend_14bit & 0x7Fu);
    uint8_t msb = (uint8_t)((bend_14bit >> 7) & 0x7Fu);
    send3((uint8_t)(0xE0u | channel), lsb, msb);
}

/* RPN: select the parameter (CC 101/100), write the value (CC 6/38), then
 * select the null RPN (127/127) so a stray CC 6/38 later can't rewrite it. */
static void send_rpn(uint8_t channel, uint8_t param_msb, uint8_t param_lsb, uint8_t value_msb, uint8_t value_lsb) {
    tiles_midi_send_cc(channel, 101u, param_msb);
    tiles_midi_send_cc(channel, 100u, param_lsb);
    tiles_midi_send_cc(channel, 6u, value_msb);
    tiles_midi_send_cc(channel, 38u, value_lsb);
    tiles_midi_send_cc(channel, 101u, 127u);
    tiles_midi_send_cc(channel, 100u, 127u);
}

void tiles_midi_mpe_init(uint8_t member_channel_count) {
    /* MPE Configuration Message: RPN 6, value MSB = Member Channel count (0
     * withdraws the zone). On the Master Channel; how a receiver recognizes an
     * MPE Lower Zone. */
    send_rpn(TILES_MIDI_MPE_MASTER_CHANNEL, 0x00u, 0x06u, member_channel_count, 0x00u);
    forget_channel_state();
    if (member_channel_count == 0u) {
        return; /* zone withdrawn: no members to configure */
    }

    /* Pitch Bend Sensitivity: RPN 0, MSB = semitones, LSB = cents (0). On the
     * Master Channel and again on each Member Channel, since some receivers
     * only read it from the channel the note is on. */
    /* Always right after RPN 6: JUCE receivers (e.g. Equator) reset the bend
     * range to 48 semitones on every RPN 6. */
    send_rpn(TILES_MIDI_MPE_MASTER_CHANNEL, 0x00u, 0x00u, (uint8_t)TILES_MIDI_MPE_PITCH_BEND_RANGE_SEMITONES, 0x00u);
    for (uint8_t i = 0; i < member_channel_count; i++) {
        send_rpn((uint8_t)(TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL + i), 0x00u, 0x00u,
                 (uint8_t)TILES_MIDI_MPE_PITCH_BEND_RANGE_SEMITONES, 0x00u);
    }
}

void tiles_midi_send_start(void) {
    send1(0xFAu);
}

void tiles_midi_send_stop(void) {
    send1(0xFCu);
}

void tiles_midi_send_sysex(tiles_midi_port_t port, const uint8_t *data, uint32_t len) {
    /* USB only: DIN OUT has no SysEx path (midi/din_midi_queue.h). */
    if (port == TILES_MIDI_PORT_DIN) {
        return;
    }
    if (len > SYSEX_SEND_BUF_MAX - 2u) {
        len = SYSEX_SEND_BUF_MAX - 2u;
    }
    uint8_t msg[SYSEX_SEND_BUF_MAX];
    msg[0] = 0xF0u;
    for (uint32_t i = 0; i < len; i++) {
        msg[1u + i] = data[i];
    }
    msg[1u + len] = 0xF7u;
    uint8_t cable = port == TILES_MIDI_PORT_DAW ? TILES_USB_MIDI_CABLE_DAW : TILES_USB_MIDI_CABLE_MAIN;
    usb_write(cable, "send_sysex", msg, len + 2u);
}
