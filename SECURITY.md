# Security Policy

Aliro HomeKey is beta firmware for a **physical door lock**. A security bug
here isn't an abstract data-exposure risk — it can mean someone gets through
a door they shouldn't. Please treat reports accordingly, and please still
report them: this project has no security budget and no dedicated review,
so an outside report is often the only way a real issue gets found.

## Supported versions

Only the **latest release** is supported. This is fast-moving beta software
— there's no LTS branch, and fixes land on `main` and the next tagged
release, not backported to older ones. If you're not on the latest release,
update before reporting; the bug may already be fixed.

## Reporting a vulnerability

**Preferred: [GitHub private vulnerability reporting](https://github.com/Ruhanpaco/Aliro-homekey/security/advisories/new)**
(Security tab → "Report a vulnerability"). It's private by default, keeps the
discussion attached to the repo, and can turn directly into a security
advisory once it's fixed.

If you'd rather not use GitHub, email **hi@ruhanpacolli.online**. Include:

- What the issue is and why it's exploitable
- Steps to reproduce, or a PoC if you have one
- Which build/target you tested (esp32, esp32c3, esp32s3; plain or Matter)

**Please don't open a public issue for a vulnerability** before it's fixed —
this hands attackers a working exploit against anyone still running the
affected firmware.

There's no SLA here — this is one person's spare-time project — but reports
will be acknowledged and worked, not ignored. If you don't hear back in a
couple of weeks, following up is fair.

## What's in scope

- Anything that lets an unauthorized credential open the lock
- Anything that corrupts or bypasses the Aliro credential/key-slot check
- Anything that lets a network attacker reconfigure the device, pull
  credentials or keys out of it, or trigger unlock without going through the
  actual access decision
- Buffer overflows, memory corruption, or other flaws in the NFC frame
  parsing (`components/pn532`) or Aliro transaction handling
  (`components/aliro_reader`) reachable from an NFC tap or the network
- OTA image handling — anything that lets an unsigned or unauthorized image
  onto the device

## What's already known, and out of scope as a "new" report

This project is deliberately upfront about its current limitations rather
than pretending they don't exist — see the README's disclaimer and
[docs/ROADMAP.md](docs/ROADMAP.md). These are known, not hidden:

- **Not certified.** No CSA, Apple, Google, or Samsung certification. Do not
  treat this as equivalent to a certified commercial lock.
- **Matter builds use esp-matter's test attestation credentials**, not a real
  device attestation chain. Apple Home and others commission it past an
  "uncertified accessory" warning. That's expected today, not a bug to
  report.
- **The web UI's authentication is off by default.** When disabled, anyone on
  the same network can reconfigure the device or, if MQTT is enabled, send an
  unlock command. Turning auth on is a configuration choice available today,
  not something this firmware forces — reports about the *existence* of that
  default aren't new information, but a way to **bypass auth once it's
  turned on** absolutely is in scope.
- **The setup access point's default password (`aliro1234`) is public,**
  same as it is in this repo's docs. It's meant to be changed on first
  configuration, same as a router's default admin password.
- **The development reader identity generated under `main/certs/` is for
  bench testing only** — it's gitignored specifically so it never becomes a
  shared secret in a fork, and it should never be flashed to a lock guarding
  anything real.

If you find a way to defeat one of these *despite* the stated mitigation
(auth turned on, password changed, a real provisioned identity in use),
that's a real report — file it.

## Credit

Reporters who want it are credited in the fix's release notes and/or the
GitHub security advisory. Say in your report if you'd rather stay anonymous.
