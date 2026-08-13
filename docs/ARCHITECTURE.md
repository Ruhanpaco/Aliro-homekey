# Architecture

One seam per component, and a `main/` that only wires them together.

```text
                       ┌──────────────────────────────┐
   user device  ~NFC~  │        nfc_transport         │  ISO 14443-4 link
   (phone/watch)       │  init/poll/activate/exchange │  one driver per chip
                       └──────────────┬───────────────┘
                                      │ APDUs
                       ┌──────────────▼───────────────┐
                       │        aliro_reader          │  polling task
                       │  esp_aliro_lib + session run │  owns the transaction
                       └──────────────┬───────────────┘
                                      │ aliro_reader_result_t
                       ┌──────────────▼───────────────┐
                       │       access_control         │  who + what happens
                       │  credentials, decision, lock │
                       └──────────────────────────────┘

   browser  ~HTTP~  ┌─────────────┐   reads/writes   ┌────────────┐
                    │ web_server  │ ───────────────▶ │ app_config │ ──▶ NVS
                    │ REST + UI   │                  └────────────┘
                    └─────────────┘                        │ applied at boot
                    ┌─────────────┐                        ▼
                    │ net_manager │              transport, lock, group id
                    │ STA / AP    │
                    └─────────────┘
```

## Why these three

Every part of this project that will change independently is on one side of a
boundary:

- **The NFC chip will change.** PN532, PN7160, ST25R3916 all differ in bus,
  driver and quirks, and the right one is not settled. `nfc_transport_t` is a
  five-function vtable; a new chip is one new file.
- **The protocol will not change.** `esp_aliro_lib` is a prebuilt binary from
  Espressif. `aliro_reader` is a thin wrapper whose job is the task loop, the
  session lifecycle, and turning a transaction into a result struct. Keeping it
  thin means SDK upgrades stay cheap.
- **The policy will change constantly.** Schedules, users, revocation,
  multi-lock setups, integrations — all of that is `access_control`, and none
  of it can force a change in protocol code.

## The one non-obvious design decision

`esp_aliro_lib` treats key-slot lookup as optional. This project treats it as
mandatory.

Without it, a successful transaction tells you only that *some* valid Aliro
device authenticated — you get an `ESP_OK` and no identity. The key-slot
lookup callback is the single point in the API where the reader learns *which*
credential is presenting itself, because the device sends a key slot and the
reader answers with the matching credential public key.

So the callback does double duty: it resolves the key, and it records the slot
for the access decision that follows. `access_control` refuses any transaction
that completes without a key slot — an authenticated stranger is still a
stranger.

## Data flow of one tap

```text
reader_task
  transport->poll()
  transport->activate()            device selected
  esp_aliro_session_create()
  esp_aliro_session_run_expedited()
      └─> on_message_exchange()    ──> transport->exchange()   (many times)
      └─> on_lookup_credential()   ──> access_control_lookup_credential()
                                       records key slot + hit/miss
  esp_aliro_session_run_exchange()
  esp_aliro_session_delete()
  transport->deactivate()
  on_result()                      ──> access_control_on_reader_result()
                                       decision + access_control_unlock()
```

The result callback runs **on the reader task**, right after the field is
released. That is fine for a GPIO, and wrong for anything slow. When the first
network integration lands, this is where an `esp_event` loop goes — the reader
posts an event and returns to polling. HomeKey-ESP32 arrived at the same
structure (`app_event_loop`), and that is the natural next step here, not a
thing to build before there is a second consumer.

## Constraints inherited from the SDK

- **One reader, one session at a time.** `esp_aliro_init` fails if a reader
  exists. Hence `aliro_reader` is a singleton.
- **SDK callbacks carry no user context.** They trampoline through module
  state, which is safe only because of the singleton rule above.
- **Configuration order matters.** Certificate, vendor extension and key-slot
  lookup must all be set *before* `esp_aliro_reader_enable()`.
- **NVS is required.** The reader group sub-identifier and fast-transaction
  keys are persisted there, so `nvs_flash_init()` comes before everything.
- **The mbedTLS build matters.** P-256, ECDH, ECDSA, AES-GCM, HKDF and PEM
  parsing must be enabled; `sdkconfig.defaults` does that.

## Configuration is a boot-time input, not a live control

`app_config` owns a struct in NVS; `web_server` edits it; everything else
reads it once during `app_main()` and never looks again. Nothing reconfigures
a running SPI bus or a lock GPIO underneath the reader, which is why saving
tells the user a restart is required.

That is a deliberate simplification, and the reason `app_config` has no locks
or change notifications. When something genuinely needs live reconfiguration,
it gets an explicit apply path — not a shared mutable struct that any task
might read mid-transaction.

`net_manager` and `web_server` start *after* the reader for the same reason a
door lock should not depend on Wi-Fi: the reader is the product, the web
service is administration.

## Deliberately not here yet

Mailbox exchange, the step-up phase, reader certificates, BLE and UWB, a
persistent credential database, enrollment, web UI, MQTT/Home Assistant, OTA.
The seams they will attach to exist; the features do not.
