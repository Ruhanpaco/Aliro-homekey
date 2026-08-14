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

## 1. Get the firmware

From the [Actions tab](https://github.com/Ruhanpaco/Aliro-homekey/actions),
open the newest green run on `main` and download the `firmware-esp32`
artifact (or `firmware-esp32s3`). Unzip it.

```bash
pip install esptool
```

## 2. Flash

**ESP32:**

```bash
esptool.py --chip esp32 --port /dev/tty.usbserial-0001 --baud 460800 write_flash 0x1000 bootloader.bin 0x8000 partition-table.bin 0x15000 ota_data_initial.bin 0x20000 aliro_homekey.bin
```

**ESP32-S3** — the bootloader sits at `0x0`, not `0x1000`:

```bash
esptool.py --chip esp32s3 --port /dev/tty.usbmodem101 --baud 460800 write_flash 0x0 bootloader.bin 0x8000 partition-table.bin 0x15000 ota_data_initial.bin 0x20000 aliro_homekey.bin
```

Two offsets here are not the usual ones. The app is at **`0x20000`**, not
`0x10000`, because `partitions.csv` puts a 48 KB NVS, the OTA data and the PHY
data ahead of `ota_0`. And `ota_data_initial.bin` has to be written, or the OTA
data partition is left blank. `flasher_args.json` in the artifact is what the
build actually used — trust it over any snippet, including this one.

Find your port with `ls /dev/tty.usb*` on macOS or `ls /dev/ttyUSB*` on Linux.

## 3. Watch it boot

```bash
esptool.py --port /dev/tty.usbserial-0001 --baud 115200 read_flash_status
```

For the log, any serial terminal at **115200 baud** works — `screen`, `minicom`,
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

## 4. Poke it over serial

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

## 5. Reach the web UI

Until Wi-Fi credentials exist, the reader runs its own access point:

- Join **`Aliro-Setup-XXXX`**, password **`aliro1234`**.
- A configuration page should open by itself (captive portal). If not, browse
  to **http://192.168.4.1/**.

After `wifi <ssid> <password>` and a restart, it joins your network instead and
prints its address in the boot log.

## What this test does and does not prove

**Proves:** the firmware builds, boots, persists configuration, brings up
Wi-Fi in both modes, serves the UI, drives the lock output, and accepts
console commands.

**Does not prove anything about Aliro.** There is no NFC chip driver yet, so no
card is ever detected and no transaction ever runs. That is
[Milestone 2](ROADMAP.md).

## If it does not boot

- **Nothing on serial** — wrong port, or wrong baud. The ESP32 bootloader
  prints at 115200.
- **Boot loop with `invalid header`** — the app was written to the wrong
  offset. Check `flasher_args.json`.
- **`Partitions tables occupies ... does not fit`** — a 2 MB board. It needs a
  partition table without two OTA slots.
- **Brownout / reset when the relay fires** — power the relay separately; a
  USB port cannot start a strike.
