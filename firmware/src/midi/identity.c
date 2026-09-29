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

/* Family / family-member code: self-assigned, since a non-commercial
 * manufacturer ID has no registry to assign real ones from -- these only
 * need to be internally consistent, not globally unique. Family = "TILES"
 * as a product line; member = this hardware revision (Rev A0) -- bump the
 * member code for a real, distinguishable future PCB revision a host might
 * need to tell apart, not for a firmware-only change. */
#define TILES_IDENTITY_FAMILY_LSB 0x01u
#define TILES_IDENTITY_FAMILY_MSB 0x00u
#define TILES_IDENTITY_MEMBER_LSB 0x00u /* Rev A0 */
#define TILES_IDENTITY_MEMBER_MSB 0x00u

/* Software version, 4 bytes (format is the manufacturer's choice): major,
 * minor, patch, 0 -- the same firmware version USB reports as bcdDevice,
 * both from midi/product_identity.h. */

static void identity_on_sysex(const uint8_t *data, size_t len) {
    /* Request payload (the bytes strictly between F0/F7, per midi_in.h's
     * own sysex-callback contract): 7E <device id> 06 01 -- exactly 4
     * bytes. Deliberately doesn't check data[1] (the device id byte) --
     * see this file's own header comment on why any value is accepted. */
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
    tiles_midi_send_sysex(reply, sizeof(reply));
}

void tiles_midi_identity_init(void) {
    (void)tiles_midi_in_register_sysex_callback(identity_on_sysex);
}
