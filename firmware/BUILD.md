# Building the firmware

## One-time setup

Needs `arm-none-eabi-gcc` (10.3 or newer), `cmake` and a pico-sdk
checkout:

```bash
brew install cmake picotool
git clone -b master https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
cd ~/pico-sdk && git submodule update --init
export PICO_SDK_PATH=~/pico-sdk   # add to your shell profile
cp "$PICO_SDK_PATH/external/pico_sdk_import.cmake" firmware/pico_sdk_import.cmake
```

`pico_sdk_import.cmake` is SDK boilerplate and `.gitignore`d: copy it
from the SDK you build against rather than editing it.

On an Apple-silicon Mac use a native (arm64) `arm-none-eabi-gcc`, e.g. Arm's
own toolchain (`brew install --cask gcc-arm-embedded`, which includes the
newlib C library the SDK needs). An Intel-only build of the compiler only
runs under Rosetta; without it the build fails with "Bad CPU type in
executable", and Apple is phasing Rosetta out after macOS 27. After
switching compilers, delete `firmware/build` (CMake caches the compiler
path) and configure again, pointing at the one to use if several are
installed:

```bash
PICO_TOOLCHAIN_PATH=/Applications/ArmGNUToolchain/15.3.rel1/arm-none-eabi cmake -DPICO_BOARD=pico2 ..
```

## Build

```bash
cd firmware
mkdir -p build && cd build
cmake -DPICO_BOARD=pico2 ..
make -j
```

`-DPICO_BOARD=pico2` is required (without it the image targets the
RP2040 and the board won't boot). A clean build has zero warnings. The
output is `build/src/sentia_tiles_firmware.uf2`.

## Flash

With the board running the app (no BOOTSEL button needed):

```bash
../tools/flash.sh
```

`AGENTS.md` ("Flash") covers how it works, picking one of several
boards, and the fallbacks.

## Tests

Host-side unit tests, no SDK or hardware needed:

```bash
./test/run.sh    # from firmware/
```

See `test/README.md` for what each one covers.
