#!/usr/bin/env python3
"""Check the browser flasher's NVS partition writer against ESP-IDF's format.

site/index.html builds an NVS image in JavaScript so that a browser can
provision a reader identity without a toolchain. Getting a CRC or a span wrong
there does not fail loudly -- nvs_flash_init() reports a corrupt partition, the
device quietly falls back to defaults, and the provisioned identity vanishes.

So the image is decoded here by a second, independent implementation, and every
checksum is verified with zlib.crc32(data, 0xFFFFFFFF): the exact call ESP-IDF's
own nvs_partition_gen.py makes. Requires node and nothing else.
"""

import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
PAGE_SIZE, ENTRY_SIZE, ENTRIES_PER_PAGE = 4096, 32, 126
PAGE_ACTIVE, PAGE_FULL, PAGE_EMPTY = 0xFFFFFFFE, 0xFFFFFFFC, 0xFFFFFFFF
TYPE_U8, TYPE_STR = 0x01, 0x21

failures = []


def check(condition, message):
    if not condition:
        failures.append(message)


def crc(data):
    """What nvs_partition_gen.py computes: CRC-32 starting at 0, inverted."""
    return zlib.crc32(data, 0xFFFFFFFF) & 0xFFFFFFFF


def extract_writer():
    """Pull the NVS section out of the single-file flasher."""
    html = (ROOT / "site" / "index.html").read_text()
    start = html.index("const PAGE_SIZE = 4096")
    end = html.index("* 2. Identity generation")
    body = html[start:end]
    body = body[: body.rindex("/* ===")]
    return body + "\nexport { buildNvsImage };\n"


def build(items, size=0xC000):
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        (tmp / "nvs.mjs").write_text(extract_writer())
        (tmp / "gen.mjs").write_text(
            "import { buildNvsImage } from './nvs.mjs';\n"
            f"const items = {json.dumps(items)};\n"
            f"process.stdout.write(Buffer.from(buildNvsImage({size}, "
            '[{ name: "aliro", items }])));\n'
        )
        result = subprocess.run(
            ["node", str(tmp / "gen.mjs")], capture_output=True, check=True
        )
        return result.stdout


def decode(image):
    """Independent reader. Returns (page_states, {key: value})."""
    states, values = [], {}

    for page_no in range(len(image) // PAGE_SIZE):
        page = image[page_no * PAGE_SIZE : (page_no + 1) * PAGE_SIZE]
        state, seq = struct.unpack("<II", page[0:8])

        if state == PAGE_EMPTY:
            check(page == b"\xff" * PAGE_SIZE,
                  f"page {page_no} is uninitialised but not erased")
            continue

        check(state in (PAGE_ACTIVE, PAGE_FULL), f"page {page_no} state {state:#x}")
        check(page[8] == 0xFE, f"page {page_no} is not format version 2")
        check(seq == page_no, f"page {page_no} sequence number is {seq}")
        check(crc(page[4:28]) == struct.unpack("<I", page[28:32])[0],
              f"page {page_no} header CRC")
        states.append("ACTIVE" if state == PAGE_ACTIVE else "FULL")

        bitmap, entry = page[32:64], 0
        while entry < ENTRIES_PER_PAGE:
            bit = entry * 2
            if (bitmap[bit // 8] >> (bit % 8)) & 0b11 == 0b11:
                break  # empty: the rest of the page is unused

            off = 64 + entry * ENTRY_SIZE
            e = page[off : off + ENTRY_SIZE]
            span, item_type = e[2], e[1]
            key = e[8:24].split(b"\x00")[0].decode()

            check(crc(e[0:4] + e[8:32]) == struct.unpack("<I", e[4:8])[0],
                  f"entry CRC for '{key}'")
            check(len(key) < 16, f"key '{key}' exceeds the 15-byte field")
            check(e[3] == 0xFF, f"'{key}' has a chunk index but is not a blob")

            if item_type == TYPE_U8:
                check(e[0] == 0, "a u8 appeared outside the namespace table")
                check(span == 1, f"namespace '{key}' spans {span} entries")
            elif item_type == TYPE_STR:
                length = struct.unpack("<H", e[24:26])[0]
                data_at = 64 + (entry + 1) * ENTRY_SIZE
                data = page[data_at : data_at + length]
                check(crc(data) == struct.unpack("<I", e[28:32])[0],
                      f"data CRC for '{key}'")
                check(span == 1 + -(-length // ENTRY_SIZE),
                      f"'{key}' span {span} is wrong for {length} bytes")
                check(data.endswith(b"\x00"), f"'{key}' is not NUL-terminated")
                values[key] = data[:-1].decode()
            else:
                failures.append(f"'{key}' has unexpected type {item_type:#x}")

            entry += span

    return states, values


def main():
    pem = "-----BEGIN PUBLIC KEY-----\n" + "A" * 88 + "\n-----END PUBLIC KEY-----\n"

    print("one page, the shape the flasher actually writes")
    states, values = decode(build([
        {"key": "config", "value": '{"device":{"name":"aliro-reader"}}'},
        {"key": "rdr_pub", "value": pem},
        {"key": "rdr_priv", "value": pem},
        {"key": "cred_pub", "value": pem},
        {"key": "serial", "value": "A1B2C3"},
    ]))
    check(states == ["ACTIVE"], f"expected one active page, got {states}")
    check(values.get("rdr_pub") == pem, "reader public key did not round-trip")
    check(values.get("serial") == "A1B2C3", "serial did not round-trip")
    check(set(values) == {"config", "rdr_pub", "rdr_priv", "cred_pub", "serial"},
          f"unexpected key set {sorted(values)}")

    print("spilling onto a second page")
    big = [{"key": f"blob{i}", "value": "X" * 1000} for i in range(6)]
    states, values = decode(build(big))
    check(states == ["FULL", "ACTIVE"],
          f"a sealed page and an open one expected, got {states}")
    check(len(values) == 6, f"{len(values)} of 6 items survived the spill")
    check(all(v == "X" * 1000 for v in values.values()), "spilled data corrupted")

    print("a value too large for any page is refused")
    try:
        build([{"key": "huge", "value": "X" * 5000}])
        failures.append("a 5000-byte value was accepted; it cannot fit a page")
    except subprocess.CalledProcessError as err:
        check("cannot fit one NVS page" in err.stderr.decode(),
              f"rejected for the wrong reason:\n{err.stderr.decode().strip()}")

    print()
    if failures:
        for f in failures:
            print(f"FAIL: {f}")
        return 1
    print("nvs image: format, CRCs and round-trip all verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
