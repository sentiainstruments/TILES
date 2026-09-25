#include "midi_in.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

uint32_t g_now_ms;
static uint8_t usb_q[256]; static int usb_h, usb_t;
static uint8_t din_q[256]; static int din_h, din_t;
static bool din_loss;
uint32_t tud_midi_stream_read(void *buf, uint32_t n) { uint32_t k = 0; uint8_t *o = buf; while (k < n && usb_t < usb_h) o[k++] = usb_q[usb_t++]; return k; }
bool tiles_din_midi_rx_read_byte(uint8_t *b) { if (din_t < din_h) { *b = din_q[din_t++]; return true; } return false; }
bool tiles_din_midi_rx_take_loss(void) { bool l = din_loss; din_loss = false; return l; }
static void usb(const uint8_t *b, int n) { memcpy(usb_q + usb_h, b, n); usb_h += n; }
static void din(const uint8_t *b, int n) { memcpy(din_q + din_h, b, n); din_h += n; }
#define USB(...) do { uint8_t _b[] = {__VA_ARGS__}; usb(_b, sizeof _b); } while (0)
#define DIN(...) do { uint8_t _b[] = {__VA_ARGS__}; din(_b, sizeof _b); } while (0)

typedef struct { uint8_t ch, note, vel; bool on; } note_ev_t;
static note_ev_t notes[64]; static int nnotes;
static uint8_t rts[256]; static int nrt;
static uint8_t sysex[8][16]; static size_t sysex_len[8]; static int nsx;
static void on_note(uint8_t ch, uint8_t n, uint8_t v, bool on, uint32_t t) { (void)t; notes[nnotes++] = (note_ev_t){ch, n, v, on}; }
static void on_rt(uint8_t b, uint32_t t) { (void)t; rts[nrt++] = b; }
static void on_sx(const uint8_t *d, size_t l) { memcpy(sysex[nsx], d, l); sysex_len[nsx++] = l; }
static void reset(void) { usb_h = usb_t = din_h = din_t = 0; din_loss = false; nnotes = nrt = nsx = 0; tiles_midi_in_init();
    tiles_midi_in_register_note_callback(on_note); tiles_midi_in_register_realtime_callback(on_rt); tiles_midi_in_register_sysex_callback(on_sx); g_now_ms = 10000; }

int main(void) {
    /* 1. two sources interleaving partial messages don't cross-contaminate */
    reset();
    USB(0x90, 0x3C);              /* USB: Note-On ch0 note 60, velocity still pending */
    DIN(0x91, 0x40, 0x64);        /* DIN: complete Note-On ch1 note 64 */
    tiles_midi_in_scan();
    assert(nnotes == 1 && notes[0].ch == 1 && notes[0].note == 0x40 && notes[0].vel == 0x64 && notes[0].on);
    USB(0x64);                    /* USB's velocity arrives next scan */
    tiles_midi_in_scan();
    assert(nnotes == 2 && notes[1].ch == 0 && notes[1].note == 0x3C && notes[1].vel == 0x64);

    /* 2. running status is per source */
    reset();
    USB(0x90, 60, 100); DIN(0x92, 50, 90);
    USB(62, 100);       DIN(52, 90);          /* both continue under running status */
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
    USB(0xF8, 0xF8); tiles_midi_in_scan(); assert(nrt == 2);
    g_now_ms += 100; DIN(0xF8, 0xF8, 0xFA); USB(0xF8); tiles_midi_in_scan();
    assert(nrt == 3);                                  /* only USB's tick; DIN's clock+start dropped */
    /* owner silent > 500 ms -> DIN takes over, USB is now the one dropped */
    g_now_ms += 600; DIN(0xF8, 0xFA); tiles_midi_in_scan();
    assert(nrt == 5 && rts[3] == 0xF8 && rts[4] == 0xFA);
    g_now_ms += 100; USB(0xF8, 0xFC); tiles_midi_in_scan();
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
    /* ...and a loss on DIN never disturbs USB's parser mid-message */
    reset(); USB(0x90, 0x3C); tiles_midi_in_scan(); din_loss = true; tiles_midi_in_scan(); USB(0x64); tiles_midi_in_scan();
    assert(nnotes == 1 && notes[0].note == 0x3C);

    /* 8. SysEx from DIN is delivered, and doesn't collide with a USB SysEx in flight */
    reset();
    USB(0xF0, 0x7D, 0x01); DIN(0xF0, 0x7D, 0x02, 0xF7); tiles_midi_in_scan();
    assert(nsx == 1 && sysex_len[0] == 2 && sysex[0][1] == 0x02);
    USB(0x03, 0xF7); tiles_midi_in_scan();
    assert(nsx == 2 && sysex_len[1] == 3 && sysex[1][0] == 0x7D && sysex[1][2] == 0x03);

    /* 9. activity counter moves for DIN notes and DIN clock, not for Active Sensing */
    reset(); uint32_t a0 = tiles_midi_in_activity_count();
    DIN(0xFE); tiles_midi_in_scan(); assert(tiles_midi_in_activity_count() == a0);
    DIN(0x90, 60, 100, 0xF8); tiles_midi_in_scan(); assert(tiles_midi_in_activity_count() == a0 + 2);

    /* 10. unchanged behavior: USB-only stream parses exactly as before */
    reset(); USB(0x93, 40, 127, 41, 127, 0x83, 40, 0); tiles_midi_in_scan();
    assert(nnotes == 3 && notes[0].on && notes[1].on && !notes[2].on && notes[2].ch == 3);
    printf("midi_in (USB + DIN): all tests pass\n");
    return 0;
}
