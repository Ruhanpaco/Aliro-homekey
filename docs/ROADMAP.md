# Roadmap

## Milestone 1 — the skeleton runs (current)

**Goal:** a correctly structured ESP-IDF project that builds for the ESP32,
boots, initializes the Aliro SDK, and polls.

Done when `idf.py monitor` shows:

```text
I aliro/app     : Aliro HomeKey starting
I aliro/access  : credential 'dev-credential' registered
I aliro/access  : lock output on GPIO 2 (active high), unlock 3000 ms
W nfc/stub      : no NFC frontend configured; reader will never detect a device
I aliro/reader  : NFC transport: stub
I aliro/group-id: 00 11 22 33 ...
I aliro/app     : reader running with 1 credential(s)
```

Everything above is real code paths: the SDK is initialized, a reader is
created and enabled, its identity is persisted in NVS, the credential's key
slot is derived by the SDK, and the polling task is running. Only the radio is
missing.

- [x] ESP-IDF project skeleton, components, board defaults
- [x] `esp_aliro_lib` pulled in via the component manager
- [x] Reader lifecycle + transaction loop
- [x] Credential store keyed by Aliro key slot
- [x] Lock output with automatic relock
- [x] Development identity generation
- [x] Runtime configuration in NVS with pin validation
- [x] Wi-Fi with setup-AP fallback and captive portal
- [x] Configuration web service and multi-page UI ([docs/WEB.md](WEB.md))
- [x] MQTT: lock state, tap events, commands, Home Assistant discovery
- [x] Serial debug console for bench bring-up
- [x] **Verified building** — CI compiles for esp32 and esp32s3 on ESP-IDF 5.4
      and 5.5; the app is ~1.19 MB, 39% free in a 1.875 MB partition
- [ ] **Verified booting on real hardware** — not yet run on a board. See
      [FIRST-TEST.md](FIRST-TEST.md).

The configuration logic is covered by a host-side test (65 checks over
validation, JSON patching, MQTT rules, secret masking and persistence),
`tools/check_consistency.py` catches Kconfig and component-dependency mistakes
without a compiler, and the UI has been driven end to end against a stand-in
for the device API. The firmware itself now compiles in CI. None of that is a
substitute for a board.

## Milestone 2 — a real NFC frontend

Pick a chip and write one `nfc_transport_t` implementation.

The decision matters and is not made yet:

| Chip | Notes |
| --- | --- |
| **ST25R3916** | What Espressif's own example uses (M5Stack Unit NFC), so it is the known-good path. Driver is C++. |
| **PN532** | Cheap, everywhere, well understood by the HomeKey community. Older part; check ISO 14443-4 extended APDU and throughput against Aliro's needs before committing. |
| **PN7160** | Modern NCI part, used by HomeKey-ESP32. More capable, more driver work. |

Done when a device emulator completes an expedited transaction and the lock
GPIO fires.

## Milestone 3 — a credential lifecycle

Today credentials are compiled in. Next: persist them in NVS, add and revoke
them at runtime, and log every tap with its outcome.

This is also where the **hard unknown** sat: how a real wallet ends up holding
a credential for this reader. Espressif's SDK deliberately stops at the
transaction and leaves the credential lifecycle to the product.

Most of that is now answered, and the answer is Matter. The Door Lock cluster
(`0x0101`) carries the Aliro provisioning feature and nothing else does:
`SetAliroReaderConfig` gives the lock its reader key pair, and `SetCredential`
with an Aliro endpoint key enrolls a phone. `components/matter_lock` implements
that side, so a controller can drive the whole lifecycle over the standard
path. Credentials provisioned this way persist in NVS and are re-enrolled at
boot.

**Attestation turned out not to be the wall it looked like.** This image
carries esp-matter's test credentials, and Apple Home commissions it anyway
after warning that the accessory is uncertified. Verified on hardware: two
fabrics, a user and Aliro credentials provisioned, lock and unlock driving the
GPIO from the Home app. `chip-tool` and Home Assistant accept it too; Google
and Samsung are untested.

Certification is what a product being sold needs. It is not what this needs to
be tried.

**The other end is answered too, on both paths.** An iPhone holding an endpoint
key that Apple Home provisioned into this reader opened the lock over NFC twice
over: a fast transaction in 569 ms, where the SDK recognised a persistent key
stored from an earlier exchange, and a standard one in 2047 ms, where it ran the
full exchange and the key-slot lookup named the credential — `granted: 'matter
ev1' (standard transaction)`. The standard path is what a phone runs the first
time it meets a reader, so that is the whole protocol proven end to end.

What is not proven is the rest of the ecosystem. The Google and Samsung wallets
are untested. Express Mode — a tap that opens the door without unlocking the
phone or picking the key in Wallet — has not been available to select in Apple
Home; selecting the key manually and presenting it works. That may be
certification, since the accessory reports itself as an uncertified test vendor,
but it is a guess and not something the logs can settle.

## Milestone 4 — protocol depth

Reader certificates (`LOAD CERT` / `AUTH1` policies), mailbox exchange, the
step-up phase, vendor extensions. All are already-present SDK entry points that
this codebase does not call yet.

## Milestone 5 — the product layer

Only once a tap opens a door reliably: `esp_event` app loop, OTA, multiple
locks and readers.

Two gaps in the web service that must close before this touches a real door:

- **Authentication.** A username and password can now be set in the UI, and
  every configuration and OTA endpoint refuses without it. It is off until
  someone turns it on, it travels over plain HTTP, and there is no recovery
  path if the password is lost short of erasing the configuration over USB.
- **Logs.** There is no log view in the UI. The ring buffer that fed one cost
  ~16 KB of DRAM on a board where the web server was already failing to
  allocate a 1 KB request buffer, so it was removed; the serial port still
  prints everything.

## Later — beyond NFC

BLE-based passive unlock and UWB ranging are part of Aliro and are the reason
the standard is interesting long-term. `esp_aliro_lib` is NFC-only today.

## Open questions

1. Which NFC frontend, and does the cheapest option meet Aliro's timing?
2. ~~What exactly does credential provisioning require outside the firmware?~~
   A Matter controller and, for the phone ecosystems specifically, a device
   attestation certificate that only comes with certification. See milestone 3.
3. Where does the reader private key live in a build meant for a real door —
   flash encryption, the DS peripheral, or an external secure element?
4. Does anything here need certification to be lawful to publish and use? (The
   protocol library is Apache-2.0; interoperability claims are a separate
   matter.)
