#!/usr/bin/env python3
"""Disassembles a range of the game executable, with import names and the strings that instructions point at.

What it does: prints `count` instructions from a virtual address (or the whole function containing it, from
private/analysis/functions.json, with --func), naming IAT slots ("call [KERNEL32.dll!Sleep]") and showing the ASCII /
UTF-16 string behind any address operand. For reading the game's code while writing the runtime.
What it does not do: decompile, or follow calls.

usage: .venv/bin/python -I tools/dis.py <address> [count] [--func]
"""
import bisect
import json
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "funcs"))
from capstone import CS_ARCH_X86, CS_MODE_32, Cs  # noqa: E402
from pe32 import DEFAULT_EXE, DEFAULT_GAME, Image  # noqa: E402


def string_at(img, va):
    if img.section_of(va) not in (".rdata", ".data"):
        return None
    b = img.bytes_at(va, 160)
    s = b.split(b"\0")[0]
    if len(s) >= 3 and all(32 <= ch < 127 for ch in s):
        return '"%s"' % s.decode()
    w = b.decode("utf-16-le", "replace").split("\0")[0]
    if len(w) >= 3 and all(32 <= ord(ch) < 127 for ch in w):
        return 'L"%s"' % w
    return None


def main():
    args = [x for x in sys.argv[1:] if not x.startswith("--")]
    va = int(args[0], 16)
    count = int(args[1]) if len(args) > 1 else 40
    img = Image(os.path.join(DEFAULT_GAME, DEFAULT_EXE))
    end = None
    if "--func" in sys.argv:
        fj = json.load(open("private/analysis/functions.json"))["functions"]
        starts = [f["start"] for f in fj]
        i = bisect.bisect_right(starts, va) - 1
        va, end = fj[i]["start"], fj[i]["end"]
        count = 100000
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    code = img.bytes_at(va, (end - va) if end else count * 15)
    n = 0
    for ins in md.disasm(code, va):
        note = []
        for m in re.findall(r"0x[0-9a-f]+", ins.op_str):
            v = int(m, 16)
            if v in img.iat:
                note.append(img.iat[v])
            s = string_at(img, v)
            if s:
                note.append(s)
        print("%08x  %-8s %-44s %s" % (ins.address, ins.mnemonic, ins.op_str, " ; ".join(note)))
        n += 1
        if n >= count or (end and ins.address + ins.size >= end):
            break


if __name__ == "__main__":
    main()
