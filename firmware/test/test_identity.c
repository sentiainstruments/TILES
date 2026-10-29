/* midi/identity.c -- the Universal MIDI Identity Request/Reply handshake. Stubs the two
 * midi_in.h/midi_out.h functions it calls so the real identity.c compiles and runs on the host. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "midi_in.h"
static tiles_midi_in_sysex_callback_t g_cb;
bool tiles_midi_in_register_sysex_callback(tiles_midi_in_sysex_callback_t cb) { g_cb = cb; return true; }

static uint8_t g_last_reply[32];
static uint32_t g_last_len;
static int g_send_count;
static tiles_midi_port_t g_last_port;
void tiles_midi_send_sysex(tiles_midi_port_t port, const uint8_t *data, uint32_t len) {
    assert(len <= sizeof(g_last_reply));
    g_last_port = port;
    memcpy(g_last_reply, data, len);
    g_last_len = len;
    g_send_count++;
}

#include "identity.c"

static void request_on(tiles_midi_port_t port, uint8_t device_id) {
    uint8_t req[4] = {0x7E, device_id, 0x06, 0x01};
    g_cb(port, req, sizeof(req));
}
static void request(uint8_t device_id) { request_on(TILES_MIDI_PORT_MAIN, device_id); }

int main(void) {
    tiles_midi_identity_init();
    assert(g_cb != NULL);

    /* 1. a real request (any device id byte) gets exactly one reply, correctly shaped. */
    g_send_count = 0;
    request(0x7F);
    assert(g_send_count == 1);
    assert(g_last_len == 13);
    uint8_t expect[13] = {0x7E, 0x7F, 0x06, 0x02, TILES_SYSEX_MANUFACTURER_ID, 0x01, 0x00, 0x00, 0x00,
                          TILES_FW_VERSION_MAJOR, TILES_FW_VERSION_MINOR, TILES_FW_VERSION_PATCH, 0x00};
    assert(memcmp(g_last_reply, expect, 13) == 0);
    assert(g_last_reply[4] == 0x7D); /* pre-production: the non-commercial/development ID */

    /* 2. the device id byte in the REQUEST is ignored -- reply always uses 0x7F (unaddressed). */
    g_send_count = 0;
    request(0x03);
    assert(g_send_count == 1 && g_last_reply[1] == 0x7F);

    /* 3. every wrong shape is silently ignored -- no reply, ever. */
    struct { uint8_t bytes[6]; size_t len; } bad[] = {
        {{0x7D, 0x7F, 0x06, 0x01}, 4},          /* wrong universal ID (this is Scene Launch's own manufacturer byte) */
        {{0x7E, 0x7F, 0x07, 0x01}, 4},          /* wrong sub-ID #1 (not General Information) */
        {{0x7E, 0x7F, 0x06, 0x02}, 4},          /* sub-ID #2 says Reply, not Request */
        {{0x7E, 0x7F, 0x06}, 3},                /* truncated */
        {{0x7E, 0x7F, 0x06, 0x01, 0x00}, 5},    /* one byte too long */
        {{0}, 0},                               /* empty frame */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        g_send_count = 0;
        g_cb(TILES_MIDI_PORT_MAIN, bad[i].bytes, bad[i].len);
        assert(g_send_count == 0);
    }

    /* 4. a real Scene Launch frame (manufacturer 0x7D, unrelated to the Universal Non-Realtime
     * ID 0x7E this file owns) never triggers a reply either -- the two protocols coexist. */
    g_send_count = 0;
    uint8_t scene_frame[] = {0x7D, 0x01, 0x10, 0, 0, 0, 0, 0, 0};
    g_cb(TILES_MIDI_PORT_DAW, scene_frame, sizeof(scene_frame));
    assert(g_send_count == 0);

    /* 5. the reply goes back on the port the request came in on */
    request_on(TILES_MIDI_PORT_DAW, 0x7F);
    assert(g_send_count == 1 && g_last_port == TILES_MIDI_PORT_DAW);
    request_on(TILES_MIDI_PORT_MAIN, 0x7F);
    assert(g_send_count == 2 && g_last_port == TILES_MIDI_PORT_MAIN);

    printf("identity: all tests pass\n");
    return 0;
}
