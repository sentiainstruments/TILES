#include "midi_out.h"

#include "din_midi.h"

#include "tusb.h"

#include "pico/time.h"

#include <stdio.h>

#define TILES_MIDI_CABLE_NUM 0u

/* Found investigating real feedback ("something is clashing and
 * causing those crashes only on play" -- Ableton actively playing,
 * sending clock, presumably meaning this device's own MIDI OUTPUT is
 * also more continuous than it is at idle): tud_midi_stream_write()
 * (confirmed reading TinyUSB's own midi_device.c) writes only as many
 * bytes as CURRENTLY fit in its 64-byte TX FIFO and silently returns
 * early otherwise -- every call site here ignored that return value,
 * so under any real backpressure (the host briefly not draining fast
 * enough, or several messages queued back-to-back in one scan) a
 * message could go out MISSING ITS TAIL BYTES -- a Note-On with no
 * velocity, silently corrupting the stream a receiver has to parse,
 * with nothing here ever aware it happened.
 *
 * Real feedback, later, root-caused to exactly this: "pedal only
 * sticks when you release the note but hold pedal and then release
 * it." tiles_midi_send_cc_broadcast() (services/pedal.c's own sustain-
 * off send) fires 17 back-to-back 3-byte CC messages (51 bytes) in one
 * call -- comfortably enough on its own to overflow the 64-byte TX
 * FIFO if a note-off from releasing a pad moments earlier is still
 * sitting in it, exactly the gesture that reproduces the stick:
 * whichever Member Channel's CC64=0 landed on the truncated tail of
 * that burst never reached the synth, so that one note stayed
 * sustained even after the pedal genuinely released, while every other
 * channel that fit released fine.
 *
 * This used to deliberately NOT retry or pump tud_task() to make room,
 * out of a real concern: turning a dropped message into a NEW blocking
 * wait if the host genuinely isn't draining would reopen the exact
 * failure class this whole session's other fixes (I2C, CDC) were about
 * closing. send_with_retry() below still doesn't retry unconditionally
 * -- it's bounded by a real wall-clock deadline
 * (MIDI_SEND_RETRY_TIMEOUT_MS), long enough to ride out an ordinary
 * multi-message burst like the broadcast above, short enough that a
 * genuinely absent/stalled host still returns promptly instead of
 * hanging the main loop. Pumping tud_task() inside the wait is required,
 * not optional -- it's the only thing that actually drains the TX FIFO
 * to the host at all (see main.c's own loop, which normally does this
 * once per iteration); without calling it again here, waiting alone
 * would never free any room. warn_if_truncated() below still logs
 * anything that couldn't be recovered even after retrying, so a
 * genuinely stalled host remains visible rather than silently eaten. */
#define MIDI_SEND_RETRY_TIMEOUT_MS 5u

static uint32_t send_with_retry(const uint8_t *msg, uint32_t len) {
    uint32_t sent = tud_midi_stream_write(TILES_MIDI_CABLE_NUM, msg, len);
    if (sent >= len) {
        return sent;
    }
    uint32_t deadline_ms = to_ms_since_boot(get_absolute_time()) + MIDI_SEND_RETRY_TIMEOUT_MS;
    while (sent < len && to_ms_since_boot(get_absolute_time()) < deadline_ms) {
        tud_task();
        sent += tud_midi_stream_write(TILES_MIDI_CABLE_NUM, msg + sent, len - sent);
    }
    return sent;
}

static void warn_if_truncated(const char *what, uint32_t sent, uint32_t expected) {
    if (sent != expected) {
        printf("[midi_out] %s truncated: wrote %u/%u bytes (host still not draining after %ums retry)\n", what,
               (unsigned)sent, (unsigned)expected, (unsigned)MIDI_SEND_RETRY_TIMEOUT_MS);
    }
}

/* Every message the controller performs -- notes, expression, CCs, transport
 * Start/Stop -- goes out on TWO independent sinks: USB (only while a host has
 * the device mounted) and the DIN jack (whenever DIN initialized, host or
 * not -- real feedback: "yes build DIN MIDI"; the hardware handoff's
 * external-power-only mode has no USB host at all, which is the jack's whole
 * point). The DIN send comes FIRST and never waits: it only queues bytes
 * (midi/din_midi_queue.c), while the USB write below can spend up to
 * MIDI_SEND_RETRY_TIMEOUT_MS pumping tud_task() for room. Messages meant for
 * the DAW's remote script rather than for an instrument -- see
 * tiles_midi_send_daw_cc() and tiles_midi_send_sysex() -- use the USB-only
 * path and never touch DIN. */
static void usb_write(const char *what, const uint8_t *msg, uint32_t len) {
    if (!tud_midi_mounted()) {
        return;
    }
    warn_if_truncated(what, send_with_retry(msg, len), len);
}

static void send1(uint8_t status) {
    uint8_t msg[1] = {status};
    tiles_din_midi_send(msg, sizeof(msg));
    usb_write("send1", msg, sizeof(msg));
}

static void send2(uint8_t status, uint8_t data1) {
    uint8_t msg[2] = {status, data1};
    tiles_din_midi_send(msg, sizeof(msg));
    usb_write("send2", msg, sizeof(msg));
}

static void send3(uint8_t status, uint8_t data1, uint8_t data2) {
    uint8_t msg[3] = {status, data1, data2};
    tiles_din_midi_send(msg, sizeof(msg));
    usb_write("send3", msg, sizeof(msg));
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

void tiles_midi_send_daw_cc(uint8_t channel, uint8_t controller, uint8_t value) {
    uint8_t msg[3] = {(uint8_t)(0xB0u | channel), controller, value};
    usb_write("send_daw_cc", msg, sizeof(msg));
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
     * Sent on the Zone Master Channel (applying zone-wide per the MPE
     * specification's own convention) AND redundantly on every Member
     * Channel individually -- see this function's own declaration in
     * midi_out.h for why: real feedback found the Master-Channel-only
     * send wasn't taking effect on whatever was actually receiving
     * pitch bend, which only ever reads it from the Member Channel a
     * note is actually on. */
    send_rpn(TILES_MIDI_MPE_MASTER_CHANNEL, 0x00u, 0x00u, (uint8_t)TILES_MIDI_MPE_PITCH_BEND_RANGE_SEMITONES, 0x00u);
    for (uint8_t i = 0; i < TILES_MIDI_MPE_NUM_MEMBER_CHANNELS; i++) {
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

/* 32 bytes is generous headroom over this codebase's own actual SysEx
 * message sizes (see midi/midi_in.h's own MIDI_IN_SYSEX_MAX for the
 * receive side's matching ceiling) -- callers passing more than fits
 * (len clamped below) would indicate a genuine bug in whatever's
 * building the message, not a real, larger protocol message this
 * function needs to support. */
#define SYSEX_SEND_BUF_MAX 32u

void tiles_midi_send_sysex(const uint8_t *data, uint32_t len) {
    /* USB only, like tiles_midi_send_daw_cc(): this is the Ableton remote
     * script's private protocol, not something to spray at whatever
     * hardware is on the DIN jack. */
    if (!tud_midi_mounted()) {
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
    uint32_t total = len + 2u;
    warn_if_truncated("send_sysex", send_with_retry(msg, total), total);
}
