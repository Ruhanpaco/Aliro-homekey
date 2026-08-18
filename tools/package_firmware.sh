#!/usr/bin/env bash
#
# Assemble the release artifacts for one target, from inside the ESP-IDF
# environment (esptool has to be on PATH).
#
#     tools/package_firmware.sh esp32
#     tools/package_firmware.sh esp32 matter    # names the files *.matter.*
#
# Produces, in firmware/:
#
#   <target>.firmware.bin          the app image alone. Flash at the app offset,
#                                  or serve it as an OTA update. Small.
#   <target>.firmware.factory.bin  bootloader + partition table + OTA data + app,
#                                  padded to the full flash size and flashed at
#                                  0x0. One file, one offset, and it overwrites
#                                  a previous install completely.

set -euo pipefail

target="${1:?usage: package_firmware.sh <target> [variant]}"
variant="${2:-}"
root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/firmware"
mkdir -p "$out"

# A variant builds the same project with a different configuration, so its
# images must not overwrite the plain ones in the same artifact.
name="$target${variant:+.$variant}"

cd "$root/build"

# @flash_args carries the flash mode/freq/size and every offset/file pair the
# build decided on, so the factory image cannot drift from the real layout.
if ! python -m esptool --chip "$target" merge_bin \
        -o "$out/$name.firmware.factory.bin" -f raw \
        --fill-flash-size 4MB @flash_args 2>/dev/null; then
    echo "note: --fill-flash-size unsupported, producing an unpadded image"
    python -m esptool --chip "$target" merge_bin \
        -o "$out/$name.firmware.factory.bin" -f raw @flash_args
fi

cp aliro_homekey.bin "$out/$name.firmware.bin"

echo "packaged $name:"
ls -l "$out"
