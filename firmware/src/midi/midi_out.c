#include "midi_out.h"

#include "din_midi.h"
#include "usb_midi_packet.h"

#include "tusb.h"

#include "pico/time.h"

#include <stdio.h>

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
 * would never free any room. A message that still can't be written
 * after retrying is logged, so a genuinely stalled host remains visible
 * rather than silently eaten.
 *
 * Writes whole USB-MIDI packets (midi/usb_midi_packet.h), not TinyUSB's
 * byte stream: a packet is either queued complete or not at all, so a
 * stalled host can drop a message but never split one -- and with two
 * ports (midi/midi_ports.h), the stream API's single partial-message
 * state would finish a message cut short on one port on the other. */
#define MIDI_SEND_RETRY_TIMEOUT_MS 5u

/* 32 bytes is generous headroom over this codebase's own actual SysEx
 * message sizes (see midi/midi_in.h's own MIDI_IN_SYSEX_MAX for the
 * receive side's matching ceiling) -- callers passing more than fits
 * (len clamped in tiles_midi_send_sysex()) would indicate a genuine bug in
 * whatever's building the message, not a real, larger protocol message
 * this file needs to support. */
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

/* Every message the controller performs -- notes, expression, CCs, transport
 * Start/Stop -- goes out on TWO independent sinks: the USB MAIN port (only
 * while a host has the device mounted) and the DIN jack (whenever DIN
 * initialized, host or not -- real feedback: "yes build DIN MIDI"; the
 * hardware handoff's external-power-only mode has no USB host at all, which
 * is the jack's whole point). The DIN send comes FIRST and never waits: it
 * only queues bytes (midi/din_midi_queue.c), while the USB write below can
 * spend up to MIDI_SEND_RETRY_TIMEOUT_MS pumping tud_task() for room.
 * Messages meant for the DAW's remote script rather than for an instrument
 * -- tiles_midi_send_daw_cc() -- go to the DAW port only and never touch DIN
 * (midi/midi_ports.h). */
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

/* Last Pitch Bend / Channel Pressure value put on the wire per channel, so
 * tiles_midi_send_note_setup() only sends what isn't already at its
 * default -- on DIN every skipped message is ~1 ms less before the Note-On.
 * UNKNOWN until first sent, and again after every zone declaration (a
 * receiver resets a channel's controllers when it enters or leaves a zone). */
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

/* Every channel 2-16 could ever carry -- see this function's own header
 * comment in midi_out.h for why this is deliberately NOT services/midi_
 * channels.h's own (dynamic, often smaller) live zone size. 15, not a
 * services/midi_channels.h constant: this file stays unaware of that
 * module entirely (see midi_out.h's own header on the module boundary),
 * and this number is fixed by the 16-channel MIDI spec itself, not by
 * anything this board's layout could change. */
#define MIDI_BROADCAST_CHANNEL_COUNT 15u

void tiles_midi_send_cc_broadcast(uint8_t controller, uint8_t value) {
    tiles_midi_send_cc(TILES_MIDI_MPE_MASTER_CHANNEL, controller, value);
    for (uint8_t i = 0; i < MIDI_BROADCAST_CHANNEL_COUNT; i++) {
        tiles_midi_send_cc((uint8_t)(TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL + i), controller, value);
    }
}

/* Real feedback: "panic should be forced sleep with shift button. like
 * that action sends a panic note off" -- standard MIDI practice for a
 * hardware controller's own "stop everything" gesture: CC 123 (All Notes
 * Off) is what a DAW's own panic button sends and is the accepted cure for
 * a stuck note; CC 120 (All Sound Off) is the harsher backup for a
 * receiver that doesn't fully honor 123 (confirmed against real-world
 * practice: "use CC 123 first for standard panic control, and CC 120 as a
 * backup if you need guaranteed immediate silence"). Sent on every channel
 * -- reuses tiles_midi_send_cc_broadcast(), which already covers the
 * Master Channel plus 2-16 (chord/game/sequencer/Song's channels included,
 * not just the live MPE zone -- see that function's own header comment),
 * since a stuck note could genuinely be on any of them, not only a live
 * MPE one. Deliberately independent of this device's own internal note
 * bookkeeping: the whole point of a MIDI panic is to silence a receiver
 * even if the sender's own tracking of what's held is wrong -- unlike
 * tiles_expression_force_release_all() (services/expression.c), which
 * only sends note-offs for whatever this file's own state believes is
 * currently held. */
void tiles_midi_send_panic(void) {
    /* Real feedback found chasing a still-open "notes stick" report:
     * "panic hardware didnt clear it but panic built into the plugin did
     * stop notes." All Notes Off (123)/All Sound Off (120) below are
     * proven (via real hardware trace, see services/README.md's own
     * sustain-pedal entries) to leave this board correctly, on every
     * channel -- but a receiver that ties a note's release to seeing
     * CC64 (Damper Pedal/Sustain) go low, not just to a Channel Mode
     * message, can legitimately keep holding a note through 123/120
     * alone if it still believes the pedal is down. Standard MIDI panic
     * implementations commonly clear sustain for exactly this reason.
     * Sent FIRST, before the two below -- if a stuck note really is
     * being held open by a stale "pedal down" belief, that belief should
     * be cleared before asking the receiver to also drop the notes
     * themselves. */
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

void tiles_midi_mpe_init(uint8_t member_channel_count) {
    /* MPE Configuration Message: RPN 6 (param MSB=0x00, LSB=0x06), value
     * MSB = number of Member Channels, LSB unused (0). Sent on the Zone
     * Master Channel -- this is the message an MPE-aware receiver uses
     * to recognize this as an MPE Lower Zone at all (0 = none: withdraws
     * the zone -- see this function's own declaration in midi_out.h). */
    send_rpn(TILES_MIDI_MPE_MASTER_CHANNEL, 0x00u, 0x06u, member_channel_count, 0x00u);
    forget_channel_state();
    if (member_channel_count == 0u) {
        return; /* zone withdrawn -- no Member Channels to give a bend range */
    }

    /* Pitch Bend Sensitivity: RPN 0 (param MSB=0x00, LSB=0x00), value
     * MSB = semitones, LSB = cents (0 here -- whole-semitone range).
     * Sent on the Zone Master Channel (applying zone-wide per the MPE
     * specification's own convention) AND redundantly on every Member
     * Channel individually -- see this function's own declaration in
     * midi_out.h for why: real feedback found the Master-Channel-only
     * send wasn't taking effect on whatever was actually receiving
     * pitch bend, which only ever reads it from the Member Channel a
     * note is actually on. */
    /* Always right after the RPN 6 above, never without it: JUCE-based
     * receivers (ROLI Equator among them) reset the zone's bend range to
     * the spec's 48-semitone default on every RPN 6 -- see services/
     * midi_channels.h's header on the mid-session re-declaration that used
     * to skip this. */
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
