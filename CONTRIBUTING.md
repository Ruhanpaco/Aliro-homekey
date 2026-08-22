# Contributing to Aliro HomeKey

Thanks for your interest. Contributions of every size are welcome — a bug
report, a pin table fix, a driver for another NFC frontend, or a wallet nobody
has tried yet. This project exists so that "an Apple-only door" is not the only
option, and it stays useful only if the work going in is easy for the next
person to pick up.

> [!NOTE]
>
> **AI/LLM guidelines**
>
> Using an AI to help write code, docs or the site is fine, as long as:
>
> - No unnecessary information is added
> - No unnecessary dependencies are added
> - The PR contains no changes to unrelated files, and no unrelated changes
>   within one file — keep it to the purpose of the PR
> - Every claim is checked by a human before the PR is opened
>
> All of that implies human review. The tool is assisting you, not the other
> way round. "AI slop" — output nobody read, padding a PR to look bigger than
> it is — will not be merged.
>
> This matters more here than in most projects, because a model that has never
> seen your board will happily invent a pin table, an SDK call or a register
> that does not exist, and the result is firmware for a door. If you cannot
> follow this, an issue in your own words is still genuinely useful.

## How to contribute

### Getting started

1. **Fork the repository** on GitHub

2. **Clone your fork**:

   ```bash
   git clone https://github.com/your-username/Aliro-homekey.git
   cd Aliro-homekey
   ```

3. **Read the two short documents** that say what this project is, and what it
   is not: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and
   [docs/ROADMAP.md](docs/ROADMAP.md).

### Development setup

1. **Install ESP-IDF** 5.2 or newer — see the
   [ESP-IDF getting started guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/index.html).
   The Aliro SDK arrives through the component manager; there are no
   submodules to initialise.

2. **Build the plain firmware**:

   ```bash
   idf.py set-target esp32
   idf.py build
   ```

3. **Build with Matter** — only if you are touching `components/matter_lock`.
   It needs [esp-matter](https://github.com/espressif/esp-matter), which is a
   2 GB checkout with its own submodules, so most changes do not:

   ```bash
   . $ESP_MATTER_PATH/export.sh
   idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.matter" build
   ```

4. **Flash and watch it boot**:

   ```bash
   idf.py -p /dev/ttyUSB0 flash monitor
   ```

   [docs/FIRST-TEST.md](docs/FIRST-TEST.md) describes what a healthy boot looks
   like and what each step actually proves.

### Checks you can run without a board

Most mistakes are catchable before a build, and this runs in seconds:

```bash
python3 tools/check_consistency.py   # Kconfig, dependency and wiring errors
```

The web UI is a single hand-written file. To look at it in a browser without
flashing anything:

```bash
python3 tools/build_ui_preview.py
```

### Making changes

1. **Create a branch**:

   ```bash
   git checkout -b feature/your-feature-name
   ```

2. **Make the change**, keeping it to one thing. A PR that fixes a bug and
   reformats a file is two PRs.

3. **Test it.** If it touches hardware behaviour, say in the PR which board you
   ran it on and what you saw. "Should work" is not a test result, and this
   project has been wrong often enough to earn that rule.

4. **Open the PR** describing what changed and why. Logs and serial output are
   welcome — they are usually the fastest way to show a thing works.

## Reporting and fixing a bug

This project has been burned before by fixing the wrong thing: a fix built on
a guess instead of a real log, shipped, then found broken by the next report.
So bugs go through the same order every time, including when the report comes
up in conversation rather than as a GitHub issue:

1. **Gather full repro information before diagnosing anything.** Board and
   NFC frontend, firmware version or commit, exact steps, and — for anything
   Matter, Wi-Fi, or NFC-timing related — a fresh boot log that actually
   covers the failure, not a description of it. A log from a different bug is
   worse than no log; it looks like evidence and isn't.
2. **File a GitHub issue** with the symptom and whatever's been diagnosed so
   far from that evidence, even if the fix is already obvious. This is what
   keeps a record of what actually broke and why, separate from the fix
   itself — useful when a "fixed" bug comes back, which has happened here
   more than once (see the ECP beacon in the git log).
3. **Fix it**, with the commit or PR referencing the issue (`Fixes #N` /
   `Closes #N`) so the two stay linked and the issue closes automatically
   when the fix lands on `main`.

Skipping straight to a fix without an issue is fine for something trivial and
self-contained (a typo, an off-by-one with no behavioral ambiguity). Anything
where the root cause took real investigation gets the issue, so the next
person — human or AI — hitting something similar can find it instead of
re-diagnosing from nothing.

## Coding standards

- **C for the components, C++ only where a dependency forces it.** The Matter
  glue is C++ because esp-matter is; nothing else needs to be.
- **Keep the seams.** `nfc_transport`, `aliro_reader`, `access_control`,
  `app_config`, `net_manager` and `web_server` each own one job. New work
  belongs behind an existing seam or in a new component, not spread across
  three.
- **Comments explain why, not what.** The code already says what it does. A
  comment earns its place by recording the thing that is not obvious — the
  errata, the ordering that turned out to be load-bearing, the API that lies.
- **Errors are handled or reported, never swallowed.** A function that returns
  `ESP_OK` after something failed is worse than one that crashes.
- **No new dependency without a reason** you can state in one sentence.

## Licensing

By contributing you agree your work is licensed under **Apache-2.0**, matching
the project and `esp_aliro_lib`.

If you adapt code from another project, say so in the PR, keep the original
copyright line in the file, and add the project to [NOTICE.md](NOTICE.md).
Parts of this codebase are adapted from MIT-licensed work and that attribution
is a licence condition, not a courtesy — see NOTICE.md for what and where.

## Where to start

- **A tap that fails.** Serial output from a refused or misread credential is
  the most useful bug report this project can receive.
- **Another NFC frontend.** PN532 works; PN5180, ST25R3916 and RC522 do not
  exist yet. `nfc_transport` is the seam to implement.
- **Another wallet.** Only Apple has been tested. Whether a Google or Samsung
  credential provisions and taps is genuinely unknown.
- **Anything marked unknown in [docs/ROADMAP.md](docs/ROADMAP.md).** The list is
  kept honest on purpose.
