# Aliro HomeKey

An open-source **Aliro reader for the ESP32**.

Tap a phone or watch to a small ESP32 board and a door opens — where the phone
can be an iPhone, a Pixel or a Galaxy, because the reader speaks
[Aliro](https://csa-iot.org/all-solutions/aliro/), the cross-ecosystem standard
for mobile access credentials, rather than any one vendor's protocol.

> **Status: foundation phase.** The project structure, the build, the
> transaction plumbing and the configuration web service exist. There is no
> NFC chip driver yet, so the firmware builds, boots, serves its UI and polls,
> but cannot yet open anything. See [docs/ROADMAP.md](docs/ROADMAP.md).

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

## Layout

```text
├── main/               wiring only: load config, start the reader, then the network
│   └── certs/          reader key pair + allowed credentials (generated, never committed)
├── components/
│   ├── app_config/     runtime config in NVS + pin validation rules
│   ├── nfc_transport/  hardware seam: one interface, one driver per NFC chip
│   ├── aliro_reader/   esp_aliro_lib wrapped in a polling task; owns the transaction
│   ├── access_control/ credential store, access decision, lock output
│   ├── net_manager/    Wi-Fi with a setup-AP fallback and captive portal
│   ├── mqtt_manager/   lock state, tap events, Home Assistant discovery
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
