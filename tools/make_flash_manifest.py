#!/usr/bin/env python3
"""Build an ESP Web Tools manifest from what the build actually produced.

    tools/make_flash_manifest.py <site-dir> <target>...

Each <target> is expected at <site-dir>/<target>/ containing the binaries and
the flasher_args.json the build wrote. Offsets come from that file rather than
from memory, because they differ per chip — the bootloader sits at 0x1000 on
an ESP32 and at 0x0 on an ESP32-S3, and getting it wrong produces a board that
boot-loops with an invalid header.
"""

import json
import pathlib
import sys

# ESP Web Tools identifies chips by these exact strings.
CHIP_FAMILIES = {
    "esp32": "ESP32",
    "esp32s2": "ESP32-S2",
    "esp32s3": "ESP32-S3",
    "esp32c3": "ESP32-C3",
    "esp32c6": "ESP32-C6",
    "esp32h2": "ESP32-H2",
}


def build_for(site: pathlib.Path, target: str) -> dict:
    args = json.loads((site / target / "flasher_args.json").read_text())

    parts = []
    for offset, filename in sorted(args["flash_files"].items(), key=lambda kv: int(kv[0], 16)):
        parts.append({
            "path": f"{target}/{pathlib.Path(filename).name}",
            "offset": int(offset, 16),
        })

    return {"chipFamily": CHIP_FAMILIES[target], "parts": parts}


def main() -> None:
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)

    site = pathlib.Path(sys.argv[1])
    targets = sys.argv[2:]

    builds = []
    for target in targets:
        if target not in CHIP_FAMILIES:
            raise SystemExit(f"unknown target '{target}'")
        if not (site / target / "flasher_args.json").exists():
            print(f"skipping {target}: no flasher_args.json", file=sys.stderr)
            continue
        builds.append(build_for(site, target))

    if not builds:
        raise SystemExit("no builds found")

    manifest = {
        "name": "Aliro HomeKey",
        "version": (site / "VERSION").read_text().strip() if (site / "VERSION").exists() else "dev",
        # A first flash should start from a clean NVS; an upgrade should not,
        # or the reader loses its config and its persisted reader identity.
        "new_install_prompt_erase": True,
        "builds": builds,
    }

    out = site / "manifest.json"
    out.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"wrote {out}")
    for build in builds:
        offsets = ", ".join(f"0x{p['offset']:x} {pathlib.Path(p['path']).name}" for p in build["parts"])
        print(f"  {build['chipFamily']}: {offsets}")


if __name__ == "__main__":
    main()
