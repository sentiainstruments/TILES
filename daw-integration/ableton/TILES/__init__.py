"""
Ableton Live Control Surface entry point for SENTIA TILES.

Ableton finds a Control Surface by folder name (this one, "TILES") and
calls create_instance() below to obtain it -- see this package's own
TILES.py for the actual implementation and daw-integration/README.md
for how to install this folder into Ableton so it shows up at all.
"""

from .TILES import TILES


def create_instance(c_instance):
    return TILES(c_instance)
