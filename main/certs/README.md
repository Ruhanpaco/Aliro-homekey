# Reader identity

This directory holds the reader's key pair and the credentials it trusts. The
build embeds them into the firmware image.

Nothing here is committed. `tools/gen_reader_identity.sh` generates a
development set, and the build runs it automatically the first time if the
files are missing.

| File | What it is | Where it belongs |
| --- | --- | --- |
| `reader_privkey.pem` | Reader key pair, private half | Only on the ESP32 |
| `reader_pubkey.pem` | Reader key pair, public half | Given to whoever issues credentials |
| `credential_pubkey.pem` | A credential allowed to open this lock | On the reader |
| `credential_privkey.pem` | The matching credential secret | On the test device only |

Two things this is **not**:

- **Not a reader certificate.** A production Aliro reader carries an X.509
  certificate issued by a CA the user device trusts, sent during
  authentication via `LOAD CERT` or `AUTH1`. Getting one is an ecosystem
  process, not a build step. Until then the reader runs with
  `ESP_ALIRO_CERT_POLICY_NONE`, which only works against a device that has
  been told to trust this reader public key directly.
- **Not a wallet credential.** `credential_pubkey.pem` stands in for a key
  that a real wallet would hold. A phone will not present it. Use it with a
  device emulator such as the CSA `aliro-actuator` reference.

Storing the reader private key in flash as plaintext is a development
shortcut. Production wants flash encryption at minimum, and ideally the key in
an eFuse-backed HMAC/DS peripheral or an external secure element.
