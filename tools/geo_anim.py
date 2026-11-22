#!/usr/bin/env python3
"""Turns a USF_GEO_DUMP file (runtime/src/d3d9/dxvk_geodump.c) into an animation the launcher mock-up plays from the
game's atlases: for each frame, each draw's Bezier patches (the control points and corner colours the game's
default_PCT_Patch_VS reads from c17 / c145) and its matrix, so tools/launcher_mock.html evaluates the same patches.

What it does: groups the records by frame, finds the loop (the first frame whose patches come back exactly L frames
later), keeps the frames before it (the entrance) and one loop, and writes private/launcher/<name>.json.gz (30 frames a second, positions in thousandths).
usage: python3 -I tools/geo_anim.py [--draws N] <geo.bin> <name> <hash>=<atlas name> ...   (a Python with numpy)
"""
import collections
import json
import os
import struct
import sys

import numpy as np

NREG = 212


def records(path):
    d = open(path, "rb").read()
    p = 0
    while p + 28 + NREG * 16 <= len(d):
        magic, frame, h, typ, vn, ni, stride = struct.unpack_from("<7I", d, p)
        p += 28
        if magic != 0x4F4547:
            break
        c = np.frombuffer(d, "<f4", NREG * 4, p).reshape(NREG, 4)
        p += NREG * 16
        if p + vn * stride + 2 * ni > len(d):
            break
        zs = {int(round(struct.unpack_from("<f", d, p + i * stride + 8)[0])) for i in range(vn)}
        p += vn * stride + 2 * ni
        yield frame, h, c, max(zs) + 1 if zs else 0


def main():
    args = sys.argv[1:]
    ndraws = 0
    if args[0] == "--draws":                              # keep only frames with this many draws (a settled screen)
        ndraws, args = int(args[1]), args[2:]
    path, name, maps = args[0], args[1], dict(a.split("=") for a in args[2:])
    byf = collections.defaultdict(list)
    for frame, h, c, npatch in records(path):
        key = "%08x" % h
        if key in maps:
            byf[frame].append((maps[key], c, npatch))
    frames = sorted(byf)[:-1]                              # (the last one may be cut)
    if ndraws:
        frames = [f for f in frames if len(byf[f]) == ndraws and abs(byf[f][0][1][1][3]) < 1e-6]   # and in place (c1.w = 0)
    sig = {f: np.concatenate([c[17:17 + 8 * n].ravel() for _, c, n in byf[f]]) for f in frames}
    start, loop = frames[0], 0
    for f in frames:
        for lag in range(30, 600):
            g = f + lag
            if g in sig and sig[g].shape == sig[f].shape and np.array_equal(sig[g], sig[f]):
                start, loop = f, lag
                break
        if loop:
            break
    keep = [f for f in frames if f < start + loop][::2]   # 30 frames a second
    out = {"loop_from": 0, "fps": 30, "frames": []}
    for f in keep:
        draws = []
        for atlas, c, n in byf[f]:
            pts = (c[17:17 + 8 * n].ravel() * 1000).round().astype(int).tolist()          # thousandths
            col = c[145:145 + 4 * n].ravel()
            col = 1 if np.all(col == 1) else (col * 100).round().astype(int).tolist()  # hundredths, 1 = all white
            m = (c[0:4].ravel() * 100000).round().astype(int).tolist()                    # the matrix, 1e-5
            draws.append([atlas, m, (c[13] * 100).round().astype(int).tolist(), n, pts, col])
        out["frames"].append(draws)
    os.makedirs("private/launcher", exist_ok=True)
    dst = "private/launcher/%s.json" % name
    import gzip
    open(dst + ".gz", "wb").write(gzip.compress(json.dumps(out, separators=(",", ":")).encode(), 9))
    dst += ".gz"
    print(dst, len(keep), "frames, loop of", loop, "from frame", out["loop_from"], os.path.getsize(dst) // 1024, "KB")


if __name__ == "__main__":
    main()
