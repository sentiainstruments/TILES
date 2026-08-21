# Building the firmware

Not yet buildable on this machine — `cmake` and the pico-sdk checkout are
both missing. `arm-none-eabi-gcc` (10.3) is already installed.

## One-time setup

```bash
brew install cmake
git clone -b master https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
cd ~/pico-sdk && git submodule update --init
export PICO_SDK_PATH=~/pico-sdk   # add to your shell profile
cp "$PICO_SDK_PATH/external/pico_sdk_import.cmake" firmware/pico_sdk_import.cmake
```

`pico_sdk_import.cmake` is SDK boilerplate, not project code, and is
`.gitignore`d — copy it in fresh from whatever SDK checkout you're
building against rather than hand-editing it.

## Build

```bash
cd firmware
mkdir build && cd build
cmake -DPICO_BOARD=pico2 ..
make -j
```

Produces `src/sentia_tiles_firmware.uf2` — flash by holding BOOTSEL while
plugging in the Pico 2, then copying the `.uf2` to the mass-storage
device that appears.

## Running the pad-table test (no toolchain needed)

`firmware/test/test_pad_config.c` is plain host C, no Pico SDK
dependency:

```bash
cd firmware
cc -std=c11 -Wall -Wextra -Isrc/board test/test_pad_config.c src/board/pad_config.c -o /tmp/test_pad_config
/tmp/test_pad_config
```
