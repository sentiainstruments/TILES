#include "identity.h"

#include "midi_in.h"
#include "midi_out.h"

#include <stddef.h>

#define UNIVERSAL_NON_REALTIME 0x7Eu
#define SUB_ID_GENERAL_INFO 0x06u
#define SUB_ID_IDENTITY_REQUEST 0x01u
#define SUB_ID_IDENTITY_REPLY 0x02u
#define DEVICE_ID_UNADDRESSED 0x7Fu

/* MMA-reserved "non-commercial/educational use" manufacturer ID -- the same
 * one daw-integration's own Scene Launch protocol already uses (see this
 * file's own header comment for why that's a different, unrelated use of
 * the same ID), appropriate here for the same reason: SENTIA has no
 * registered manufacturer ID of its own (see midi/usb_descriptors.c's own
 * header comment on borrowing Raspberry Pi Trading's USB VID for the same
 * underlying reason). */
#define TILES_IDENTITY_MANUFACTURER_ID 0x7Du

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

/* Software version, 4 bytes -- no real firmware version scheme exists in
 * this codebase yet (nothing else here tracks one), so this is a plain,
 * manually-maintained placeholder rather than new versioning
 * infrastructure invented as a side effect of this one feature. Update by
 * hand if a real scheme is ever built. */
#define TILES_IDENTITY_VERSION_1 0u
#define TILES_IDENTITY_VERSION_2 0u
#define TILES_IDENTITY_VERSION_3 0u
#define TILES_IDENTITY_VERSION_4 1u

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
                       TILES_IDENTITY_MANUFACTURER_ID,
                       TILES_IDENTITY_FAMILY_LSB,
                       TILES_IDENTITY_FAMILY_MSB,
                       TILES_IDENTITY_MEMBER_LSB,
                       TILES_IDENTITY_MEMBER_MSB,
                       TILES_IDENTITY_VERSION_1,
                       TILES_IDENTITY_VERSION_2,
                       TILES_IDENTITY_VERSION_3,
                       TILES_IDENTITY_VERSION_4};
    tiles_midi_send_sysex(reply, sizeof(reply));
}

void tiles_midi_identity_init(void) {
    (void)tiles_midi_in_register_sysex_callback(identity_on_sysex);
}
