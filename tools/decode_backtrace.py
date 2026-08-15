#!/usr/bin/env python3
"""Turn a panic backtrace back into function names and source lines.

    tools/decode_backtrace.py firmware/aliro_homekey.elf 0x402215a1
    pkill -f monitor; pbpaste | tools/decode_backtrace.py firmware/aliro_homekey.elf

Why this exists rather than xtensa-esp32-elf-addr2line: the toolchain is a
multi-gigabyte install, and the machine that needs to read a crash is usually
the one holding the USB cable, not the one that built the firmware. pyelftools
reads DWARF from any ELF regardless of target architecture, so this works
anywhere Python does.

It matters most on the Matter build, which sets
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_SILENT -- an assert there prints its
address and nothing else, no file, no line, no expression.

Addresses can be given as arguments, or piped in as raw monitor output: the
"Backtrace: 0x400d7e11:0x3ffb7e50 ..." form is understood, and only the PC of
each pair is decoded (the second half is the stack pointer).

Use the ELF from the build that crashed. A different build of the same source
will still answer -- names shift only slightly between builds, so the output
looks entirely reasonable and is quietly wrong. Match the "ELF file SHA256"
the panic prints against the ELF before believing a line number.

Where a line points into a header, that is usually inlining rather than an
error: the table records the innermost inlined location, not the caller.
"""

from __future__ import annotations

import re
import sys
from bisect import bisect_right

from elftools.elf.elffile import ELFFile

ADDRESS = re.compile(r"0x[0-9a-fA-F]{8}")


def load_functions(elf: ELFFile) -> tuple[list[int], list[tuple[int, str]]]:
    """Sorted function start addresses, and (end, name) beside each."""
    functions: list[tuple[int, int, str]] = []
    for section in elf.iter_sections():
        if not hasattr(section, "iter_symbols"):
            continue
        for symbol in section.iter_symbols():
            if symbol["st_info"]["type"] != "STT_FUNC" or not symbol.name:
                continue
            start = symbol["st_value"]
            size = symbol["st_size"] or 1
            functions.append((start, start + size, symbol.name))

    functions.sort()
    return [f[0] for f in functions], [(f[1], f[2]) for f in functions]


def load_lines(elf: ELFFile) -> list[tuple[int, int, list[tuple[int, str, int]]]]:
    """Line rows grouped into sequences: (low, high, [(address, file, line)]).

    A sequence -- not a compilation unit -- is DWARF's unit of contiguous
    code, ended by an end_sequence row whose address is one past the last
    instruction. A unit may contain several, scattered anywhere in the image,
    so a unit's outermost addresses say nothing about what lies between them.

    Two earlier versions of this got it wrong in the same direction: bisecting
    one merged table, then bisecting per unit. Both answer every address
    confidently, and both attributed an ESP-IDF panic to a newlib header. An
    address inside no sequence has no line, and saying so is the point.
    """
    if not elf.has_dwarf_info():
        return []

    sequences: list[tuple[int, int, list[tuple[int, str, int]]]] = []
    dwarf = elf.get_dwarf_info()

    for unit in dwarf.iter_CUs():
        program = dwarf.line_program_for_CU(unit)
        if program is None:
            continue

        header = program.header
        file_entries = header["file_entry"]
        include_dirs = header["include_directory"]
        rows: list[tuple[int, str, int]] = []

        for entry in program.get_entries():
            state = entry.state
            if state is None:
                continue

            if state.end_sequence:
                # Its address is the end of the range, exclusive.
                if rows:
                    rows.sort()
                    sequences.append((rows[0][0], state.address, rows))
                rows = []
                continue

            try:
                file_entry = file_entries[state.file]
            except IndexError:
                continue

            name = file_entry.name
            name = name.decode() if isinstance(name, bytes) else name

            directory = file_entry.get("dir_index", 0)
            if directory and directory < len(include_dirs):
                folder = include_dirs[directory]
                folder = folder.decode() if isinstance(folder, bytes) else folder
                name = f"{folder}/{name}"

            rows.append((state.address, name, state.line))

    sequences.sort()
    return sequences


def source_for(address: int, sequences) -> str:
    """The file:line for an address, or nothing when no sequence covers it."""
    index = bisect_right([sequence[0] for sequence in sequences], address) - 1
    while index >= 0:
        low, high, rows = sequences[index]
        if low <= address < high:
            row = bisect_right([r[0] for r in rows], address) - 1
            if row >= 0:
                _, path, number = rows[row]
                return f"    {path}:{number}"
            return ""
        # Sequences can nest oddly after linker garbage collection; step back
        # over any whose range ends before this address.
        if high <= address:
            return ""
        index -= 1
    return ""


def describe(address: int, starts, spans, units) -> str:
    index = bisect_right(starts, address) - 1
    if index < 0:
        return f"0x{address:08x}  ?"

    end, name = spans[index]
    if address >= end:
        # Inside no known function: usually ROM, which carries no symbols.
        return f"0x{address:08x}  ROM or unmapped"

    offset = address - starts[index]
    return f"0x{address:08x}  {name} +{offset}{source_for(address, units)}"


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2

    path = sys.argv[1]
    text = " ".join(sys.argv[2:])
    if not text and not sys.stdin.isatty():
        text = sys.stdin.read()
    if not text:
        print("no addresses given", file=sys.stderr)
        return 2

    # "0xPC:0xSP" pairs: keep the program counters, drop the stack pointers.
    pairs = re.findall(r"(0x[0-9a-fA-F]{8}):0x[0-9a-fA-F]{8}", text)
    addresses = [int(a, 16) for a in (pairs if pairs else ADDRESS.findall(text))]

    seen: set[int] = set()
    ordered = [a for a in addresses if not (a in seen or seen.add(a))]

    with open(path, "rb") as handle:
        elf = ELFFile(handle)
        starts, spans = load_functions(elf)
        units = load_lines(elf)

        for address in ordered:
            print(describe(address, starts, spans, units))

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
