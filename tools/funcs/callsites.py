#!/usr/bin/env python3
"""Argument bytes pushed before every indirect call in Rayman Origins.exe (for the runtime's COM / C++ objects).

Why: the runtime answers methods of objects the game gets from it (Steam interfaces, Direct3D 9, DirectInput,
DirectSound). Those methods pop their own arguments (stdcall / thiscall), and the runtime must pop exactly what the
caller pushed or the game's stack breaks. Instead of a hand table for every method of every interface version, this
tool measures it at each call site: inside a basic block, `push` adds 4, `pop` removes 4, `sub esp, n` / `add esp, n`
add / remove n, and a direct call or a jump target starts the count again. The value at `call [reg+N]` is the number of
argument bytes (for COM this includes the `this` pointer pushed as the first argument).
What it does: writes private/analysis/callsite_args.bin, pairs of little-endian uint32 (return address, bytes),
sorted, and prints how many sites were measured. The runtime warns when a call site is missing from it.
What it does not do: see pushes made before a branch into the block (the count would be low; rare in MSVC code).

usage: .venv/bin/python -I tools/funcs/callsites.py [--analysis private/analysis]
"""
import argparse
import collections
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capstone import CS_ARCH_X86, CS_MODE_32, Cs  # noqa: E402
from capstone import x86 as X  # noqa: E402

from descent import Descent  # noqa: E402
from pe32 import DEFAULT_EXE, DEFAULT_GAME, Image  # noqa: E402

BRANCH = {"jmp", "jo", "jno", "jb", "jae", "je", "jne", "jbe", "ja", "js", "jns", "jp", "jnp", "jl", "jge", "jle", "jg",
          "loop", "jecxz"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=DEFAULT_GAME)
    ap.add_argument("--analysis", default="private/analysis")
    ap.add_argument("--exe", default=DEFAULT_EXE)
    a = ap.parse_args()
    img = Image(os.path.join(a.game, a.exe))
    data = json.load(open(os.path.join(a.analysis, "functions.json")))
    starts = {f["start"] for f in data["functions"]}
    d = Descent(img)
    d.noreturn = set(data.get("noreturn", []))
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    sites = {}
    hist = collections.Counter()
    for s in sorted(starts):
        f = d.run(s, starts.__contains__, img.text_end)
        addrs = sorted(f.insns)
        targets = set()
        dec = []
        for ad in addrs:
            ins = next(md.disasm(img.bytes_at(ad, 16), ad, 1), None)
            if ins is None:
                break
            dec.append(ins)
            if ins.mnemonic in BRANCH and ins.operands and ins.operands[0].type == X.X86_OP_IMM:
                targets.add(ins.operands[0].imm & 0xFFFFFFFF)
        for t in f.tables:
            targets.update(t.targets)
        # the first push of a callee-saved register that the function pops again is a register save, not an argument
        popped = {ins.reg_name(ins.operands[0].reg) for ins in dec if ins.mnemonic == "pop" and ins.operands
                  and ins.operands[0].type == X.X86_OP_REG}
        saves, seen = set(), set()
        for ins in dec:
            if ins.mnemonic == "push" and ins.operands and ins.operands[0].type == X.X86_OP_REG:
                r = ins.reg_name(ins.operands[0].reg)
                if r in ("ebx", "esi", "edi", "ebp") and r in popped and r not in seen:
                    saves.add(ins.address)
                seen.add(ins.operands[0].type == X.X86_OP_REG and ins.reg_name(ins.operands[0].reg))
        depth, prev_end = 0, None
        for ins in dec:
            if ins.address in targets or ins.address != prev_end:
                depth = 0
            prev_end = ins.address + ins.size
            m, ops = ins.mnemonic, ins.operands
            if m == "push":
                depth += 0 if ins.address in saves else 4
            elif m == "pop":
                depth = max(0, depth - 4)
            elif m in ("sub", "add") and ops[0].type == X.X86_OP_REG and ins.reg_name(ops[0].reg) == "esp" \
                    and ops[1].type == X.X86_OP_IMM:
                n = ops[1].imm & 0xFFFFFFFF
                n = n - (1 << 32) if n & 0x80000000 else n
                depth = depth + n if m == "sub" else max(0, depth - n)
                if depth > 256:
                    depth = 0                      # a stack frame, not arguments
            elif m == "call":
                if ops[0].type != X.X86_OP_IMM:
                    sites[ins.address + ins.size] = depth
                    hist[min(depth, 40)] += 1
                depth = 0
            elif m in ("mov", "lea") and ops[0].type == X.X86_OP_REG and ins.reg_name(ops[0].reg) == "esp":
                depth = 0
    out = os.path.join(a.analysis, "callsite_args.bin")
    with open(out, "wb") as fh:
        for ra in sorted(sites):
            fh.write(struct.pack("<II", ra, sites[ra]))
    print("indirect call sites measured: %d -> %s" % (len(sites), out))
    print("argument bytes: " + ", ".join("%d:%d" % kv for kv in sorted(hist.items())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
