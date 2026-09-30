#include "identity.h"

#include "midi_in.h"
#include "midi_out.h"
#include "product_identity.h"

#include <stddef.h>

#define UNIVERSAL_NON_REALTIME 0x7Eu
#define SUB_ID_GENERAL_INFO 0x06u
#define SUB_ID_IDENTITY_REQUEST 0x01u
#define SUB_ID_IDENTITY_REPLY 0x02u
#define DEVICE_ID_UNADDRESSED 0x7Fu

/* Family / member codes are self-assigned (a development manufacturer ID
 * has no registry): family = TILES, member = hardware revision (Rev A0).
 * Bump the member code for a distinguishable new PCB, not for firmware. */
#define TILES_IDENTITY_FAMILY_LSB 0x01u
#define TILES_IDENTITY_FAMILY_MSB 0x00u
#define TILES_IDENTITY_MEMBER_LSB 0x00u /* Rev A0 */
#define TILES_IDENTITY_MEMBER_MSB 0x00u

/* Version, 4 bytes (manufacturer's choice): major, minor, patch, 0; the
 * same version USB reports as bcdDevice (midi/product_identity.h). */

static void identity_on_sysex(tiles_midi_port_t port, const uint8_t *data, size_t len) {
    /* Request payload between F0/F7: 7E <device id> 06 01, exactly 4 bytes.
     * The device id is not checked (see the header). */
    if (len != 4u || data[0] != UNIVERSAL_NON_REALTIME || data[2] != SUB_ID_GENERAL_INFO ||
        data[3] != SUB_ID_IDENTITY_REQUEST) {
        return;
    }
    uint8_t reply[] = {UNIVERSAL_NON_REALTIME,
                       DEVICE_ID_UNADDRESSED,
                       SUB_ID_GENERAL_INFO,
                       SUB_ID_IDENTITY_REPLY,
                       TILES_SYSEX_MANUFACTURER_ID,
                       TILES_IDENTITY_FAMILY_LSB,
                       TILES_IDENTITY_FAMILY_MSB,
                       TILES_IDENTITY_MEMBER_LSB,
                       TILES_IDENTITY_MEMBER_MSB,
                       TILES_FW_VERSION_MAJOR,
                       TILES_FW_VERSION_MINOR,
                       TILES_FW_VERSION_PATCH,
                       0u};
    /* Back on the port the request came from. DIN requests get no reply (no
     * SysEx path). */
    tiles_midi_send_sysex(port, reply, sizeof(reply));
}

void tiles_midi_identity_init(void) {
    (void)tiles_midi_in_register_sysex_callback(identity_on_sysex);
}
