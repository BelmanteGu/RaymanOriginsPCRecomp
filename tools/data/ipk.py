#!/usr/bin/env python3
"""Lists or extracts the files of a UbiArt IPK archive (bundle_PC.ipk), for studying the game's data.

What it does: reads the directory (version 3: big-endian; per entry: part count, size, compressed size, timestamp,
offsets, UTF-16BE path) and prints `size zsize path`, or extracts the entries whose path contains a filter into
private/ (zlib entries are inflated). It only reads the archive.
What it does not do: write archives or know the formats inside them.

usage: .venv/bin/python -I tools/data/ipk.py <file.ipk> [--grep text] [--extract <outdir>]
"""
import argparse
import struct
import zlib


def entries(f):
    head = f.read(0x30)
    assert struct.unpack(">I", head[:4])[0] == 0x50EC12BA, "not an IPK"
    base = struct.unpack(">I", head[12:16])[0]
    f.seek(0)
    d = f.read(base)
    count = struct.unpack(">I", d[0x2C:0x30])[0]
    p = 0x30
    for _ in range(count):
        parts, size, zsize = struct.unpack(">III", d[p:p + 12])
        p += 20
        offs = struct.unpack(">%dQ" % parts, d[p:p + 8 * parts])
        p += 8 * parts
        n = struct.unpack(">I", d[p:p + 4])[0]
        p += 4
        name = d[p:p + 2 * n].decode("utf-16-be")
        p += 2 * n
        yield name, size, zsize, base + offs[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ipk")
    ap.add_argument("--grep")
    ap.add_argument("--extract")
    a = ap.parse_args()
    with open(a.ipk, "rb") as f:
        for name, size, zsize, off in list(entries(f)):
            if a.grep and a.grep.lower() not in name.lower():
                continue
            print("%10d %10d %s" % (size, zsize, name))
            if a.extract:
                import os
                f.seek(off)
                raw = f.read(zsize or size)
                data = zlib.decompress(raw) if zsize else raw
                out = os.path.join(a.extract, name)
                os.makedirs(os.path.dirname(out), exist_ok=True)
                open(out, "wb").write(data)


if __name__ == "__main__":
    main()
