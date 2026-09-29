#include "midi_in.h"
#include "usb_midi_packet.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

uint32_t g_now_ms;
/* USB arrives as USB-MIDI packets (midi/usb_midi_packet.h); USB()/DAW() queue one complete message
 * each on cable 0 (MAIN) / cable 1 (DAW) -- a USB packet never splits a channel message, only SysEx. */
static uint8_t usb_q[128][4]; static int usb_h, usb_t;
static uint8_t din_q[256]; static int din_h, din_t;
static bool din_loss;
bool tud_midi_n_packet_read(uint8_t itf, uint8_t packet[4]) { (void)itf; if (usb_t >= usb_h) return false; memcpy(packet, usb_q[usb_t++], 4); return true; }
bool tiles_din_midi_rx_read_byte(uint8_t *b) { if (din_t < din_h) { *b = din_q[din_t++]; return true; } return false; }
bool tiles_din_midi_rx_take_loss(void) { bool l = din_loss; din_loss = false; return l; }
static void usb_on(uint8_t cable, const uint8_t *b, int n) {
    size_t k = tiles_usb_midi_pack(cable, b, (size_t)n, &usb_q[usb_h], TILES_USB_MIDI_MAX_PACKETS);
    assert(k > 0); usb_h += (int)k;
}
static void usb_raw(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) { uint8_t p[4] = {b0, b1, b2, b3}; memcpy(usb_q[usb_h++], p, 4); }
static void din(const uint8_t *b, int n) { memcpy(din_q + din_h, b, n); din_h += n; }
#define USB(...) do { uint8_t _b[] = {__VA_ARGS__}; usb_on(0, _b, sizeof _b); } while (0)
#define DAW(...) do { uint8_t _b[] = {__VA_ARGS__}; usb_on(1, _b, sizeof _b); } while (0)
#define DIN(...) do { uint8_t _b[] = {__VA_ARGS__}; din(_b, sizeof _b); } while (0)

typedef struct { uint8_t ch, note, vel; bool on; } note_ev_t;
static note_ev_t notes[64]; static int nnotes;
static uint8_t rts[256]; static int nrt;
static uint8_t sysex[8][16]; static size_t sysex_len[8]; static tiles_midi_port_t sysex_port[8]; static int nsx;
static void on_note(uint8_t ch, uint8_t n, uint8_t v, bool on, uint32_t t) { (void)t; notes[nnotes++] = (note_ev_t){ch, n, v, on}; }
static void on_rt(uint8_t b, uint32_t t) { (void)t; rts[nrt++] = b; }
static void on_sx(tiles_midi_port_t port, const uint8_t *d, size_t l) { memcpy(sysex[nsx], d, l); sysex_port[nsx] = port; sysex_len[nsx++] = l; }
static void reset(void) { usb_h = usb_t = din_h = din_t = 0; din_loss = false; nnotes = nrt = nsx = 0; tiles_midi_in_init();
    tiles_midi_in_register_note_callback(on_note); tiles_midi_in_register_realtime_callback(on_rt); tiles_midi_in_register_sysex_callback(on_sx); g_now_ms = 10000; }

