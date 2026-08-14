#!/usr/bin/env bash
#
# Compile and run the host-side tests for app_config.
#
#   test/host/run.sh
#
# No ESP-IDF and no hardware: the ESP-IDF headers this code touches are stubbed
# in stubs/, and NVS is a fake in nvs_fake.c. The stubs model an ESP32 (GPIO
# 0-39, 34-39 input-only), so the pin rules under test are the real ones.

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
src="$root/components/app_config"
build="$here/build"
mkdir -p "$build"

# cJSON is an ESP-IDF component on the device; fetch a copy for the host.
if [ ! -f "$build/cJSON.c" ]; then
    echo "fetching cJSON..."
    base="https://raw.githubusercontent.com/DaveGamble/cJSON/master"
    curl -fsSL "$base/cJSON.c" -o "$build/cJSON.c"
    curl -fsSL "$base/cJSON.h" -o "$build/cJSON.h"
fi

cc="${CC:-cc}"
"$cc" -std=gnu11 -Wall -Wextra -Wno-unused-parameter -g \
    -o "$build/test_config" \
    "$here/test_config.c" "$here/nvs_fake.c" "$build/cJSON.c" \
    "$src/app_config.c" "$src/gpio_rules.c" \
    -I"$build" -I"$here/stubs" -I"$src/include"

"$build/test_config"

# The PN532 driver against a simulated chip. Nothing here needs hardware: the
# fake parses the frames the driver emits and validates both checksums, so a
# framing mistake fails on a laptop instead of looking like bad wiring.
"$cc" -std=gnu11 -Wall -Wextra -Wno-unused-parameter -g \
    -o "$build/test_pn532" \
    "$here/test_pn532.c" \
    "$root/components/nfc_transport/pn532.c" \
    -I"$here/stubs" -I"$root/components/nfc_transport/include" \
    -I"$root/components/app_config/include"

"$build/test_pn532"
