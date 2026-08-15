# First bench test

Getting a board to boot this firmware, without installing ESP-IDF.

The build runs in CI for `esp32` and `esp32s3` on ESP-IDF 5.4 and 5.5, and
publishes the flashable binaries, so nothing has to be built locally. The
binaries are attached to each [release](https://github.com/Ruhanpaco/Aliro-homekey/releases)
and to every green CI run.

Flash them with a browser-based flasher, or with `esptool` as below.

## What you need

- An **ESP32** board with **4 MB flash** — an ESP32-WROOM-32 DevKitC or an
  ESP32-S3 DevKit. `partitions.csv` reserves two 1.875 MB app slots, which does
  not fit on a 2 MB part.
- A USB cable that carries data.
- Nothing wired to the lock GPIO yet. The default is GPIO 2, which is the
  on-board LED on most DevKitCs — good enough to watch an unlock.

## 1. Flash it — the easy way

Each [release](https://github.com/Ruhanpaco/Aliro-homekey/releases) attaches a
**factory image** per target, flashed at offset `0x0`:

- `esp32.firmware.factory.bin`
- `esp32s3.firmware.factory.bin`

One file, one offset, nothing to get wrong. It is padded to the full 4 MB, so
flashing it overwrites a previous install completely rather than leaving old
NVS behind. Use any browser flasher that takes
a raw image — [Adafruit WebSerial ESPTool](https://adafruit.github.io/Adafruit_WebSerial_ESPTool/)
works well:

1. **Connect**, and check the chip it reports matches the image you picked.
2. **Erase** on a first install, so NVS starts clean.
3. Put the merged `.bin` in the **first slot at offset `0`**, leave the rest
   empty, and **Program**.

Or with `esptool`:

```bash
esptool.py --chip esp32 --port /dev/tty.usbserial-0001 --baud 460800 write_flash 0x0 esp32.firmware.factory.bin
```

## 2. Flash it — the app alone

`<target>.firmware.bin` is the app partition on its own: the actual firmware,
without the bootloader or partition table. Use it to reflash during development
without disturbing the stored configuration.

```bash
esptool.py --chip esp32 --port /dev/tty.usbserial-0001 --baud 460800 write_flash 0x20000 esp32.firmware.bin
```

This is also the image the reader accepts over the air — see below. Because the
web UI is compiled into the application rather than kept on a separate
filesystem, this one file carries both the firmware and its interface; there is
no second image to install alongside it.

The CI artifact (Actions tab) additionally carries the individual images if you
want to write them one by one:

**ESP32:**

```bash
esptool.py --chip esp32 --port /dev/tty.usbserial-0001 --baud 460800 write_flash 0x1000 bootloader.bin 0x8000 partition-table.bin 0x15000 ota_data_initial.bin 0x20000 aliro_homekey.bin
```

**ESP32-S3** — the bootloader sits at `0x0`, not `0x1000`:

```bash
esptool.py --chip esp32s3 --port /dev/tty.usbmodem101 --baud 460800 write_flash 0x0 bootloader.bin 0x8000 partition-table.bin 0x15000 ota_data_initial.bin 0x20000 aliro_homekey.bin
```

Two of those offsets are not the usual ones. The app is at **`0x20000`**, not
`0x10000`, because `partitions.csv` puts a 48 KB NVS, the OTA data and the PHY
data ahead of `ota_0`. And `ota_data_initial.bin` has to be written, or the OTA
data partition is left blank. `flasher_args.json` in the zip is what the build
actually used — trust it over any snippet, including this one.

Find your port with `ls /dev/tty.usb*` on macOS or `ls /dev/ttyUSB*` on Linux.

## 3. Update it without a cable

Once the reader is on your network, **OTA Update** in the web UI takes that same
`<target>.firmware.bin` and writes it into whichever of the two application
slots is not currently running, then reboots into it.

A freshly installed image boots once on trial. It is only made permanent after
it reaches the end of startup with the console and the configuration UI running
— so a build that panics on the way up, or that cannot serve its own UI, is put
back by the bootloader at the next reset. A bad update costs a power cycle, not
a cable.

Do **not** feed the factory image to OTA. It contains the bootloader and
partition table and is laid out for offset `0x0`, so it cannot boot from an
application slot; the UI refuses a file with `.factory.` in its name for exactly
this reason.

## 4. Watch it boot

Reset the board and open a serial terminal at **115200 baud** — `screen`, `minicom`,
`picocom`, or the Arduino IDE's monitor:

```bash
screen /dev/tty.usbserial-0001 115200
```

(`screen` exits with `Ctrl-A` then `K`.)

A healthy first boot looks like this:

```text
I (312) aliro/app: Aliro HomeKey starting
I (318) aliro/config: no stored configuration, using defaults
I (325) aliro/access: credential 'dev-credential' registered
I (331) aliro/access: lock output on GPIO 2 (active high), unlock 3000 ms
W (338) nfc: no driver implemented for the selected chip, using the stub transport
W (338) nfc/stub: no NFC frontend configured; reader will never detect a device
I (344) aliro/reader: NFC transport: stub
I (351) aliro/group-id: 00 11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF
W (358) aliro/net: no Wi-Fi credentials stored
W (365) aliro/net: setup AP 'Aliro-Setup-XXXX' is up, open http://192.168.4.1/
I (372) aliro/web: configuration UI listening on port 80
I (379) aliro/console: debug console ready - type 'help' for commands
```

Every line above is a real code path: the Aliro SDK is initialised, a reader is
created and enabled, its identity is persisted to NVS, the credential's key
slot is derived by the SDK, and the polling task is running. The only thing
missing is the radio.

## 5. Poke it over serial

The same serial connection is an interactive console. Press Enter for a prompt:

```text
aliro> help
aliro> status
aliro> nfc
aliro> unlock
aliro> wifi "My Network" hunter2
aliro> restart
```

`unlock` drives the lock GPIO for the configured duration — on a DevKitC the
on-board LED lights for three seconds. That is the first end-to-end proof that
configuration, access control and the GPIO output all work together.

`wifi` saves credentials and survives a restart, so you can get the device onto
your network without ever opening the web UI.

## 6. Reach the web UI

Until Wi-Fi credentials exist, the reader runs its own access point:

- Join **`Aliro-Setup-XXXX`**, password **`aliro1234`**.
- A configuration page should open by itself (captive portal). If not, browse
  to **http://192.168.4.1/**.

After `wifi <ssid> <password>` and a restart, it joins your network instead and
prints its address in the boot log.

## 7. The Matter build

A second image, built by the `matter-firmware` workflow, is the same firmware
with the Matter door lock endpoint switched on. Flash
`esp32.matter.firmware.factory.bin` at `0x0` exactly as above — it wipes the
board, so stored Wi-Fi credentials and the reader identity go with it.

Work through it in this order, because each step gates the next.

**a. The boot log.** Three lines appear that the ordinary build never prints:

```text
aliro/matter: commissioning payload: MT:...
aliro/matter: manual pairing code:   ...
aliro/matter: door lock on endpoint 1, 0 fabric(s)
```

If those are there, the stack came up and found its commissioning data.

**b. The web UI.** This is the real thing under test, not Matter. In this build
the Matter stack initialises `esp_netif` and the Wi-Fi driver, and `net_manager`
joins what is already running rather than creating its own. If that handover is
wrong, the symptom is the setup portal or the dashboard never appearing — so
check it before anything else, and keep the USB cable attached, because a board
with no UI and no network is one you reflash rather than fix.

The dashboard gains a **Matter** card showing the pairing code and whether a
controller has provisioned a reader identity.

**c. Commissioning.** With `chip-tool`:

```bash
chip-tool pairing ble-wifi 1 "<ssid>" "<password>" 20202021 3840
```

The card should move to "Commissioned (1)".

**d. Provisioning.** `SetAliroReaderConfig` hands the lock a reader key pair,
then `SetCredential` with an Aliro endpoint key enrols a phone. The reader
restarts on the new identity and the card shows "Provisioned".

**What this cannot do yet:** Apple, Google and Samsung refuse to commission it.
They require a device attestation certificate chaining to a root on the CSA's
compliance ledger, which comes with certification; this image carries
esp-matter's test credentials. `chip-tool` and Home Assistant accept those.

## What this test does and does not prove

**Proves:** the firmware builds, boots, persists configuration, brings up
Wi-Fi in both modes, serves the UI, drives the lock output, and accepts
console commands.

**Says nothing about a real tap.** The PN532 driver has never run against
silicon, and neither has the Aliro transaction on this hardware. A board with
no reader wired up still boots and still serves its UI; it just never sees a
card.

## If it does not boot

- **Nothing on serial** — wrong port, or wrong baud. The ESP32 bootloader
  prints at 115200.
- **Boot loop with `invalid header`** — the app was written to the wrong
  offset. Check `flasher_args.json`.
- **`Partitions tables occupies ... does not fit`** — a 2 MB board. It needs a
  partition table without two OTA slots.
- **Brownout / reset when the relay fires** — power the relay separately; a
  USB port cannot start a strike.