int main(void) {
    /* 1. two sources interleaving partial messages don't cross-contaminate: a SysEx split across USB
     * packets over two scans, with a complete DIN Note-On (and a DIN half-message) in between */
    reset();
    usb_raw(0x04, 0xF0, 0x7D, 0x01);   /* USB SysEx start/continue packet, frame still open */
    DIN(0x91, 0x40, 0x64, 0x92, 0x30); /* DIN: complete Note-On, then half of another */
    tiles_midi_in_scan();
    assert(nnotes == 1 && notes[0].ch == 1 && notes[0].note == 0x40 && notes[0].vel == 0x64 && notes[0].on && nsx == 0);
    usb_raw(0x06, 0x05, 0xF7, 0x00);   /* the SysEx ends next scan */
    DIN(0x50);                          /* ...and DIN's velocity */
    tiles_midi_in_scan();
    assert(nsx == 1 && sysex_len[0] == 3 && sysex[0][2] == 0x05 && sysex_port[0] == TILES_MIDI_PORT_MAIN);
    assert(nnotes == 2 && notes[1].ch == 2 && notes[1].note == 0x30 && notes[1].vel == 0x50);

    /* 2. running status on DIN isn't disturbed by USB traffic in between (USB-MIDI packets always
     * carry their status byte, so USB itself never uses running status) */
    reset();
    USB(0x90, 60, 100); DIN(0x92, 50, 90);
    USB(0x90, 62, 100); DIN(52, 90);          /* DIN continues under running status */
    tiles_midi_in_scan();
    int usb_notes = 0, din_notes = 0;
    for (int i = 0; i < nnotes; i++) { if (notes[i].ch == 0) usb_notes++; if (notes[i].ch == 2) din_notes++; }
    assert(usb_notes == 2 && din_notes == 2 && nnotes == 4);

    /* 3. Note-On velocity 0 = Note-Off, and Note-Off, from DIN */
    reset();
    DIN(0x90, 60, 0, 0x80, 61, 64);
    tiles_midi_in_scan();
    assert(nnotes == 2 && !notes[0].on && !notes[1].on && notes[1].note == 61);

    /* 4. clock arbitration: first source owns it; the other is ignored while owner is active */
    reset();
    USB(0xF8); USB(0xF8); tiles_midi_in_scan(); assert(nrt == 2);
    g_now_ms += 100; DIN(0xF8, 0xF8, 0xFA); USB(0xF8); tiles_midi_in_scan();
    assert(nrt == 3);                                  /* only USB's tick; DIN's clock+start dropped */
    /* owner silent > 500 ms -> DIN takes over, USB is now the one dropped */
    g_now_ms += 600; DIN(0xF8, 0xFA); tiles_midi_in_scan();
    assert(nrt == 5 && rts[3] == 0xF8 && rts[4] == 0xFA);
    g_now_ms += 100; USB(0xF8); USB(0xFC); tiles_midi_in_scan();
    assert(nrt == 5);                                  /* USB now ignored (DIN owns it) */
    g_now_ms += 100; DIN(0xFC); USB(0xF8); tiles_midi_in_scan();
    assert(nrt == 6 && rts[5] == 0xFC);                /* DIN's Stop reaches the sequencer; USB tick still dropped */

    /* 5. non-realtime bytes (Active Sensing etc.) never fire or take ownership */
    reset(); DIN(0xFE, 0xFE); USB(0xF8); tiles_midi_in_scan(); assert(nrt == 1);

    /* 6. Real-Time byte in the middle of a DIN Note-On doesn't break it */
    reset(); DIN(0x90, 0xF8, 60, 0xF8, 100); tiles_midi_in_scan();
    assert(nnotes == 1 && notes[0].note == 60 && notes[0].vel == 100 && nrt == 2);

    /* 7. loss on DIN: half-assembled message dropped, leftover data byte ignored, next status re-syncs */
    reset();
    DIN(0x90, 0x3C); tiles_midi_in_scan();
    din_loss = true; DIN(0x64); tiles_midi_in_scan();  /* would have completed the note without the loss */
    assert(nnotes == 0);
    DIN(0x90, 0x3E, 0x50); tiles_midi_in_scan();
    assert(nnotes == 1 && notes[0].note == 0x3E);
    /* ...and a loss on DIN never disturbs USB's parser mid-message (a SysEx split across scans) */
    reset(); usb_raw(0x04, 0xF0, 0x7D, 0x01); tiles_midi_in_scan(); din_loss = true; tiles_midi_in_scan();
    usb_raw(0x06, 0x09, 0xF7, 0x00); tiles_midi_in_scan();
    assert(nsx == 1 && sysex_len[0] == 3 && sysex[0][2] == 0x09);

    /* 8. SysEx from DIN is delivered, and doesn't collide with a USB SysEx in flight */
    reset();
    usb_raw(0x04, 0xF0, 0x7D, 0x01); DIN(0xF0, 0x7D, 0x02, 0xF7); tiles_midi_in_scan();
    assert(nsx == 1 && sysex_len[0] == 2 && sysex[0][1] == 0x02 && sysex_port[0] == TILES_MIDI_PORT_DIN);
    usb_raw(0x06, 0x03, 0xF7, 0x00); tiles_midi_in_scan();
    assert(nsx == 2 && sysex_len[1] == 3 && sysex[1][0] == 0x7D && sysex[1][2] == 0x03);

    /* 9. activity counter moves for DIN notes and DIN clock, not for Active Sensing */
    reset(); uint32_t a0 = tiles_midi_in_activity_count();
    DIN(0xFE); tiles_midi_in_scan(); assert(tiles_midi_in_activity_count() == a0);
    DIN(0x90, 60, 100, 0xF8); tiles_midi_in_scan(); assert(tiles_midi_in_activity_count() == a0 + 2);

    /* 10. USB-only stream: a run of Note-Ons/Offs parses exactly as before */
    reset(); USB(0x93, 40, 127); USB(0x93, 41, 127); USB(0x83, 40, 0); tiles_midi_in_scan();
    assert(nnotes == 3 && notes[0].on && notes[1].on && !notes[2].on && notes[2].ch == 3);
    /* 11. the DAW port (cable 1): its own parser, SysEx tagged with its port; its notes still reach the
     * note listeners (the melodic echo's notes come from the control surface script's output) */
    reset();
    usb_raw(0x14, 0xF0, 0x7D, 0x01);            /* DAW SysEx open... */
    USB(0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7);     /* ...while a whole MAIN SysEx arrives */
    usb_raw(0x16, 0x10, 0xF7, 0x00);             /* DAW SysEx closes */
    DAW(0x90, 60, 100);                          /* a note on the DAW port */
    DAW(0xF8);                                   /* a clock tick on the DAW port */
    tiles_midi_in_scan();
    assert(nsx == 2 && sysex_port[0] == TILES_MIDI_PORT_MAIN && sysex_len[0] == 4 && sysex[0][0] == 0x7E);
    assert(sysex_port[1] == TILES_MIDI_PORT_DAW && sysex_len[1] == 3 && sysex[1][2] == 0x10);
    assert(nnotes == 1 && notes[0].note == 60 && nrt == 1);

    /* 12. packets on a cable this device doesn't have, or with a reserved CIN, are ignored */
    reset(); usb_raw(0x29, 0x90, 60, 100); usb_raw(0x00, 0x90, 61, 100); USB(0x90, 62, 100); tiles_midi_in_scan();
    assert(nnotes == 1 && notes[0].note == 62);

    printf("midi_in (USB MAIN + DAW + DIN): all tests pass\n");
    return 0;
}
