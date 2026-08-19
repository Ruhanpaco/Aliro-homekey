<div align="center">

<img src="https://ruhanpaco.github.io/Aliro-homekey/assets/aliro-mark.svg" width="120" alt="Aliro HomeKey logo">

# Aliro HomeKey

### An open-source Aliro access reader for the ESP32

[![Status](https://img.shields.io/badge/status-v0.4%20beta-ff79c6?style=for-the-badge)](docs/ROADMAP.md)
[![Platform](https://img.shields.io/badge/platform-ESP32-8be9fd?style=for-the-badge)](https://www.espressif.com/en/products/socs/esp32)
[![Matter](https://img.shields.io/badge/Matter-supported-50fa7b?style=for-the-badge)](docs/ARCHITECTURE.md)
[![License](https://img.shields.io/badge/license-Apache--2.0-f1fa8c?style=for-the-badge)](LICENSE)

**One reader. One standard. Multiple wallet ecosystems.**

[Website](https://ruhanpaco.github.io/Aliro-homekey) · [Browser Flasher](https://ruhanpaco.github.io/Aliro-homekey) · [Architecture](docs/ARCHITECTURE.md) · [Roadmap](docs/ROADMAP.md) · [Releases](https://github.com/Ruhanpaco/Aliro-homekey/releases)

</div>

---

## What is Aliro HomeKey?

Aliro HomeKey turns an ESP32 into an experimental **Aliro access reader**. A phone or wearable can present an Aliro credential, the reader processes the transaction, and the ESP32 can drive an access-control output such as a relay, strike, or test LED.

The project is designed around **Aliro**, the Connectivity Standards Alliance standard for interoperable mobile access credentials, instead of building a reader around a single wallet vendor.

```text
┌─────────────────────────────────────────────────────────────┐
│                    MOBILE WALLETS                           │
│                                                             │
│   Apple Wallet     Google Wallet     Samsung Wallet   ...   │
└──────────────┬──────────────┬──────────────┬────────────────┘
               │              │              │
               └──────────────┼──────────────┘
                              │
                         Aliro protocol
                              │
                              ▼
                  ┌─────────────────────┐
                  │   ESP32 READER      │
                  │                     │
                  │ NFC · Aliro · NVS   │
                  │ MQTT · Matter · UI  │
                  └──────────┬──────────┘
                             │
                             ▼
                       LOCK / RELAY
```

## Current status

> **v0.4 beta:** a phone can open the door on tested hardware.

The current implementation has been verified on an **ESP32-WROOM-32 with a PN532**. The reader has been commissioned into Apple Home, an Aliro credential has been provisioned over Matter, and both transaction paths have been exercised. The measured transactions were approximately **569 ms on the fast path** and **2047 ms on the standard path**, with the lock output driven at the end of the transaction.

The surrounding product layer is also functional: Home app lock/unlock, OTA with rollback, the configuration UI, MQTT integration and Home Assistant connectivity have been exercised.

This is still a **research/beta project, not a certified access-control product**. Apple currently shows an uncertified-accessory warning during commissioning. Google and Samsung wallet flows remain untested, and the physical Apple Wallet Express Mode path still needs confirmation on the test lock.

For the implementation details and remaining work, see [`docs/ROADMAP.md`](docs/ROADMAP.md).

## Why Aliro?

Before Aliro, a phone-based access project often meant choosing one ecosystem and implementing its protocol. Aliro changes the architecture: the reader speaks a common standard while the credential can live in a supported wallet ecosystem.

That makes the goal simple:

**Build the reader once. Let the wallet ecosystem handle the credential experience.**

## How a tap works

Aliro transactions use **ISO 7816 APDUs over ISO 14443-4 NFC**. BLE and UWB are also part of the broader Aliro specification for other access experiences, but NFC is the current focus of this project.

A transaction broadly follows this flow:

1. The reader polls for a device and selects the Aliro applet.
2. The expedited phase authenticates the reader and device using P-256 cryptography and derives session material.
3. The device identifies its credential through an Aliro key slot.
4. The reader checks that credential against its local access data.
5. The exchange completes and the access-control output is driven.

Previously seen devices can use the **fast transaction path**, reducing the amount of work required during a tap.

## What powers the project?

| Project | Role |
| --- | --- |
| [`espressif/esp-aliro`](https://github.com/espressif/esp-aliro) | Espressif's Aliro implementation used for protocol handling, cryptography, session state and transaction processing. |
| [`kormax/aliro`](https://github.com/kormax/aliro) | Independent protocol research and reference material for APDUs, AIDs, key derivation and wallet behaviour. |
| [`rednblkx/HomeKey-ESP32`](https://github.com/rednblkx/HomeKey-ESP32) | Prior art for structuring an ESP32 access-control project with NFC, configuration, credentials and a web interface. |
| **ESP-IDF** | Firmware framework, networking, storage, OTA and hardware integration. |
| **ESP-Matter** | Optional Matter Door Lock integration and Aliro provisioning path. |

The SDK handles the Aliro protocol itself. This project owns the integration around it: NFC transport, credential storage, access decisions, lock control, provisioning, configuration, OTA, MQTT, Matter integration and the device UI.

## Features

<table>
<tr>
<td width="50%">

### Access reader

- Aliro reader implementation
- NFC transaction handling
- Fast and standard transaction paths
- Credential/key-slot based access decisions
- Lock / relay GPIO output

</td>
<td width="50%">

### Device management

- Browser-based configuration UI
- Wi-Fi setup access point
- NVS-backed configuration
- OTA updates with rollback
- Hardware and pin validation

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

### Developer experience

- ESP-IDF based
- Browser flasher
- Serial diagnostics
- Board-specific configuration
- Architecture and roadmap documentation

</td>
</tr>
</table>

## Hardware

The main development target is an **ESP32-WROOM-32** paired with an external NFC frontend such as the **PN532**. Other ESP32 targets supported by Espressif's Aliro SDK can be explored as the project evolves.

Typical bench setup:

```text
ESP32-WROOM-32
      │
      ├── PN532 NFC frontend
      │
      ├── Lock / relay / LED
      │
      └── Wi-Fi / MQTT / Matter
```

The exact wiring and supported board configuration live in [`boards/`](boards/) and the project documentation.

## Quick start

### 1. Install ESP-IDF

Use ESP-IDF **5.2–6.0** and make sure `idf.py` and `openssl` are available in your shell.

### 2. Select the target

```bash
idf.py set-target esp32
```

### 3. Build

```bash
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/sdkconfig.defaults.esp32" build
```

### 4. Flash and monitor

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

The development build generates a reader identity under `main/certs/`. Generated credentials are ignored by Git and should not be committed.

## Browser flasher

You can also provision a development reader identity directly from the browser.

**[Open the Aliro HomeKey Browser Flasher →](https://ruhanpaco.github.io/Aliro-homekey)**

The static flasher uses browser cryptography to generate the reader identity and WebSerial to communicate with the ESP32. The private keys are generated locally in the browser and are not uploaded to a backend.

Use a desktop browser with WebSerial support such as Chrome, Edge or Opera.

## Matter integration

Matter is optional and disabled in the normal build. The Matter build exposes the firmware as a **Matter Door Lock** endpoint with Aliro provisioning support.

The controller can provision the reader configuration and credentials through the Matter interaction model while the actual access transaction remains an NFC Aliro transaction.

Build it using Espressif's Matter environment:

```bash
docker run --rm -it -v "$PWD:/work" -w /work \
  espressif/esp-matter:latest bash -lc \
  '. $IDF_PATH/export.sh && . $ESP_MATTER_PATH/export.sh && \
   idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.matter" \
   set-target esp32 build'
```

The Matter build is also produced by the `matter-firmware` GitHub Actions workflow.

## Configuration

On first boot without Wi-Fi credentials, the reader starts a setup network similar to:

```text
Aliro-Setup-XXXX
password: aliro1234
```

The configuration UI is available at:

```text
http://192.168.4.1/
```

Configuration is stored in NVS, so changing wiring or network settings does not require rebuilding the firmware.

The device configuration covers areas such as:

- NFC interface and bus
- GPIO assignments
- Lock output and polarity
- Wi-Fi
- MQTT
- System settings

## MQTT + Home Assistant

MQTT is optional and disabled by default. When enabled, the reader can publish lock state and tap events, receive unlock commands and announce itself to Home Assistant.

More detail is available in [`docs/WEB.md`](docs/WEB.md).

## Repository layout

```text
Aliro-homekey/
│
├── site/                    # GitHub Pages + browser flasher
│   ├── index.html           # WebSerial firmware flasher
│   ├── about.html           # Project website
│   └── styles.css           # Shared website styling
│
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
│   └── web_server/           # REST API + embedded configuration UI
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

Contributions are welcome. NFC drivers, board support, wallet testing, protocol research, documentation improvements and bug fixes are all useful.

Before opening a pull request, please read [`CONTRIBUTING.md`](CONTRIBUTING.md) and check the current roadmap so work is aligned with the project's implementation priorities.

## Important disclaimer

Aliro HomeKey is a **research and hobbyist implementation**. It is not certified for production access control.

Aliro is a trademark of the Connectivity Standards Alliance. This project is not affiliated with or endorsed by the CSA, Espressif, Apple, Google or Samsung.

A shipping access-control product requires the appropriate ecosystem provisioning, certification, security review and hardware validation.

## License

Released under the **Apache License 2.0**. See [`LICENSE`](LICENSE).

<div align="center">

**Aliro HomeKey** · ESP32 · Open Source

[Back to top](#aliro-homekey)

</div>
