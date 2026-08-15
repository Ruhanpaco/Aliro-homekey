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

What is still not solved is **attestation**. Commissioning requires a device
attestation certificate chaining to a CSA-recognised root, which is issued as
part of certification. With esp-matter's test credentials this commissions with
`chip-tool` and Home Assistant and refuses to commission with Apple, Google or
Samsung. That is the remaining gap between "a phone can hold a key for this
reader" and "a phone you own can", and it is paperwork rather than code.

## Milestone 4 — protocol depth

Reader certificates (`LOAD CERT` / `AUTH1` policies), mailbox exchange, the
step-up phase, vendor extensions. All are already-present SDK entry points that
this codebase does not call yet.

## Milestone 5 — the product layer

Only once a tap opens a door reliably: `esp_event` app loop, OTA, multiple
locks and readers.

Two gaps in the web service that must close before this touches a real door:

- **Authentication.** The configuration UI is currently open to anyone on the
  network, including anyone who joins the setup AP.
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
