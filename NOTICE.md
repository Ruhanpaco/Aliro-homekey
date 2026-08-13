# Notice

Aliro HomeKey
Copyright (c) 2026 Aliro HomeKey contributors

This product includes software developed by the Aliro HomeKey contributors.
Licensed under the Apache License, Version 2.0 — see [LICENSE](LICENSE).

The following third-party works are used by this project. Each retains its
own copyright and licence; their terms are set out below.

## espressif/esp-aliro

- Project: https://github.com/espressif/esp-aliro
- Copyright: Espressif Systems
- Licence: Apache-2.0
- Used as: the `esp_aliro_lib` ESP-IDF component (pulled in by the component
  manager as `espressif/esp_aliro_lib ^1.1.0`). This is the Aliro protocol
  itself — crypto, state machine, session handling — consumed as a prebuilt
  binary in `components/aliro_reader`. No source from this project is copied
  into the tree.

## rednblkx/HomeKey-ESP32

- Project: https://github.com/rednblkx/HomeKey-ESP32
- Copyright (c) 2026 rednblkx
- Licence: MIT
- Used as: prior art, with derived code in the tree.

The MIT licence notice for the portions adapted from this project reads:

> MIT License
>
> Copyright (c) 2026 rednblkx
>
> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in
> all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.

Adapted code lives in:

- `components/app_config/gpio_rules.c` — pin tables for which GPIOs exist,
  are input-only, strapping, or reserved for flash/PSRAM.
- `components/net_manager/dns_hijack.c` — minimal captive-portal DNS responder,
  adapted in spirit from HomeKey-ESP32's `dns_server` component (and ESP-IDF's
  captive portal example).
- `components/web_server/web_server.c` — the general shape of the route table
  and the HomeKey-style JSON response envelope.

## saadeghi/daisyUI

- Project: https://github.com/saadeghi/daisyui
- Copyright (c) 2020 Pouya Saadeghi
- Licence: MIT
- Used as: the design language of the configuration UI. HomeKey-ESP32 builds
  its UI with Tailwind + daisyUI on the `dracula` and `autumn` themes;
  firmware cannot ship a bundler, so `components/web_server/web/index.html`
  reproduces the daisyUI component classes it uses by hand and copies the
  theme token values (`--color-base-100`, `--color-primary`, …) verbatim from
  daisyUI's own `dracula.css` and `autumn.css`. No daisyUI source file is
  vendored, but the class names and colour values are its work.

## kormax/aliro

- Project: https://github.com/kormax/aliro
- Copyright: kormax
- Licence: none declared upstream (research notes only; no `LICENSE` file)
- Used as: a reference for understanding the Aliro protocol — APDUs, AIDs,
  key derivation, wallet behaviour. No code from this project is copied into
  the tree, and nothing from it is distributed in the firmware.

## ESP-IDF and bundled libraries

The firmware is built on the Espressif IoT Development Framework
([ESP-IDF](https://github.com/espressif/esp-idf), Apache-2.0), pulled in by the
component manager (`idf: ">=5.2,<6.1"`). The components this project links
against and the third-party libraries ESP-IDF bundles with them:

| Library / component | Role here | Licence |
| --- | --- | --- |
| [ESP-IDF](https://github.com/espressif/esp-idf) | framework; `esp_http_server`, `nvs_flash`, `esp_wifi`, `esp_netif`, `esp_event`, `esp_app_format`, `esp_hw_support`, `driver`, `esp_timer`, `soc`, `esp_mac` | Apache-2.0 |
| [mbedTLS](https://github.com/Mbed-TLS/mbedtls) | P-256 ECDH/ECDSA, AES-GCM, HKDF-SHA256, PEM parsing for the Aliro transaction | Apache-2.0 |
| [FreeRTOS](https://github.com/FreeRTOS/FreeRTOS-Kernel) | reader task, mutexes, queues (with Espressif's SMP port) | MIT |
| [lwIP](https://savannah.nongnu.org/projects/lwip/) | TCP/IP stack, sockets, DNS | BSD-3-Clause |
| [cJSON](https://github.com/DaveGamble/cJSON) | JSON for the web API and MQTT payloads (`json` component) | MIT |
| [esp-mqtt](https://github.com/espressif/esp-mqtt) | MQTT client in `mqtt_manager` | Apache-2.0 |

These ship with the ESP-IDF distribution; their notices are reproduced in
`$IDF_PATH` and apply to the prebuilt IDF libraries this firmware links
against. None are vendored in this repository.

## cJSON (host tests only)

The host-side test harness in `test/host/run.sh` downloads
[`DaveGamble/cJSON`](https://github.com/DaveGamble/cJSON) from `master` at
test time to compile `app_config` without ESP-IDF.

- Copyright (c) 2009-2017 Dave Gamble and cJSON contributors
- Licence: MIT

Its copyright and permission notice (reproduced in `test/host/build/cJSON.h`
once fetched) is the standard MIT text.

## Build tooling

- **openssl** — `tools/gen_reader_identity.sh` generates the development
  reader and credential key pairs (P-256). Apache-2.0. Invoked at build time
  only; not linked into the firmware.
- **Python 3 standard library** — `tools/build_ui_preview.py` uses only
  `pathlib`, `re` and `sys`. No third-party modules.
- The web UI (`components/web_server/web/index.html`) is hand-written HTML with
  inline CSS, inline JavaScript and inline SVG. It loads nothing from a CDN or
  any third-party script/font.
