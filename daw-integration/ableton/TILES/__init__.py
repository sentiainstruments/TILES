"""
Ableton Live Control Surface entry point for SENTIA TILES.

Ableton finds a Control Surface by folder name (this one, "TILES") and
calls create_instance() below to obtain it -- see this package's own
TILES.py for the actual implementation and daw-integration/README.md
for how to install this folder into Ableton so it shows up at all.

get_capabilities() tells Live which of TILES's two USB MIDI ports is which
(firmware/src/midi/midi_ports.h) -- real feedback: "do 8 as how
standardized stuff works. production ready industry stuff." Same shape as
Ableton's own bundled Launchkey MK3 script (read from Live 12's
Launchkey_MK3/__init__.pyc): port 1 ("SENTIA TILES MIDI") is the
instrument -- Track and Remote on, and Live's clock goes out to it for
TILES's sequencer; port 2 ("SENTIA TILES DAW") is this script's -- Live
routes it to the script only, so Scene Launch/transport messages can
never reach an instrument track, and nothing TILES plays can be claimed
by the script (the CC64 sustain collision in daw-integration/README.md).
Listed in the device's own port order: inputs first, then outputs.
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

    # Must match firmware/src/midi/product_identity.h (TILES_USB_VID/_PID --
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
            # Lets Live pick this script by itself when it sees a TILES --
            # what "this should just work like it does for launchkey out of
            # the box" asked for. Only takes effect once the folder is
            # installed (daw-integration/README.md); Live versions without
            # auto-load just ignore the key's absence.
            capabilities[AUTO_LOAD_KEY] = True
        return capabilities

except ImportError:
    # A Live version without the v2 capabilities module: no auto-detection,
    # the ports are picked by hand in Preferences (daw-integration/README.md).
    pass


def create_instance(c_instance):
    return TILES(c_instance)
