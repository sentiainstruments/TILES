"""
Ableton Live Control Surface entry point for SENTIA TILES.

Live finds a Control Surface by its folder name ("TILES") and calls
create_instance(). The script itself is TILES.py; installing it is in
daw-integration/README.md.

get_capabilities() tells Live which of TILES's two USB MIDI ports is which
(firmware/src/midi/midi_ports.h), in the same shape as Ableton's bundled
Launchkey MK3 script. Port 1 ("SENTIA TILES MIDI") is the instrument:
Track and Remote on, and Live's clock goes out to it for TILES's
sequencer. Port 2 ("SENTIA TILES DAW") belongs to this script, so its
messages never reach a track and nothing TILES plays is claimed by the
script. Ports are listed in the device's order: inputs, then outputs.
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
    # product name in firmware/src/midi/usb_descriptors.c.
    TILES_USB_VENDOR_ID = 0x1209
    TILES_USB_PRODUCT_IDS = [0x0001]
    TILES_MODEL_NAMES = ["SENTIA TILES"]

    def get_capabilities():
        capabilities = {
            CONTROLLER_ID_KEY: controller_id(
                vendor_id=TILES_USB_VENDOR_ID,
                product_ids=TILES_USB_PRODUCT_IDS,
                model_name=TILES_MODEL_NAMES,
            ),
            PORTS_KEY: [
                inport(props=[NOTES_CC, REMOTE]),  # SENTIA TILES MIDI -> Live
                inport(props=[NOTES_CC, SCRIPT]),  # SENTIA TILES DAW  -> this script
                outport(props=[SYNC, REMOTE]),  # Live -> SENTIA TILES MIDI (clock)
                outport(props=[NOTES_CC, SCRIPT]),  # this script -> SENTIA TILES DAW
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
