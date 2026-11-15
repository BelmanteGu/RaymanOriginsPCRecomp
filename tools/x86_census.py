#!/usr/bin/env python3
"""Instruction census of SSFIV.exe's .text: the numbers that size the x86-32 -> arm64 recompiler.

What it does: linear-sweep disassembly (capstone) of the whole .text section; counts x87 / SSE / MMX instructions,
direct call targets (function candidates), indirect calls and jumps, and jump-table style `jmp [reg*4+table]`.
What it does not do: find function boundaries properly (linear sweep also decodes data between functions; see the
function-discovery tool for that). The numbers are an upper/lower bound sketch, not the final function list.

usage: .venv/bin/python -I tools/x86_census.py <path/SSFIV.exe>
"""
import sys
from collections import Counter

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_GRP_JUMP, CS_GRP_CALL
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

pe = pefile.PE(sys.argv[1], fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
code = text.get_data()[: text.Misc_VirtualSize]
start = base + text.VirtualAddress
end = start + len(code)

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True
md.skipdata = True

groups = Counter()
mnems = Counter()
call_targets = set()
ind_jmp = ind_call = 0
jmp_tables = 0
n = 0

for ins in md.disasm(code, start):
    n += 1
    m = ins.mnemonic
    if m == ".byte":
        groups["data/undecodable"] += 1
        continue
    mnems[m] += 1
    regs = [md.reg_name(r) for r in ins.regs_read + ins.regs_write]
    if m.startswith("f") and m not in ("fs",):
        groups["x87"] += 1
    elif any(r and r.startswith("xmm") for r in regs) or "xmm" in ins.op_str:
        groups["sse"] += 1
    elif "mm" in ins.op_str:
        groups["mmx"] += 1
    if ins.group(CS_GRP_CALL) or ins.group(CS_GRP_JUMP):
        op = ins.operands[0] if ins.operands else None
        if op is not None and op.type == X86_OP_IMM:
            if m == "call" and start <= op.imm < end:
                call_targets.add(op.imm)
        elif m == "call":
            ind_call += 1
        elif m == "jmp":
            ind_jmp += 1
            if op is not None and op.type == X86_OP_MEM and op.mem.index != 0 and op.mem.scale == 4:
                jmp_tables += 1

print(f"instructions:          {n}")
print(f"direct call targets:   {len(call_targets)} (function candidates)")
print(f"indirect calls:        {ind_call}")
print(f"indirect jumps:        {ind_jmp}  (jump-table form: {jmp_tables})")
for k, v in groups.most_common():
    print(f"{k:22} {v}")
print("\nmost common x87 mnemonics:")
for k, v in [(k, v) for k, v in mnems.most_common() if k.startswith("f")][:15]:
    print(f"  {k:10} {v}")
print("\nmost common SSE mnemonics:")
for k, v in [(k, v) for k, v in mnems.most_common() if k.endswith(("ss", "sd", "ps", "pd")) or k.startswith(("cvt", "movq", "movd", "pxor", "movaps", "movups"))][:15]:
    print(f"  {k:10} {v}")
