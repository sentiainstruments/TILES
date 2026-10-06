#!/usr/bin/env python3
"""
Guided current measurement for SENTIA TILES, with an inline USB-C meter
between the computer and the unit. Each step puts the unit into a fixed
state through the settings shell's bench tests (TEST LEDS / TEST MOTORS,
firmware 0.2.6+), then asks for the meter's reading and saves it to a CSV.

    TILES_SERIAL=D8D37A03B3B6CE95 ~/.venvs/tiles-tools/bin/python tools/current_test.py results.csv
    ... tools/current_test.py results.csv --recheck   # just the combined steps, read after 3 s

Leave the pads alone while it runs (a pad event takes its motor back).
Results go in docs/hardware/ (see firmware/src/services/lighting.c's
budget comment, which this replaces with measured numbers).
"""

import csv
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PYTHON = sys.executable

OFF = ["test", "off"]
# The USB ceiling (services/power.c); keep in step if it changes.
USB_CAP = "50"
COMBO = [OFF, ["test", "leds", USB_CAP], ["test", "motors", "4", "100"]]

# (label, settings-shell test arguments, seconds to let it settle before
# reading). A step whose arguments are a list of lists runs each in turn.
STEPS = [
    ("idle, everything off", ["test", "off"], 2),
    ("LEDs white 10%", ["test", "leds", "10"], 1),
    ("LEDs white 25%", ["test", "leds", "25"], 1),
    ("LEDs white 37%", ["test", "leds", "37"], 1),
    ("LEDs white 50%", ["test", "leds", "50"], 1),
    ("LEDs white 75%", ["test", "leds", "75"], 1),
    ("LEDs white 100%", ["test", "leds", "100"], 1),
    ("LEDs off again", ["test", "off"], 1),
    ("1 motor at 100%", ["test", "motors", "1", "100"], 1),
    ("4 motors at 100% (USB max)", ["test", "motors", "4", "100"], 1),
    ("4 motors at 60% (sustain level)", ["test", "motors", "4", "60"], 1),
    ("worst case USB: LEDs at the cap + 4 motors 100%", COMBO, 1),
    ("all off", ["test", "off"], 1),
]

# A short pass over the steps that should add up (LEDs, motors, both),
# each held 3 s before reading so a motor's start-up current is over.
RECHECK = [
    ("all off", ["test", "off"], 3),
    ("4 motors at 100%", ["test", "motors", "4", "100"], 3),
    ("LEDs at the cap only", [OFF, ["test", "leds", USB_CAP]], 3),
    ("LEDs at the cap + 4 motors 100%", COMBO, 3),
    ("all off", ["test", "off"], 3),
]


def tiles(*args):
    out = subprocess.run([PYTHON, os.path.join(HERE, "tiles_control.py"), *args], capture_output=True, text=True)
    text = (out.stdout + out.stderr).strip()
    if out.returncode != 0 or "ERR" in text:
        print("  !! " + text)
    return text


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    path = args[0] if args else "current_test.csv"
    steps = RECHECK if "--recheck" in sys.argv else STEPS
    info = tiles("info")
    print(info)
    print("\nRead the meter at each step and type the current in amps (e.g. 0.34), then Enter.")
    print("Motor steps run 8 s from the start of the step: read as soon as you're asked. Type s to skip.\n")
    rows = []
    for label, test_args, settle in steps:
        for command in test_args if isinstance(test_args[0], list) else [test_args]:
            tiles(*command)
        time.sleep(settle)
        value = input(f"  {label:<44} amps: ").strip()
        volts = input(f"  {'':<44} volts (Enter to skip): ").strip()
        rows.append({"step": label, "amps": value, "volts": volts})
    tiles("test", "off")
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["step", "amps", "volts"])
        writer.writeheader()
        writer.writerows(rows)
    print(f"\nSaved {len(rows)} readings to {path}")


if __name__ == "__main__":
    main()
