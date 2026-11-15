#!/usr/bin/env python3
"""Audit of a function list (private/analysis/functions.json): the places where a real function is likely missing.

What it does: counts (1) unclaimed bytes right after int3/nop padding that decode cleanly from there (a function
start the discovery left out), and (2) targets of vtable-like pointer runs in .rdata that are not function starts and
land on an instruction boundary or in unclaimed bytes. Prints the counts and the first addresses of each, so a
discovery change can be checked without running the game.
What it does not do: decide; a listed address is a candidate.

usage: .venv/bin/python -I tools/funcs/audit.py [--analysis private/analysis] [--exe Rayman Origins.exe]
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from descent import Descent  # noqa: E402
from pe32 import DEFAULT_EXE, DEFAULT_GAME, Image  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--analysis", default="private/analysis")
    ap.add_argument("--exe", default=DEFAULT_EXE)
    a = ap.parse_args()
    img = Image(os.path.join(DEFAULT_GAME, a.exe))
    data = json.load(open(os.path.join(a.analysis, "functions.json")))
    starts = {f["start"] for f in data["functions"]}
    own = open(os.path.join(a.analysis, "text_owner.bin"), "rb").read()
    t0 = img.text_start
    d = Descent(img)
    d.noreturn = set(data.get("noreturn", []))
    after_pad = []
    for i in range(1, len(own)):
        if own[i] == 0 and own[i - 1] == 3:
            f = d.run(t0 + i, starts.__contains__, img.text_end)
            if f.insns and not f.bad and (f.returns or f.tails):
                after_pad.append(t0 + i)
    lo, hi = img.sections[".rdata"]
    vt = set()
    for x in range(lo, hi - 3, 4):
        v = img.u32(x)
        if v and img.in_text(v) and v not in starts and own[v - t0] == 0 and \
                any(img.in_text(img.u32(x + k) or 0) for k in (-4, 4)):
            f = d.run(v, starts.__contains__, img.text_end)
            if f.insns and not f.bad and (f.returns or f.tails):
                vt.add(v)
    print("functions: %d" % len(starts))
    print("unclaimed clean code right after padding: %d %s" % (len(after_pad), [hex(x) for x in after_pad[:12]]))
    print("vtable targets in unclaimed clean code:   %d %s" % (len(vt), [hex(x) for x in sorted(vt)[:12]]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
