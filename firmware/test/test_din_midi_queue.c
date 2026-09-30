/* midi/din_midi_queue.c -- the hardware-free half of DIN MIDI OUT: ordering,
 * coalescing of pitch bend/pressure/expression, Real-Time priority,
 * whole-message overflow drops, ring wraparound. */
#include "din_midi_queue.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int drain(uint8_t *out, int max) {
    int n = 0; uint8_t b;
    while (n < max && tiles_din_queue_pop_tx_byte(&b)) out[n++] = b;
    return n;
}
static void push(uint8_t a, uint8_t b, uint8_t c) { uint8_t m[3] = {a, b, c}; assert(tiles_din_queue_push_message(m, 3)); }
static void push2(uint8_t a, uint8_t b) { uint8_t m[2] = {a, b}; assert(tiles_din_queue_push_message(m, 2)); }
static void push1(uint8_t a) { assert(tiles_din_queue_push_message(&a, 1)); }

int main(void) {
    uint8_t out[1024]; int n;

    /* 1. reliable messages: exact bytes, in order */
    tiles_din_queue_init();
    push(0x91, 60, 100); push(0x81, 60, 0);
    n = drain(out, 64);
    assert(n == 6 && !memcmp(out, (uint8_t[]){0x91,60,100,0x81,60,0}, 6));

    /* 2. pitch bend coalesces to the latest value; nothing goes out until service() */
    tiles_din_queue_init();
    for (int i = 0; i < 50; i++) push(0xE3, i & 0x7F, 0x40);
    assert(drain(out, 64) == 0);
    assert(tiles_din_queue_service());
    n = drain(out, 64);
    assert(n == 3 && out[0] == 0xE3 && out[1] == 49 && out[2] == 0x40);
    assert(!tiles_din_queue_service()); /* nothing left dirty */

    /* 3. ordering: a pending bend goes out BEFORE a later reliable message on the same channel */
    tiles_din_queue_init();
    push(0xE2, 0, 0x40);            /* bend center on ch3 (wire ch nibble 2) */
    push(0x82, 60, 0);              /* note off, same channel */
    n = drain(out, 64);
    assert(n == 6 && !memcmp(out, (uint8_t[]){0xE2,0,0x40,0x82,60,0}, 6));
    /* a reliable message on a DIFFERENT channel must not drag another channel's pending value along */
    tiles_din_queue_init();
    push(0xE2, 1, 1); push(0x93, 60, 90);
    n = drain(out, 64);
    assert(n == 3 && out[0] == 0x93);
    assert(tiles_din_queue_service()); n = drain(out, 64); assert(n == 3 && out[0] == 0xE2);

    /* 4. real-time jumps the queue. Also exercises running status for the
     * first time: the second note-on shares the first one's status (0x90),
     * so it's compressed to just its two data bytes -- 6 total, not 7. */
    tiles_din_queue_init();
    push(0x90, 60, 100); push(0x90, 62, 100);
    push1(0xFA);
    n = drain(out, 64);
    assert(n == 6 && !memcmp(out, (uint8_t[]){0xFA, 0x90, 60, 100, 62, 100}, 6));

    /* 5. continuous values wait while the ring is deep, then flow. Distinct
     * channels each time (not all 0x90) so none of these 10 compress via
     * running status -- the point here is the byte-count threshold, not
     * that feature, so keep the count exactly what it says. */
    tiles_din_queue_init();
    for (int i = 0; i < 10; i++) push((uint8_t)(0x90 | ((i + 1) & 0x0F)), 60 + i, 100); /* 30 bytes > threshold (24) */
    push(0xE0, 5, 5);
    assert(!tiles_din_queue_service());
    uint8_t b; for (int i = 0; i < 10; i++) tiles_din_queue_pop_tx_byte(&b);  /* 20 left <= 24 */
    assert(tiles_din_queue_service());

    /* 6. overflow: whole messages only, drops counted, never a partial
     * message. Channel cycles 0-15 (never two equal adjacent values, even
     * across the wrap from 15 back to 0) so running status never once
     * compresses anything here -- this test is about overflow, not that
     * feature, so every message stays a clean, independent 3 bytes. */
    tiles_din_queue_init();
    int accepted = 0;
    for (int i = 0; i < 400; i++) {
        uint8_t m[3] = {(uint8_t)(0x90 | (i & 0x0F)), (uint8_t)(i & 0x7F), 100};
        if (tiles_din_queue_push_message(m, 3)) accepted++;
    }
    assert(accepted == 170);                       /* (512-1)/3 */
    assert(tiles_din_queue_tx_dropped() == 230);
    n = drain(out, 1024);
    assert(n == 510 && n % 3 == 0);
    for (int i = 0; i < n; i += 3) assert(out[i + 2] == 100);

    /* 7. CC classification: sustain reliable (never merged), expression coalesced.
     * Both sustain messages share status 0xB0 -- the second is exactly the
     * running-status case, compressed to its 2 data bytes. */
    tiles_din_queue_init();
    push(0xB0, 64, 127); push(0xB0, 64, 0);
    n = drain(out, 64);
    assert(n == 5 && !memcmp(out, (uint8_t[]){0xB0, 64, 127, 64, 0}, 5)); /* both sustain messages present */
    tiles_din_queue_init(); /* isolate from part 1 -- otherwise the first coalesced flush below (channel 0,
                              * same 0xB0 status as the sustain messages just sent) would itself compress */
    for (int i = 0; i < 100; i++) for (int ch = 0; ch < 16; ch++) push(0xB0 | ch, 11, i & 0x7F);
    int total = 0;
    for (int k = 0; k < 40; k++) { tiles_din_queue_service(); total += drain(out, 1024); }
    assert(total == 16 * 3);                                       /* pedal sweep: 16 messages, not 1600 */

    /* 8. rejected: SysEx, System Common, data bytes, oversize */
    tiles_din_queue_init();
    uint8_t sx[3] = {0xF0, 1, 2}; assert(!tiles_din_queue_push_message(sx, 3));
    uint8_t dt[1] = {0x40};       assert(!tiles_din_queue_push_message(dt, 1));
    uint8_t sc[2] = {0xF1, 0};    assert(!tiles_din_queue_push_message(sc, 2));
    assert(drain(out, 64) == 0);

    /* 9. channel pressure + slide + mod round-trip byte-exact. mod (CC1) and
     * slide (CC74) are DIFFERENT controllers but the SAME status byte
     * (0xB5, a Control Change on channel 5) -- running status is per
     * STATUS BYTE, not per controller, so slide (flushed right after mod)
     * correctly compresses to just its 2 data bytes; pressure (0xD5) is a
     * different status and stays full. */
    tiles_din_queue_init();
    push2(0xD5, 77); push(0xB5, 74, 33); push(0xB5, 1, 9);
    assert(tiles_din_queue_service());
    n = drain(out, 64); assert(n == 7);
    /* order is slot order: pressure, mod(CC1), slide(CC74, compressed) */
    assert(!memcmp(out, (uint8_t[]){0xD5,77, 0xB5,1,9, 74,33}, 7));

    /* 10. round-robin: service() resumes after the last flushed slot */
    tiles_din_queue_init();
    for (int ch = 0; ch < 16; ch++) push(0xE0 | ch, ch, 0);
    int seen[16] = {0}; int rounds = 0, got = 0;
    tiles_din_queue_service();                       /* shallow-ring limit: only part flushes per call */
    assert(tiles_din_queue_tx_pending_bytes() <= 27u && tiles_din_queue_tx_pending_bytes() > 0u);
    n = drain(out, 1024); got += n;
    for (int i = 0; i < n; i += 3) seen[out[i] & 0xF]++;
    while (got < 48 && rounds++ < 10) {              /* later calls pick up where the first stopped */
        tiles_din_queue_service(); n = drain(out, 1024); got += n;
        for (int i = 0; i < n; i += 3) seen[out[i] & 0xF]++;
    }
    assert(got == 48);
    for (int ch = 0; ch < 16; ch++) assert(seen[ch] == 1);

    /* 11. RX ring: order, overflow flag, recovery */
    tiles_din_queue_init();
    for (int i = 0; i < 255; i++) assert(tiles_din_queue_rx_push((uint8_t)i));
    assert(!tiles_din_queue_rx_push(0xAA));
    assert(tiles_din_queue_rx_take_overflow());
    assert(!tiles_din_queue_rx_take_overflow());
    for (int i = 0; i < 255; i++) { assert(tiles_din_queue_rx_pop(&b)); assert(b == (uint8_t)i); }
    assert(!tiles_din_queue_rx_pop(&b));
    for (int i = 0; i < 1000; i++) { assert(tiles_din_queue_rx_push((uint8_t)i)); assert(tiles_din_queue_rx_pop(&b) && b == (uint8_t)i); } /* wrap */

    /* 12. TX ring wraparound with mixed reliable traffic */
    tiles_din_queue_init();
    for (int round = 0; round < 500; round++) {
        push(0x90 | (round & 15), round & 0x7F, 100);
        n = drain(out, 8);
        assert(n == 3 && out[0] == (0x90 | (round & 15)) && out[1] == (round & 0x7F));
    }
    /* 13. running status, dedicated: three consecutive same-status
     * messages compress to just their data bytes after the first; a
     * genuine status change (even back to one used earlier) always sends
     * the full byte again; a Real-Time byte injected between two
     * running-status messages doesn't disturb it (spec: Real-Time bytes
     * never cancel running status); a fresh init resets it, so the very
     * next message after a reset is never wrongly compressed against
     * whatever the previous test left behind. */
    tiles_din_queue_init();
    push(0x93, 10, 100); push(0x93, 11, 100); push(0x93, 12, 100);
    n = drain(out, 64);
    assert(n == 7 && !memcmp(out, (uint8_t[]){0x93, 10, 100, 11, 100, 12, 100}, 7));

    tiles_din_queue_init();
    push(0x93, 10, 100); push(0x83, 10, 0); push(0x93, 20, 100);
    n = drain(out, 64); /* status changes 0x93 -> 0x83 -> 0x93: every one full, even the repeat */
    assert(n == 9 && !memcmp(out, (uint8_t[]){0x93, 10, 100, 0x83, 10, 0, 0x93, 20, 100}, 9));

    tiles_din_queue_init();
    push(0x93, 10, 100); push1(0xF8); push(0x93, 11, 100);
    n = drain(out, 64);
    assert(n == 6 && !memcmp(out, (uint8_t[]){0xF8, 0x93, 10, 100, 11, 100}, 6)); /* RT jumps ahead; running status unaffected */

    tiles_din_queue_init(); /* reset really does clear it -- not left over from the block just above */
    push(0x93, 30, 100);
    n = drain(out, 64);
    assert(n == 3 && !memcmp(out, (uint8_t[]){0x93, 30, 100}, 3));

    printf("din_midi_queue: all tests pass\n");
    return 0;
}
