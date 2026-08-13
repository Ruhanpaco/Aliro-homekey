# Configuration web service

The reader hosts its own configuration UI. Hardware wiring — which NFC chip,
which bus, which pins, which lock output — is a **runtime setting**, not a
rebuild. Kconfig only supplies the first-boot defaults.

## Reaching it

| Situation | What happens |
| --- | --- |
| No Wi-Fi credentials stored | Starts an access point `Aliro-Setup-XXXX`, password from config (default `aliro1234`), UI at `http://192.168.4.1/` |
| Credentials stored | Joins that network, UI on the address printed in the serial log |
| Cannot join after 5 attempts | Falls back to the setup AP |

In AP mode a DNS responder answers every query with the device address, so a
phone joining the network pops the page up by itself.

## API

| Method | Path | Purpose |
| --- | --- | --- |
| `GET` | `/` | The UI. One embedded HTML file, no build step. |
| `GET` | `/api/status` | Chip, firmware, uptime, heap, network, lock, reader and MQTT state |
| `GET` | `/api/hardware` | Which pins this chip allows — drives the pin pickers |
| `GET` | `/api/config` | Running configuration, passwords masked |
| `POST` | `/api/config` | Validate and persist. Body is a partial config; missing keys keep their value |
| `POST` | `/api/config/reset` | Erase the stored configuration |
| `POST` | `/api/reboot` | Restart so a saved configuration takes effect |

`POST /api/config` returns `400` with `{"ok":false,"error":"..."}` and changes
nothing when validation fails. The UI shows that string verbatim, so the
device's own rules are the only ones that matter — the browser never decides
what is valid.

Two rules worth knowing:

- **An empty password means "keep the stored one."** Passwords are never sent
  to the browser, so a UI that never saw them cannot erase them by saving.
- **Configuration is applied at boot**, not live. Saving tells you a restart is
  required, and nothing reconfigures a running SPI bus or lock GPIO underneath
  the reader.

## Pages

Five routes, hash-based, mirroring the sidebar layout HomeKey-ESP32 uses:

| Route | What lives there |
| --- | --- |
| `#/overview` | Lock state, transport, credential count, MQTT and network status. Polled every 5 s. No fields, so no save bar. |
| `#/hardware` | NFC chip, bus, pins, clock; lock output pin, polarity, duration |
| `#/network` | Wi-Fi credentials, hostname, setup-AP password |
| `#/mqtt` | Broker, credentials, TLS, topics, Home Assistant discovery |
| `#/system` | Device name, reader group identifier, restart, factory reset |

One form spans the config pages with a single sticky save bar, because the
device applies configuration as a whole at boot. Saving from any page sends the
complete document; the device treats it as a patch.

## MQTT

Off by default. When enabled, everything hangs off one base topic:

| Topic | Direction | Payload |
| --- | --- | --- |
| `<base>/status` | out, retained | `online` / `offline` (last will) |
| `<base>/lock/state` | out, retained | `locked` / `unlocked` |
| `<base>/lock/set` | in | `LOCK` / `UNLOCK` |
| `<base>/auth` | out | JSON per tap: `granted`, `reason`, `credential`, `key_slot` |

With discovery enabled the reader also publishes a retained Home Assistant
lock entity to `homeassistant/lock/<client id>/config`, so it appears without
any YAML.

HomeKey-ESP32 configures each topic individually and carries current/target
state, jammed state, battery level, alt-action and custom state maps. Those
exist to model a HomeKit lock accessory; this project has no HomeKit, so the
topics are derived from the base and the rest is not implemented.

`UNLOCK` calls the same path a granted tap does, including the automatic
relock. `LOCK` re-publishes the current state rather than cutting the unlock
short — the strike is on a timer, and a broker should not be able to leave the
door in a state the reader did not choose.

## Pin validation

`GET /api/hardware` reports, for the chip actually being built:

- `usable_pins` — exists, and not wired to flash or PSRAM
- `input_only_pins` — cannot drive an output (ESP32 GPIO 34–39)
- `strapping_pins` — sampled at reset; usable, but the UI warns
- `restricted_pins` — never offered

The device re-checks all of it on save, plus: no pin assigned to two
functions, SPI clock 100 kHz–20 MHz, I2C clock 50 kHz–1 MHz, unlock duration
100 ms–60 s, group identifier exactly 32 hex characters.

The pin tables are adapted from
[HomeKey-ESP32](https://github.com/rednblkx/HomeKey-ESP32) (MIT, © rednblkx),
along with the general shape of the route table.

## What was deliberately left out

HomeKey-ESP32's web layer also carries HomeKit pairing, Ethernet, NeoPixel,
OTA upload, certificate management, HTTPS and WebSocket log streaming. MQTT
came across; the rest did not. There is no HomeKit here and no Ethernet, and
what remains is scope for later milestones:

- **No authentication.** Anyone on the network can reconfigure the reader, and
  anyone who can reach it can send `UNLOCK` over MQTT if a broker is
  configured. Acceptable while this drives an LED on a bench; not acceptable
  on a door.
- **No OTA.** Flash over USB.
- **No live log streaming.** Status is polled every 5 s over `/api/status`;
  logs are on the serial port.

The UI is hand-written HTML in `components/web_server/web/index.html` with no
npm, bundler or framework, and is embedded straight into the firmware. If it
outgrows one file, the API is the contract — swap in a built frontend without
touching the C.

## Previewing the UI without a board

```bash
tools/build_ui_preview.py
```

Generates `build/ui-preview.html`: the firmware's own page plus a stand-in for
the device API, so the whole console is clickable on a laptop. It is generated
rather than hand-written so it cannot drift from what the ESP32 serves.
