#!/usr/bin/env python3
"""Compress a file for embedding in the firmware.

    tools/gzip_file.py <source> <destination.gz>

Used by components/web_server/CMakeLists.txt. A separate script rather than an
inline `python -c`, because a one-liner long enough to do this trips CMake's
argument parsing.

mtime is pinned to zero so identical input always produces identical output,
which keeps the application image reproducible.
"""

import gzip
import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2

    src = pathlib.Path(sys.argv[1])
    dst = pathlib.Path(sys.argv[2])
    data = src.read_bytes()
    dst.write_bytes(gzip.compress(data, 9, mtime=0))

    saved = 100 - (dst.stat().st_size * 100 // len(data)) if data else 0
    print(f"{src.name}: {len(data)} -> {dst.stat().st_size} bytes ({saved}% smaller)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
