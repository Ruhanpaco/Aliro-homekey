# Aliro HomeKey

An open-source **Aliro reader for the ESP32**.

Tap a phone or watch to a small ESP32 board and a door opens — where the phone
can be an iPhone, a Pixel or a Galaxy, because the reader speaks
[Aliro](https://csa-iot.org/all-solutions/aliro/), the cross-ecosystem standard
for mobile access credentials, rather than any one vendor's protocol.

> **Status: v0.1 — it compiles; it has never been run on a board.** The
> project structure, the Aliro transaction plumbing, the configuration web
> service, MQTT and a serial debug console are in place, and CI builds them
> for ESP32 and ESP32-S3 on ESP-IDF 5.4 and 5.5. Nobody has flashed it yet, so
> "it boots" is an expectation, not a result. There is also **no NFC chip
> driver**, so no card is ever detected and no Aliro transaction ever runs.
> See [docs/FIRST-TEST.md](docs/FIRST-TEST.md) to put it on a board, and
> [docs/ROADMAP.md](docs/ROADMAP.md) for what is missing.

## What this is, in plain terms

```text
 Apple Wallet   Google Wallet   Samsung Wallet   other Aliro wallets
        └───────────────┴───────┬───────┴────────────────┘
                                │  Aliro (one protocol, all of them)
                                ▼
                        ESP32 Aliro Reader     ← this project
                                │
                                ▼
                         Access / Lock
```

Before Aliro, putting a key in a phone meant implementing one vendor's stack:
Apple Home Key for iPhones, something else for Android. Projects like
[HomeKey-ESP32](https://github.com/rednblkx/HomeKey-ESP32) do exactly that, and
do it well — but the result is an Apple-only door.

Aliro is a Connectivity Standards Alliance standard that all the major wallets
implement. Build a reader once, and every compliant wallet can carry a key for
it. That is the whole bet of this project.

## How a tap works

Aliro is ISO 7816 APDUs carried over an ISO 14443-4 NFC link (BLE and UWB exist
in the standard too, for hands-free unlock and secure ranging; NFC comes
first). One tap runs roughly:

1. The reader polls for a device and selects the Aliro applet.
2. **Expedited phase** — reader and device authenticate each other with
   ECDH/ECDSA on P-256 and derive a session key. This is the part that decides
   whether the tap is genuine.
3. The device identifies its credential by **key slot**; the reader looks that
   slot up in its own list of allowed credentials.
4. **Exchange** closes the transaction so the phone can show its success
   animation.

A device the reader has seen before takes the **fast** path, which skips most
of the handshake — the difference between a tap that feels instant and one
that does not.

## What we build on

| Piece | What it gives us |
| --- | --- |
| [espressif/esp-aliro](https://github.com/espressif/esp-aliro) | The Aliro protocol itself, as a prebuilt ESP-IDF component (`esp_aliro_lib`). Crypto, state machine, session handling, fast transactions. Apache-2.0, prebuilt binaries per chip and IDF version. |
| [kormax/aliro](https://github.com/kormax/aliro) | Independent research notes on the protocol — APDUs, AIDs, key derivation, wallet behaviour. Our reference when the SDK's behaviour needs explaining. |
| [rednblkx/HomeKey-ESP32](https://github.com/rednblkx/HomeKey-ESP32) | Prior art for how to *organise* an ESP32 access-control product: an NFC reader interface with several chip backends, an event loop, config and credential managers, a web UI. |

**What the SDK does not give us**, and we therefore own: the NFC chip driver,
the credential store, the access decision, lock actuation, enrollment and
provisioning, and everything operational (UI, storage, OTA, integrations).

## Hardware

- **ESP32** (default target; ESP32-S3, C3, C6, H2, P4 and others are supported
  by the SDK — see `boards/`).
- An external **NFC frontend**. The ESP32 has no NFC radio. Espressif's own
  example uses an ST25R3916 (M5Stack Unit NFC); PN532 and PN7160 are the parts
  the HomeKey community uses. Not yet chosen here — see
  [docs/ROADMAP.md](docs/ROADMAP.md).
- Something to switch: relay, strike, or an LED while you develop.

## Build

Requires ESP-IDF **5.2–6.0** and `openssl` on PATH.

```bash
idf.py set-target esp32
```

```bash
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/sdkconfig.defaults.esp32" build
```

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

The first build generates a development reader identity into `main/certs/`
(gitignored — see the README there for what it is and is not).

### Without a toolchain

`site/index.html` is a single static page that does the same job in a browser:
it generates a P-256 reader identity with `crypto.subtle`, packs it into an NVS
partition image, and writes it over USB with WebSerial. It has no backend, so
the private keys never leave the tab. A board provisioned that way gets a unique
identity instead of the development one compiled into the image; `app_main`
prefers the NVS keys when they are present and falls back otherwise.

Needs Chrome, Edge or Opera on a desktop — no other browser implements
WebSerial.

### With Matter

Optional, and off by default. Turning it on presents the same firmware to phone
ecosystems as a Matter **Door Lock** (cluster `0x0101`) with the Aliro
provisioning feature, which is the standard way a controller hands a lock its
reader key pair (`SetAliroReaderConfig`) and then enrolls the phones allowed to
open it (`SetCredential` with an Aliro endpoint key). Nothing about a tap
changes: the transaction is still NFC, still handled by `aliro_reader`, and
still works with the network down.

It needs [esp-matter](https://github.com/espressif/esp-matter), which is a large
checkout with its own submodules. The easy way is Espressif's container:

```bash
docker run --rm -it -v "$PWD:/work" -w /work espressif/esp-matter:latest bash -lc '. $IDF_PATH/export.sh && . $ESP_MATTER_PATH/export.sh && idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.matter" set-target esp32 build'
```

The same build runs in CI as the `matter-firmware` workflow, which produces a
factory image and an OTA image exactly like the ordinary build does.

Two things worth knowing before flashing it:

- **It roughly doubles the application.** It still fits a 1.875 MB OTA slot on a
  4 MB board, but not by much; the workflow fails the build if it stops fitting.
- **It uses esp-matter's test attestation credentials.** `chip-tool` and Home
  Assistant will commission it. Apple, Google and Samsung will not — they
  require a real device attestation certificate, which only comes with CSA
  certification. That is a paperwork problem, not a code one, and it is the same
  for every DIY Matter device.

Commissioning details (the `MT:` payload, a link that renders it as a QR code,
and the manual pairing code) are printed at boot and shown on the dashboard.

## Layout

```text
├── site/               static GitHub Pages: browser flasher + project page
├── main/               wiring only: load config, start the reader, then the network
│   └── certs/          reader key pair + allowed credentials (generated, never committed)
├── components/
│   ├── app_config/     runtime config in NVS + pin validation rules
│   ├── nfc_transport/  hardware seam: one interface, one driver per NFC chip
│   ├── aliro_reader/   esp_aliro_lib wrapped in a polling task; owns the transaction
│   ├── access_control/ credential store, access decision, lock output
│   ├── net_manager/    Wi-Fi with a setup-AP fallback and captive portal
│   ├── mqtt_manager/   lock state, tap events, Home Assistant discovery
│   ├── matter_lock/    the Matter door lock endpoint: commissioning + Aliro provisioning
│   └── web_server/     REST API + the embedded configuration UI
├── boards/             per-board sdkconfig defaults
├── tools/              development identity generation
└── docs/               architecture, web service, roadmap
```

## Configuring it

On first boot the reader has no Wi-Fi credentials, so it starts an access
point called `Aliro-Setup-XXXX` (password `aliro1234` by default) and serves
its configuration UI at `http://192.168.4.1/`. Joining that network should pop
the page up on its own.

The console has five pages — overview, hardware, network, MQTT and system.
Everything about the wiring — NFC chip, SPI or I2C, every pin, the lock output
and its polarity — is set there and stored in NVS. No rebuild to change a pin,
and the device refuses pins that do not exist, are wired to flash, or are
already assigned to something else.

MQTT is optional and off by default. Switched on, the reader publishes lock
state and tap events, accepts unlock commands, and announces itself to Home
Assistant. Details in [docs/WEB.md](docs/WEB.md).

To see the UI without a board: `tools/build_ui_preview.py` builds a clickable
copy from the firmware's own page.

## Documentation

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — the seams, and why they are where they are
- [docs/WEB.md](docs/WEB.md) — the configuration service, its API, and what was left out
- [docs/FIRST-TEST.md](docs/FIRST-TEST.md) — flashing a board and what a healthy boot looks like
- [docs/ROADMAP.md](docs/ROADMAP.md) — milestones, and the honest list of unknowns

## Contributing

Contributions are welcome — especially a real NFC frontend, and a verified
build on real hardware. See [CONTRIBUTING.md](CONTRIBUTING.md) for the bar a
contribution has to clear, where to start, and the licensing rules.

## Licence

Apache-2.0, matching `esp_aliro_lib`.

Aliro is a trademark of the Connectivity Standards Alliance. This project is
not affiliated with or endorsed by the CSA, Espressif, Apple, Google or
Samsung. Interoperating with a shipping wallet requires a provisioned,
certified reader; this is a hobbyist and research codebase.
