# Notice

Aliro HomeKey
Copyright (c) 2026 Aliro HomeKey contributors
Licensed under the Apache License, Version 2.0 — see [LICENSE](LICENSE).

Almost all of this repository is original work. This file exists for the parts
that are not, because two of the licences involved require it. It is kept as
short as honesty allows.

## Attribution that is a licence condition

### rednblkx/HomeKey-ESP32 — MIT

<https://github.com/rednblkx/HomeKey-ESP32> · Copyright (c) 2026 rednblkx

Code adapted from this project lives in:

| File | What was taken |
| --- | --- |
| `components/app_config/gpio_rules.c` | the pin tables — which GPIOs exist, are input-only, strapping, or reserved for flash and PSRAM |
| `components/net_manager/dns_hijack.c` | the shape of the captive-portal DNS responder |
| `components/web_server/web/index.html` | the visual style of the configuration UI |

MIT requires its copyright and permission notice to travel with substantial
portions of the work:

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

### saadeghi/daisyUI — MIT

<https://github.com/saadeghi/daisyui> · Copyright (c) 2020 Pouya Saadeghi

Firmware cannot ship a bundler, so `components/web_server/web/index.html`
reproduces daisyUI's component class names by hand and copies its `dracula` and
`autumn` theme colour values verbatim. No daisyUI file is vendored, but the
class names and the colours are its work, under the same MIT terms quoted
above with Pouya Saadeghi's copyright.

## Dependencies, for information

These are linked or pulled in by the build; none are vendored in this
repository, and their own notices ship with them.

| Project | Role | Licence |
| --- | --- | --- |
| [espressif/esp-aliro](https://github.com/espressif/esp-aliro) | `esp_aliro_lib` — the Aliro protocol itself: crypto, state machine, sessions | Apache-2.0 |
| [espressif/esp-matter](https://github.com/espressif/esp-matter) and [connectedhomeip](https://github.com/project-chip/connectedhomeip) | the Matter Door Lock endpoint in the Matter build | Apache-2.0 |
| [ESP-IDF](https://github.com/espressif/esp-idf) | framework — HTTP server, NVS, Wi-Fi, netif, timers, drivers | Apache-2.0 |
| [mbedTLS](https://github.com/Mbed-TLS/mbedtls) | P-256, AES-GCM, HKDF-SHA256, PEM parsing | Apache-2.0 |
| [FreeRTOS](https://github.com/FreeRTOS/FreeRTOS-Kernel) | tasks, mutexes, queues | MIT |
| [lwIP](https://savannah.nongnu.org/projects/lwip/) | TCP/IP, sockets, DNS | BSD-3-Clause |
| [cJSON](https://github.com/DaveGamble/cJSON) | JSON for the web API and MQTT | MIT |
| [esp-mqtt](https://github.com/espressif/esp-mqtt) | MQTT client | Apache-2.0 |

## Research referenced, not used

[kormax/aliro](https://github.com/kormax/aliro) — research notes on the Aliro
protocol: APDUs, AIDs, key derivation, wallet behaviour. Read while working on
this, no code taken, nothing from it distributed in the firmware. No licence is
declared upstream.

## Trademarks

Aliro is a trademark of the Connectivity Standards Alliance. Matter is a
trademark of the CSA. This project is not affiliated with or endorsed by the
CSA, Espressif, Apple, Google or Samsung.
