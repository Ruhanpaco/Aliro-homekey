<div align="center">

<img src="https://ruhanpaco.github.io/Aliro-homekey/assets/aliro-mark.svg" width="120" alt="Aliro HomeKey logo">

# Aliro HomeKey

### Open-source Aliro access reader for ESP32

[![Status](https://img.shields.io/badge/status-v0.4%20beta-ff79c6?style=for-the-badge)](docs/ROADMAP.md)
[![ESP32](https://img.shields.io/badge/ESP32-supported-8be9fd?style=for-the-badge)](https://www.espressif.com/en/products/socs/esp32)
[![Aliro](https://img.shields.io/badge/Aliro-supported-50fa7b?style=for-the-badge)](docs/ARCHITECTURE.md)
[![Matter](https://img.shields.io/badge/Matter-supported-8be9fd?style=for-the-badge)](docs/ARCHITECTURE.md)
[![License](https://img.shields.io/badge/license-Apache--2.0-f1fa8c?style=for-the-badge)](LICENSE)

**One reader. One standard. Multiple wallet ecosystems.**

[Website](https://ruhanpaco.github.io/Aliro-homekey) · [Browser Flasher](https://ruhanpaco.github.io/Aliro-homekey) · [Architecture](docs/ARCHITECTURE.md) · [Roadmap](docs/ROADMAP.md) · [Releases](https://github.com/Ruhanpaco/Aliro-homekey/releases)

</div>

---

## What is Aliro HomeKey?

Aliro HomeKey turns an ESP32 into an experimental **Aliro access reader**. A phone or wearable can present an Aliro credential, the reader processes the transaction, and the ESP32 can drive an access-control output such as a relay, strike, or test LED.

The project is built around **Aliro**, the Connectivity Standards Alliance standard for interoperable mobile access credentials, rather than locking the reader to one wallet vendor.

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

> **v0.4 beta:** a phone can open the door on tested hardware.

The current implementation has been verified on an **ESP32-WROOM-32 with a PN532**. The reader has been commissioned into Apple Home, an Aliro credential has been provisioned over Matter, and both transaction paths have been exercised. The measured transactions were approximately **569 ms on the fast path** and **2047 ms on the standard path**, with the lock output driven at the end of the transaction.

The surrounding product layer is also functional: Home app lock/unlock, OTA with rollback, configuration UI, MQTT integration and Home Assistant connectivity have been exercised.

This remains a **research/beta project, not a certified access-control product**. Apple currently shows an uncertified-accessory warning during commissioning. Google and Samsung wallet flows remain untested, and the physical Apple Wallet Express Mode path still needs confirmation on the test lock.

## Why Aliro?

Traditional mobile access systems often require a reader to be built around one ecosystem. Aliro provides a common access standard designed for interoperability between credential ecosystems and access-control devices.

The goal is simple:

> **Build the reader once. Let the wallet ecosystem handle the credential experience.**

## How a tap works

Aliro transactions use **ISO 7816 APDUs over ISO 14443-4 NFC**. BLE and UWB are also part of the broader Aliro specification, while NFC is the current focus of this project.

1. The reader detects a device and selects the Aliro applet.
2. The expedited phase authenticates the reader and device using P-256 cryptography.
3. The device identifies its credential through an Aliro key slot.
4. The reader checks the credential against its local access data.
5. The transaction completes and the access-control output is driven.

Previously seen credentials can use the **fast transaction path**, reducing the work required during a tap.

## Built with ESP-IDF + Aliro

The project keeps the stack deliberately simple:

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

The **Espressif ESP-Aliro SDK** handles the Aliro protocol, cryptography, session state and transaction processing. Aliro HomeKey builds the hardware and product layer around it: NFC transport, credentials, access decisions, lock control, configuration, OTA, MQTT, Matter and the device UI.

## Supported boards

Aliro HomeKey is designed for the **ESP32 family**. Board support is separated through the `boards/` directory so hardware-specific defaults can be maintained without changing the core firmware.

### Tested

| Board | Status | NFC | Notes |
| --- | --- | --- | --- |
| **ESP32-WROOM-32** | ✅ Tested | PN532 | Primary development and transaction-testing platform |

### Supported by the ESP-Aliro platform

The Espressif Aliro SDK supports a broader ESP32 target family, allowing Aliro HomeKey to be extended to additional boards:

`ESP32` · `ESP32-S3` · `ESP32-C3` · `ESP32-C6` · `ESP32-H2` · `ESP32-P4`

> Board-level validation is not identical to SDK target support. A target listed above may require its own pin map, partition configuration, NFC wiring and hardware validation before being considered fully supported by Aliro HomeKey.

See [`boards/`](boards/) for the board-specific configuration available in this repository.

## Hardware

The primary development setup is:

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
- Relay, electric strike, lock controller or LED for the access output
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

## Quick start

Install **ESP-IDF 5.2–6.0** and make sure `idf.py` and `openssl` are available.

```bash
idf.py set-target esp32
```

```bash
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/sdkconfig.defaults.esp32" build
```

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

The development build generates a reader identity under `main/certs/`. Generated credentials are ignored by Git and should not be committed.

## Browser flasher

Prefer a graphical setup? Provision a development reader identity directly from the browser.

<div align="center">

### [Open the Aliro HomeKey Browser Flasher →](https://ruhanpaco.github.io/Aliro-homekey)

**WebSerial · Browser Crypto · No backend**

</div>

The static flasher generates the reader identity locally in the browser and communicates with the ESP32 over WebSerial. Private keys are not uploaded to a backend.

Use a desktop browser with WebSerial support such as Chrome, Edge or Opera.

## Matter integration

Matter is optional and disabled in the normal build. The Matter build exposes the firmware as a **Matter Door Lock** endpoint with Aliro provisioning support.

The controller provisions the reader configuration and credentials through Matter, while the actual access transaction remains an NFC Aliro transaction.

```bash
docker run --rm -it -v "$PWD:/work" -w /work \
  espressif/esp-matter:latest bash -lc \
  '. $IDF_PATH/export.sh && . $ESP_MATTER_PATH/export.sh && \
   idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.matter" \
   set-target esp32 build'
```

## Configuration

On first boot without Wi-Fi credentials, the reader starts a setup network:

```text
Aliro-Setup-XXXX
password: aliro1234
```

Then open:

```text
http://192.168.4.1/
```

Configuration is stored in NVS, so changing wiring or network settings does not require rebuilding the firmware.

Configurable areas include:

- NFC interface and bus
- GPIO assignments
- Lock output and polarity
- Wi-Fi
- MQTT
- System settings

## MQTT + Home Assistant

MQTT is optional and disabled by default. When enabled, the reader can publish lock state and tap events, receive unlock commands and announce itself to Home Assistant.

See [`docs/WEB.md`](docs/WEB.md) for the configuration service and API details.

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

Before opening a pull request, read [`CONTRIBUTING.md`](CONTRIBUTING.md) and check the current roadmap.

## Important disclaimer

Aliro HomeKey is a **research and hobbyist implementation**. It is not certified for production access control.

Aliro is a trademark of the Connectivity Standards Alliance. This project is not affiliated with or endorsed by the CSA, Espressif, Apple, Google or Samsung.

A shipping access-control product requires the appropriate ecosystem provisioning, certification, security review and hardware validation.

## License

Released under the **Apache License 2.0**. See [`LICENSE`](LICENSE).

<div align="center">

**Aliro HomeKey**

`ESP32` · `Aliro` · `ESP-IDF` · `Matter` · `MQTT` · `Open Source`

[Back to top](#aliro-homekey)

</div>
