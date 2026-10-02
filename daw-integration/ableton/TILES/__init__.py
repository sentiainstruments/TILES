"""
Ableton Live Control Surface entry point for SENTIA TILES.

Live finds a Control Surface by its folder name ("TILES") and calls
create_instance(). The script itself is TILES.py; installing it is in
daw-integration/README.md.

get_capabilities() tells Live which of TILES's two USB MIDI ports is which
(firmware/src/midi/midi_ports.h), in the same shape as Ableton's bundled
Launchkey MK3 script. Port 1 ("SENTIA TILES 2 MIDI" on unit 2) is the
instrument: Track and Remote on, and Live's clock goes out to it for
TILES's sequencer. Port 2 ("SENTIA TILES 2 DAW") belongs to this script,
so its messages never reach a track and nothing TILES plays is claimed by
the script. Ports are listed in the device's order: inputs, then outputs.
Each unit's name carries its number, so two units at once each get their
own TILES control surface row.
"""

from .TILES import TILES

try:
    from ableton.v2.control_surface.capabilities import (
        CONTROLLER_ID_KEY,
        NOTES_CC,
        PORTS_KEY,
        REMOTE,
        SCRIPT,
        SYNC,
        controller_id,
        inport,
        outport,
    )

    try:
        from ableton.v2.control_surface.capabilities import AUTO_LOAD_KEY
    except ImportError:
        AUTO_LOAD_KEY = None

    # Must match firmware/src/midi/product_identity.h (TILES_USB_VID/_PID,
    # the pid.codes test ID while TILES is pre-production) and the USB
    # product name in firmware/src/midi/usb_descriptors.c: "SENTIA TILES N"
    # for unit N (board/unit_id.h), plain "SENTIA TILES" before 0.2.1.
    TILES_USB_VENDOR_ID = 0x1209
    TILES_USB_PRODUCT_IDS = [0x0001]
    TILES_MODEL_NAMES = ["SENTIA TILES %d" % n for n in range(1, 5)] + ["SENTIA TILES"]

    def get_capabilities():
        capabilities = {
            CONTROLLER_ID_KEY: controller_id(
                vendor_id=TILES_USB_VENDOR_ID,
                product_ids=TILES_USB_PRODUCT_IDS,
                model_name=TILES_MODEL_NAMES,
            ),
            PORTS_KEY: [
                inport(props=[NOTES_CC, REMOTE]),  # SENTIA TILES N MIDI -> Live
                inport(props=[NOTES_CC, SCRIPT]),  # SENTIA TILES N DAW  -> this script
                outport(props=[SYNC, REMOTE]),  # Live -> SENTIA TILES N MIDI (clock)
                outport(props=[NOTES_CC, SCRIPT]),  # this script -> SENTIA TILES N DAW
            ],
        }
        if AUTO_LOAD_KEY is not None:
            # Lets Live pick this script by itself when it sees a TILES,
            # once the folder is installed. Older Live versions don't
            # have the key.
            capabilities[AUTO_LOAD_KEY] = True
        return capabilities

except ImportError:
    # A Live version without the v2 capabilities module: no auto-detection,
    # the ports are picked by hand in Preferences (daw-integration/README.md).
    pass


def create_instance(c_instance):
    return TILES(c_instance)
