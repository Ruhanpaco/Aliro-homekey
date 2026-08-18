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

The Google and Samsung wallets remain untested, and the first attempt showed why
that is harder than finding someone with the right phone.

A Galaxy owner tried and got as far as *"unable to pair since I don't have a
SmartThings Hub"*. That is not a fault in this firmware: the device advertises
for commissioning over the same BLE path Apple uses, and nothing reached it.
Samsung's Digital Home Key is added to Samsung Wallet **during** the lock's
Matter onboarding in the SmartThings app, so SmartThings has to be the
administrator, and SmartThings needs a hub to be a Matter administrator at all.
A Galaxy phone on its own cannot hold the fabric.

Worth telling a prospective tester: the hub does not have to be a hub. A
SmartThings Station, a 2022-or-later Samsung TV, a Family Hub fridge and some
Samsung soundbars all act as one, so someone may already own the missing piece.

Home Assistant is the useful half-test. Its Matter server commissions over Wi-Fi
with no hub, and can drive the Door Lock cluster including
`SetAliroReaderConfig`, which would prove the Matter and Aliro plumbing works
under an administrator that is not Apple. It will not put a key in any wallet,
so it answers half the question and should not be reported as more than that.

### Express Mode

Express Mode needs more than the Aliro APDU transaction: during discovery the
reader must emit an Apple Enhanced Contactless Polling (ECP) frame with the
Aliro TCI `20 42 20` and the first eight bytes of its reader group identifier.
Without that announcement, a locked phone does not offer the credential and the
user has to select the key in Wallet first.

The PN532 driver now implements the working PN532 cadence documented by
kormax: run a normal Type-A poll, and when it finds no target, set
`CIU_BitFraming` for an eight-bit frame and send the CRC-appended ECP beacon with
`InCommunicateThru`. The previous attempt rewrote the CRC and timeout registers
instead, omitted the bit-framing write, and left the next poll waiting for an
ACK that never came. ECP is enabled by default again now that the command stream
stays synchronized.

The remaining step is a physical locked-phone test. The ordinary and fast Aliro
transactions are proven on hardware, but this corrected discovery path should
not be called verified until the test lock accepts a tap with Wallet closed.

Certification cannot be self-issued, and this is by construction rather than an
oversight. A local attestation chain is two commands away — `chip-cert
gen-att-cert` makes a self-signed PAA, which signs a PAI, which signs a DAC. The
Certification Declaration is not: esp-matter's own certification guide notes
that the official CD is "issued by CSA after passing certification", and a
commissioner only trusts a PAA that is in the Distributed Compliance Ledger.
Attestation exists so a device cannot vouch for itself; if it could, it would
prove nothing.

Apple HomeKey over HomeKit, which rednblkx's HomeKey-ESP32 implements on an
ESP32 and a PN532, is still a different protocol and ecosystem. Its working ECP
transport is useful prior art, but this project emits the Aliro TCI and continues
with the Aliro transaction instead.

* <https://www.matteralpha.com/industry-news/espressif-s-aliro-demo-is-here-and-it-works-great>
* <https://github.com/kormax/aliro>
* <https://github.com/kormax/apple-enhanced-contactless-polling/tree/main/examples#pn532>
* <https://github.com/espressif/esp-matter/blob/main/docs/en/certification.rst>

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
