#!/usr/bin/env python3
"""Lists or extracts the entries of an Ultima VII Flex archive (an 80-byte
title, the magic 0xFFFF1A00, the entry count, then from 0x80 a table of
{offset, size} pairs, 32-bit little-endian; empty entries have size 0).

Usage: flexx.py FILE [OUTDIR [EXT]]   no OUTDIR: list; else writes
OUTDIR/NN.EXT (NN the entry number, EXT default "bin")."""
import hashlib
import os
import struct
import sys


def entries(path):
    d = open(path, "rb").read()
    magic, count = struct.unpack_from("<II", d, 80)
    if magic != 0xFFFF1A00:
        sys.exit(f"{path}: not a Flex file")
    for i in range(count):
        off, size = struct.unpack_from("<II", d, 0x80 + 8 * i)
        yield i, d[off:off + size] if size else b""


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    ext = sys.argv[3] if len(sys.argv) > 3 else "bin"
    for i, data in entries(sys.argv[1]):
        if len(sys.argv) < 3:
            if data:
                print(f"{i:3d} {len(data):7d} {data[:4]} {hashlib.md5(data).hexdigest()[:8]}")
        elif data:
            os.makedirs(sys.argv[2], exist_ok=True)
            open(os.path.join(sys.argv[2], f"{i:02d}.{ext}"), "wb").write(data)


main()
