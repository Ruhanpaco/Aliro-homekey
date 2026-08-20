<div align="center">

<img src="https://ruhanpaco.github.io/Aliro-homekey/assets/aliro-mark.svg" width="120" alt="Aliro HomeKey logo">

# Aliro HomeKey

### Open-source Aliro access reader for ESP32

[![Status](https://img.shields.io/badge/status-v0.5%20beta-ff79c6?style=for-the-badge)](https://github.com/Ruhanpaco/Aliro-homekey/releases)
[![ESP32](https://img.shields.io/badge/ESP32-supported-8be9fd?style=for-the-badge)](https://www.espressif.com/en/products/socs/esp32)
[![Aliro](https://img.shields.io/badge/Aliro-supported-50fa7b?style=for-the-badge)](docs/ARCHITECTURE.md)
[![Matter](https://img.shields.io/badge/Matter-supported-8be9fd?style=for-the-badge)](docs/ARCHITECTURE.md)
[![License](https://img.shields.io/badge/license-Apache--2.0-f1fa8c?style=for-the-badge)](LICENSE)

**One reader. One standard. Multiple wallet ecosystems.**

[Website](https://ruhanpaco.github.io/Aliro-homekey) · [Browser Flasher](https://ruhanpaco.github.io/Aliro-homekey) · [Architecture](docs/ARCHITECTURE.md) · [Roadmap](docs/ROADMAP.md) · [Releases](https://github.com/Ruhanpaco/Aliro-homekey/releases)

</div>

---

## What is Aliro HomeKey?

Aliro HomeKey turns an ESP32 into an experimental **Aliro access reader**. A phone or wearable presents an Aliro credential, the reader runs the transaction, and the ESP32 drives an access-control output — a relay, strike, or test LED.

It's built around **Aliro**, the Connectivity Standards Alliance standard for interoperable mobile access credentials, instead of locking the reader to one wallet vendor.

```text
┌──────────────────────────────────────────────────────────────┐
│                         MOBILE WALLETS                       │
│                                                              │
│   Apple Wallet     Google Wallet     Samsung Wallet     ...   │
└──────────────┬──────────────┬──────────────┬─────────────────┘
               │              │              │
               └──────────────┼──────────────┘
                              │
                         Aliro protocol
                              │
                              ▼
                  ┌──────────────────────┐
                  │     ESP32 READER     │
                  │                      │
                  │ NFC · Aliro · Matter │
                  │ MQTT · NVS · OTA     │
                  └──────────┬───────────┘
                             │
                             ▼
                         LOCK / RELAY
```

## Current status

> **v0.5 beta:** a phone opens the door on tested hardware, and the surrounding product layer works alongside it.

Verified on an **ESP32-WROOM-32 with a PN532**: commissioned into Apple Home, provisioned with an Aliro credential over Matter, and driven through both transaction paths — roughly **569 ms on the fast path** and **2047 ms on the standard path**, ending with the lock output firing. Home app lock/unlock, OTA with rollback, the configuration UI, MQTT, and Home Assistant discovery have all been exercised on the same board.

`main` also carries fixes not yet in a tagged release: the Apple ECP beacon defaults back off after it was found to wedge the NFC bus, Wi-Fi power save is disabled once connected (it was causing Matter subscriptions to silently time out), and a duplicate-notification bug in Matter lock reporting is fixed. These will ship together in the next release.

This remains a **research/beta project, not a certified access-control product**. Apple shows an uncertified-accessory warning during commissioning. Google and Samsung wallet flows are untested, and Apple Wallet Express Mode still needs confirmation on the physical test lock.

## Why Aliro?

Traditional mobile access systems are usually built around one ecosystem. Aliro is a common access standard designed for interoperability between credential ecosystems and access-control devices.

> **Build the reader once. Let the wallet ecosystem handle the credential experience.**

## Get firmware onto a board

There are four ways to do this — pick whichever fits. Click a heading below to expand it.

<details open>
<summary><strong>Build from source</strong> — full control, requires the ESP-IDF toolchain</summary>

Install **ESP-IDF 5.2–6.0** and make sure `idf.py` and `openssl` are on your PATH.

```bash
idf.py set-target esp32
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/sdkconfig.defaults.esp32" build
idf.py -p /dev/ttyUSB0 flash monitor
```

The first build generates a development reader identity under `main/certs/`. Generated credentials are gitignored — see [`main/certs/README.md`](main/certs/README.md) before using them for anything but development.

</details>

<details>
<summary><strong>Flash a released build</strong> — no toolchain needed, one file, esptool only</summary>

Grab the matching pair from [Releases](https://github.com/Ruhanpaco/Aliro-homekey/releases) for your target (`esp32`, `esp32c3`, or `esp32s3`):

```bash
# Wipes the board and installs everything: bootloader, partitions, app
python -m esptool --chip esp32 write_flash 0x0 esp32.firmware.factory.bin

# Or, over the device's own OTA page once it's already running:
# open http://<device-ip>/#/ota and upload esp32.firmware.bin
```

</details>

<details>
<summary><strong>Browser flasher</strong> — no toolchain, no CLI, generates a unique identity in-tab</summary>

<div align="center">

### [Open the Aliro HomeKey Browser Flasher →](https://ruhanpaco.github.io/Aliro-homekey)

**WebSerial · Browser Crypto · No backend**

</div>

Generates a unique P-256 reader identity locally in the browser, builds an NVS partition image, and writes it over WebSerial. Private keys never leave the tab. Needs desktop Chrome, Edge, or Opera.

</details>

<details>
<summary><strong>Matter build</strong> — same firmware, plus a Matter Door Lock endpoint</summary>

Matter is optional and off in the normal build. Turning it on lets a controller (Apple Home, Google Home, Home Assistant) provision the reader identity and enrolled credentials directly — the NFC transaction itself is unchanged either way.

```bash
docker run --rm -it -v "$PWD:/work" -w /work \
  espressif/esp-matter:latest_idf_v5.5.4 bash -lc \
  '. "$IDF_PATH/export.sh" && . "$ESP_MATTER_PATH/export.sh" && \
   idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.matter" \
   set-target esp32 build'
```

GitHub Actions runs the same build through the `matter-firmware` workflow and publishes factory and OTA images to [Releases](https://github.com/Ruhanpaco/Aliro-homekey/releases).

</details>

## How a tap works

Aliro transactions use **ISO 7816 APDUs over ISO 14443-4 NFC**. BLE and UWB are part of the broader Aliro specification; NFC is this project's current focus.

1. The reader detects a device and selects the Aliro applet.
2. The expedited phase authenticates reader and device with P-256 cryptography.
3. The device identifies its credential through an Aliro key slot.
4. The reader checks that key slot against its local access data.
5. The transaction completes and the access-control output fires.

A previously-seen credential can use the **fast transaction path**, which skips most of the handshake.

## Built with ESP-IDF + Aliro

<div align="center">

| Layer | Technology |
| --- | --- |
| **Access protocol** | **Aliro** |
| **Aliro implementation** | **Espressif ESP-Aliro SDK** |
| **Firmware framework** | **ESP-IDF** |
| **Hardware** | **ESP32 family** |
| **NFC** | **PN532 / compatible NFC frontends** |
| **Smart-home integration** | **Matter** |
| **Messaging** | **MQTT** |
| **Automation** | **Home Assistant** |

</div>

The **Espressif ESP-Aliro SDK** handles the Aliro protocol, cryptography, session state, and transaction processing. Aliro HomeKey builds the hardware and product layer around it: NFC transport, credentials, access decisions, lock control, configuration, OTA, MQTT, Matter, and the device UI.

## Board support

<details open>
<summary><strong>ESP32-WROOM-32</strong> — ✅ hardware-tested</summary>

The primary development and transaction-testing platform. Everything under [Current status](#current-status) above was verified on this board.

</details>

<details>
<summary><strong>ESP32-C3 / ESP32-S3</strong> — builds and releases, not yet hardware-verified</summary>

CI builds and publishes firmware for both targets on every release, and both pass the OTA-slot-fit check. Neither has been run against real hardware by this project yet — pin maps exist in [`boards/`](boards/) and `components/app_config/Kconfig`, but treat them as unverified until someone confirms a tap actually opens a lock on one.

</details>

<details>
<summary><strong>ESP32-C6 / ESP32-H2 / ESP32-P4</strong> — SDK-supported, no board work done here</summary>

The Espressif Aliro SDK targets these chips, so Aliro HomeKey could be extended to them, but no pin defaults, partition tuning, or testing exists in this repository yet.

</details>

> Board-level validation is not the same as SDK target support. A target listed above may still need its own pin map, partition configuration, NFC wiring, and hardware validation before being considered fully supported.

## Hardware

```text
                    ┌──────────────────┐
                    │  ESP32-WROOM-32   │
                    └────────┬─────────┘
                             │
                ┌────────────┼────────────┐
                │            │            │
                ▼            ▼            ▼
             PN532        Wi-Fi       Lock / Relay
              NFC                         / LED
                │
                ▼
           Mobile wallet
```

Typical hardware:

- **ESP32-WROOM-32** or another supported ESP32 target
- **PN532** or compatible NFC frontend
- Relay, electric strike, lock controller, or LED for the access output
- USB connection for development and flashing

## Features

<table>
<tr>
<td width="50%">

### Access

- Aliro reader implementation
- NFC transaction handling
- Fast and standard transaction paths
- Credential/key-slot based access decisions
- Lock / relay GPIO output

</td>
<td width="50%">

### Device

- Browser-based configuration UI
- Wi-Fi setup access point
- NVS-backed configuration
- OTA updates with rollback
- Hardware and GPIO validation

</td>
</tr>
<tr>
<td>

### Integrations

- Matter Door Lock endpoint
- Aliro provisioning through Matter
- MQTT
- Home Assistant discovery
- Lock state and tap events

</td>
<td>

### Developer tools

- ESP-IDF based
- Browser flasher
- Serial diagnostics
- Board-specific defaults
- Architecture and roadmap documentation

</td>
</tr>
</table>

## Configuration

On first boot without Wi-Fi credentials, the reader starts a setup network:

```text
Aliro-Setup-XXXX
password: aliro1234
```

Then open `http://192.168.4.1/`. Configuration is stored in NVS, so changing wiring or network settings doesn't require rebuilding the firmware. Configurable areas include NFC interface and bus, GPIO assignments, lock output and polarity, Wi-Fi, MQTT, and system settings.

MQTT is optional and off by default. Enabled, the reader publishes lock state and tap events, accepts unlock commands, and announces itself to Home Assistant. See [`docs/WEB.md`](docs/WEB.md) for the full API.

## Repository layout

```text
Aliro-homekey/
│
├── site/                    # GitHub Pages: browser flasher + project website
├── main/                    # Firmware entry point
│   └── certs/               # Generated development identity material
│
├── components/
│   ├── app_config/          # Runtime configuration + NVS
│   ├── nfc_transport/       # NFC hardware abstraction
│   ├── aliro_reader/        # Aliro transaction integration
│   ├── access_control/      # Credentials + access decisions + lock output
│   ├── net_manager/         # Wi-Fi + setup AP
│   ├── mqtt_manager/        # MQTT + Home Assistant integration
│   ├── matter_lock/         # Matter Door Lock + Aliro provisioning
│   └── web_server/          # REST API + embedded configuration UI
│
├── boards/                  # Board-specific ESP-IDF defaults
├── tools/                   # Development and firmware tooling
├── docs/                    # Architecture, API and roadmap
└── README.md                # You are here
```

## Documentation

| Document | Purpose |
| --- | --- |
| [`ARCHITECTURE.md`](docs/ARCHITECTURE.md) | System boundaries, components and data flow |
| [`WEB.md`](docs/WEB.md) | Configuration service, REST API and web UI |
| [`FIRST-TEST.md`](docs/FIRST-TEST.md) | Hardware flashing and healthy boot procedure |
| [`ROADMAP.md`](docs/ROADMAP.md) | Current milestones, validation work and known gaps |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | Contribution workflow and project rules |

## Contributing

Contributions are welcome — NFC drivers, board support, wallet testing, protocol research, documentation, and bug fixes are all useful. Read [`CONTRIBUTING.md`](CONTRIBUTING.md) and check the roadmap before opening a pull request.

## Important disclaimer

Aliro HomeKey is a **research and hobbyist implementation**, not certified for production access control.

Aliro is a trademark of the Connectivity Standards Alliance. This project is not affiliated with or endorsed by the CSA, Espressif, Apple, Google, or Samsung.

A shipping access-control product requires the appropriate ecosystem provisioning, certification, security review, and hardware validation.

## License

Released under the **Apache License 2.0**. See [`LICENSE`](LICENSE).

<div align="center">

**Aliro HomeKey**

`ESP32` · `Aliro` · `ESP-IDF` · `Matter` · `MQTT` · `Open Source`

[Back to top](#aliro-homekey)

</div>
