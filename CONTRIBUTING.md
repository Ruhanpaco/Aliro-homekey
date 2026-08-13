# Contributing

Contributions of every size are welcome: a bug report, a pin table fix, a new
NFC chip driver, or a whole milestone. This project is open so that "an
Apple-only door" is not the only option, and it stays open only if the work
that goes in is easy for the next person to pick up.

Before anything else, read [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and
[docs/ROADMAP.md](docs/ROADMAP.md). They are short, and they say what this
project is and — just as important — what it is not.

## Code of conduct

Be useful, be honest, be kind. There is no formal CoC text yet; until there
is, assume the baseline is: critique the code, not the person, and never ship
a contribution that pretends to do something it does not.

## AI-assisted contributions

AI/LLM tooling is fine to assist with code, docs and this wiki — but the
tool is assisting you, not the other way round. The same bar applies to
machine-written output as to anything else:

- No unnecessary information added.
- No unnecessary dependencies added.
- No changes to unrelated files, or unrelated changes within one file.
- Every claim is verified by a human before the PR is opened — an AI that
  hallucinates a pin table or an SDK call is worse than no help at all.

"AI slop" — output nobody reviewed that pads a PR to look bigger than it is —
is not welcome. If you cannot follow this, an issue written in your own words
is still appreciated.

### Using an AI without making bad code

A model that sounds confident is not the same as a model that is right. Treat
every byte of AI output as a first draft from a stranger who has never seen
your ESP32, and run it through this checklist:

1. **Give it the context.** Point the tool at `docs/ARCHITECTURE.md`,
   `docs/ROADMAP.md` and the component it will touch. "Write a driver" without
   the seams produces something that has to be thrown away. The seams are the
   point of this codebase.
2. **Build it, and run the tests.** Nothing AI-produced is accepted because it
   *looks* right. `idf.py ... build` and `test/host/run.sh` are the floor.
   If you cannot build it, do not submit it.
3. **Verify every API name against the real headers.** Models hallucinate
   function names, enum values, Kconfig symbols and SDK behaviour. Check
   `esp_aliro.h`, the ESP-IDF headers and the datasheet before trusting any
   of them. The safest way to "know" an API is to have compiled a call to it.
4. **Ask it to explain, then check the explanation.** If the model cannot
   tell you why a change is correct — the timing budget, the SPI transaction
   shape, why a key slot is required — the change is not ready. An answer you
   cannot defend to a reviewer is an answer you should not ship.
5. **Beware the confident mistake.** Real failure modes this project has seen
   the tools get wrong: an "input-only" GPIO that can drive an output, a pin
   wired to flash offered in the UI, an SDK callback used before the reader
   is enabled, a pin table for the wrong chip. Every one of these looks
   reasonable in a diff and fails on hardware.
6. **Keep changes small and reviewable.** AI output is easy to trust in bulk
   and hard to review in bulk. One focused change, one PR.
7. **Never let AI write the security-sensitive parts on its own.** Crypto,
   key handling, NVS layout and access decisions get *extra* human review,
   not less. If a reviewer cannot trace the trust boundary of a change, it
   does not land.

### AI output can carry someone else's copyright

Language models can reproduce code they were trained on — including code under
a different licence, from a different author, verbatim. That is why the
attribution rules below apply to AI-generated code exactly as they apply to
code you copied by hand. If the model produces a function that matches an
existing open-source implementation, treat it as derived work: attribute it,
check its licence against Apache-2.0, and add it to [NOTICE.md](NOTICE.md).
"When a model wrote it" is not an answer to a licence question; provenance
is.

## The bar for a contribution

A contribution is "done" when it clears all of these:

1. **It builds.** `idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/sdkconfig.defaults.esp32" build`
   compiles cleanly for the default target.
2. **It does not regress the host tests.** `test/host/run.sh` passes.
3. **It fits the seams.** New NFC chips are one new `nfc_transport_t` file.
   New policy lives in `access_control`. Protocol logic stays in
   `aliro_reader`. If your change touches two components, say why in the PR.
4. **It is honest.** If a feature works for one chip but not another, or only
   on a bench, the docs and the PR description say so. The README has a
   status line for a reason.
5. **It matches the style.** Four-space indent, K&R braces, no trailing
   whitespace, and no comments that restate the code. Follow the
   `SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors` header used in
   the existing files.

## Setting up

```bash
# ESP-IDF 5.2–6.0 and openssl on PATH. See the README's Build section.
idf.py set-target esp32
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/sdkconfig.defaults.esp32" build

# Host tests: validation, JSON patching, secret masking, GPIO rules — no board needed.
test/host/run.sh

# See the configuration UI without a board.
tools/build_ui_preview.py
```

The first build generates a development identity into `main/certs/`. Those
files are gitignored; never commit them. See `main/certs/README.md` for what
the identity is and is not.

## Where to start

The roadmap is the honest list of what is missing, in priority order. Today
the gaps that matter are:

- **Verified build on real hardware.** Nothing in this repo has been compiled
  for an ESP32 yet. Building, flashing, and reporting what `idf.py monitor`
  shows is the highest-value contribution there is.
- **A real NFC frontend** (`docs/ROADMAP.md`, Milestone 2). ST25R3916, PN532
  or PN7160: pick a chip, implement `nfc_transport_t`, and a tap stops being
  a stub. This is the single biggest step the project can take.
- **Credential lifecycle** (Milestone 3): persist credentials in NVS, add and
  revoke them at runtime, log every tap.
- **Web service hardening** (Milestone 5): authentication for the config UI,
  and live log streaming.

If you want to work on something not on the roadmap, open an issue first and
make the case. A door lock has a long tail of maintenance; work that lands on
a whim is work someone else inherits.

## Bugs and security

Open an issue. For something that could let an attacker open a door or read
credential material — and on this project, treat crypto, key handling, NVS
layout, and network-facing code as security-relevant — report it privately to
the maintainers before opening an issue. Do not file a public issue for a
credential leak while the fixed firmware is unpublished.

## Pull requests

1. Branch from `main` with a descriptive name: `feat/st25r3916-driver`,
   `fix/esp32c6-strapping-pins`.
2. Keep the PR small enough to review in one sitting. Split unrelated changes
   into separate PRs.
3. Write a PR description that says what changed, why, and how you verified it
   (which target, what `test/host/run.sh` said, what you saw on the bench).
4. CI runs the host tests and an `esp32`/`esp32s3` build. Make both green.
5. Review comments are a request to talk, not a verdict. Push follow-up
   commits; the history can be squashed on merge.

## Commits

Concise, imperative, and specific — the same style the rest of the repo uses:

```
feat(nfc_transport): add ST25R3916 driver over SPI

Implements init/poll/activate/exchange on the ST25R3916. Verified against
the CSA aliro-actuator emulator; ISO 14443-4 extended APDUs confirmed.
```

## Recognition

Contributors get their name or handle in the release notes, and are invited to
the contributors team after significant, repeated work. A door lock project
lives or dies on who sticks around to maintain what they built; that work gets
named.

## Licensing and attribution

By contributing, you agree to license your contribution under the Apache
License 2.0, matching the rest of the project (see [LICENSE](LICENSE)). Every
file carries an `SPDX-License-Identifier: Apache-2.0` header; keep it there.

Deriving from another project is welcome — this codebase already builds on
[HomeKey-ESP32](https://github.com/rednblkx/HomeKey-ESP32) and Espressif's
Aliro SDK — but attribution is not optional. The rules:

1. **Say where it came from.** If you adapt or copy code, keep its original
   copyright header in the file, and say in the PR exactly what was taken,
   from which project, under which licence. "Inspired by" and "copied from"
   are different claims; be precise.
2. **Update NOTICE.md.** Every third-party work this project uses or derives
   from goes in [NOTICE.md](NOTICE.md): project and URL, copyright line,
   licence, and what/where it is used. If you add or replace a dependency —
   including a component-manager dependency in an `idf_component.yml`, or
   code the AI produced that reproduces an existing implementation — extend
   NOTICE.md in the same PR.
3. **Licence compatibility is the reviewer's gate.** The project is
   Apache-2.0. A change that would make the project distribute someone else's
   code under an incompatible licence (GPL-only, no-licence, proprietary)
   will be asked to find another way. MIT, BSD and Apache-2.0 sources are
   fine with attribution; always keep their notices intact.
4. **No licence means no use.** A project with no `LICENSE` file, or code you
   cannot trace to a licence, cannot be copied into this repository — unless
   it is referenced only for reading, as `kormax/aliro` is.
5. **Dependencies get the same treatment as code.** Adding a library is
   adding someone else's work. Declare it in the component manifest, and list
   it in NOTICE.md with its licence — before, not after, the PR is reviewed.

### Is it a derivative work? Decide before you submit

Copyright protects *expression*, not ideas. A function's purpose — poll the
NFC field, validate a pin, derive a key — was never anyone's property. But
the *specific implementation* — its structure, ordering, choices, the way it
handles edge cases — is the author's expression, and it belongs to them until
it stops being recognisably theirs. That is the whole question in a PR that
started from someone else's code. Run it through this:

| What you did | Is it derived? | What you owe |
| --- | --- | --- |
| Copied the code, changed nothing but the variable names | Yes — obviously | Full attribution (rules 1–2), licence text, header kept |
| Copied the code and rewrote it heavily — new structure, new flow, reshaped to fit these components | **Yes.** Heavy editing is still editing; the expression survived. If you can point at the original and say "this became that," it is derived | Full attribution, and the PR says exactly how far the rewrite went |
| Asked an AI to copy the code and "improve" it for this project | **Yes.** The model is just a faster copyer; it cannot give you rights the original author did not | Exactly the same as the two rows above |
| Read their code, understood the approach, then wrote your own from a blank file | No — you took an idea, which is free to take | Attribution is a courtesy, not an obligation. A "inspired by `project`" note in the PR is still smart, and costs nothing |
| Only the *concept* survives — "poll, authenticate, unlock" | No | Nothing. That was never anyone's property |

The practical test: **if you can point at the original and say "this became
that," attribute it. If you can only say "this gave me the idea," you are
clear.** When in doubt, attribute — on this project the cost is a short
notice and one NOTICE.md line, and a door-lock codebase has zero appetite for
a provenance dispute later.

### Bringing in files from a repo or an SDK

The rules get *heavier* the more of someone else's work you bring in. A
couple of adapted lines — an idiom, a pin number, a constant — needs honest
credit and nothing more. But once a PR adds **a file, several files, or a
substantial portion of a file** from another repo, an SDK, or an AI-assisted
reworking of either, a one-line credit is not enough. The change must ship
with the full package:

- **The licence text, not a link to it.** MIT and BSD require the copyright
  and permission notice to be included in copies and substantial portions.
  Apache-2.0 derivatives must keep the NOTICE. If a licence asks to be
  reproduced, reproduce it — in the file header, the NOTICE.md entry, or
  both. A URL is not compliance.
- **A NOTICE.md entry that names the files.** Not just "we use project X" —
  *which* files or portions came over, from which project, under which
  licence, and whether they were modified. Compare the existing entries for
  `esp-aliro` and `HomeKey-ESP32`: each lists exactly where the work lives
  in this tree.
- **The original copyright header stays on the file.** If a file is largely
  derived, keep the upstream copyright line in it. Add yours below, and note
  the derivation in a comment. Removing an upstream copyright header to make
  it look original is a hard reject.
- **The PR says how it was changed.** "Vendored `dns_hijack.c` and reduced it
  to a single-query responder" tells a reviewer what to check. "Adapted the
  driver" tells them nothing.
- **Same treatment for AI-modified copies.** If you asked an AI to take
  someone's file and improve it, the result is still a derivative work with
  the same obligations — provenance, licence text, NOTICE.md, headers. The
  model did not remove the original author's rights; it cannot, and neither
  can you.
