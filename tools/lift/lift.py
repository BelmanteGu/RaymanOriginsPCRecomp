#!/usr/bin/env python3
"""The x86-32 -> C lifter (M4): every function of Rayman Origins.exe as a C function `void f_<addr>(CPU *c)`.

What it does: reads private/analysis/functions.json (tools/funcs/discover.py), walks each function again with the final
list of starts, translates every instruction (emit_int / emit_x87 / emit_sse) and its control flow:
  * jumps inside the function are `goto`; a switch table is a C `switch` on the address read from the table;
  * a direct call pushes the return address on the guest stack and calls the lifted function; registers live in C
    locals and are written back to CPU around calls (SYNC) and read again after (LOAD);
  * `ret n` pops the return address and n bytes and returns; a jump to another function is a tail call;
  * indirect calls and jumps go through usf_icall (the runtime's address -> function table, imports included).
A function with an instruction not handled yet becomes a stub that stops with its address and the reason, and is
counted in the census. Output (game-derived, so in private/): lifted_NNN.c files, lifted.h, lifted_table.c.
What it does not do: run anything, or check the result (tools/lift/difftest does that against the original code).

usage: .venv/bin/python -I tools/lift/lift.py [--out private/lifted] [--only 0x401000,...] [--per-file 400]
"""
import argparse
import collections
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "funcs"))
from capstone import CS_ARCH_X86, CS_MODE_32, Cs  # noqa: E402
from capstone import x86 as X  # noqa: E402

import emit_int  # noqa: E402
import emit_sse  # noqa: E402
import emit_x87  # noqa: E402
from descent import Descent  # noqa: E402
from ops import Ops, Unsupported, hexu  # noqa: E402
from pe32 import DEFAULT_EXE, DEFAULT_GAME, Image  # noqa: E402

REGS = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi")
FLAGS = ("fk", "fr", "fa", "fb")
# registers and lazy flags go to CPU around calls and returns; loads nobody uses are removed by clang
SYNC = " ".join("c->%s = %s;" % (r, r) for r in REGS + FLAGS)
LOAD = " ".join("%s = c->%s;" % (r, r) for r in REGS + FLAGS)
JCC = {"jo", "jno", "jb", "jae", "je", "jne", "jbe", "ja", "js", "jns", "jp", "jnp", "jl", "jge", "jle", "jg"}
HEADER = '#include "usf/cpu.h"\n#include "usf/flags.h"\n#include "usf/x87.h"\n#include "lifted.h"\n'

# UbiArt fits its picture, cameras and screen rectangles to 16:9 with integer code: "lea r, [x + x*8]; shr r, 4" (x * 9 / 16)
# next to the back buffer's width ([+0x10A0]), about a hundred times (found by scanning the lifted code: the instruction
# after such a lea, within 14 instructions of a load of the width; plus the three of the viewport function at 0x7D98C1,
# which loads the width before). On a screen narrower than 16:9 (tablets: 5:3, 16:10, 4:3) that made a 16:9 band with
# black bars: the scene's viewport, a render target made every frame and two StretchRects to centre it. With
# usf_fill_screen (d3d9_device.c apply_aspect) each of them uses the screen's own shape instead of 16:9:
# x * usf_fit_h / usf_fit_w, so the picture fills the screen, as the 16/9 float pair already does for the camera.
# USF_LETTERBOX=1 keeps the game's bars. The register is the shr's destination; its input is x * 9 (exact).
FIT_SITES = (
    0x40FFAF, 0x41196C, 0x4B84CF, 0x4B84DF, 0x4B859F, 0x4B8678, 0x4B9CA9, 0x4B9CC9,
    0x4B9D50, 0x4BA108, 0x4BABA2, 0x4BACB1, 0x4BB13D, 0x4BB1C9, 0x4BB24C, 0x4BB268,
    0x4BB2F2, 0x4BB561, 0x4BB6A1, 0x4BB76C, 0x4BBA2D, 0x4BC0A1, 0x4BC36C, 0x4BF90B,
    0x4BF941, 0x4C223C, 0x4C2450, 0x4C26D0, 0x4C44D9, 0x4C50D5, 0x4C56B2, 0x4C5836,
    0x51E3C2, 0x54ADEA, 0x54B350, 0x5605F3, 0x560607, 0x561F91, 0x561FA5, 0x5C3F6D,
    0x5C4D63, 0x5C525D, 0x6585BA, 0x67B9F3, 0x67C1E5, 0x67C1F5, 0x67D0C3, 0x67D0D7,
    0x697239, 0x69724A, 0x697A6F, 0x698C04, 0x6D7700, 0x6F0EA3, 0x6F112E, 0x6F1141,
    0x6F196A, 0x6F197A, 0x6F1D5F, 0x7322EF, 0x7322FF, 0x74E644, 0x756A5D, 0x7744DB,
    0x7A1572, 0x7B96B6, 0x7B9791, 0x7B9F4A, 0x7D63FE, 0x7D98ED, 0x7D9901, 0x7D9943,
    0x7D9957, 0x7DA5CE, 0x7DA6AE, 0x7DAE6C, 0x7DAE80, 0x7DFBA7, 0x7E05EB, 0x7E05FF,
    0x7FF540, 0x7FF550, 0x7FFA65, 0x83B93B, 0x85C8CD, 0x85CA25, 0x85CA3B, 0x86CDE9,
    0x86D500, 0x879846, 0x87985A, 0x87F004, 0x87F036, 0x87F47E, 0x87F48E,
)
GAME_PATCHES = {a: "%s = usf_fill_screen ? (uint32_t)((uint64_t)(%s / 9u) * usf_fit_h / usf_fit_w) : %s >> 4; " \
                   "fk = FK(FK_SHR, 4); fr = %s; fa = %s; fb = 4;" for a in FIT_SITES}


def fname(a):
    return "f_%08X" % a


class FuncLifter:
    def __init__(self, img, md, f, starts, noreturn):
        self.img, self.md, self.f = img, md, f
        self.starts, self.noreturn = starts, noreturn
        self.tables = {t.jump_at: t for t in f.tables}
        self.partial = []

    def decode(self, a):
        off = a - self.img.text_start
        for ins in self.md.disasm(self.img.mem[a - self.img.base: a - self.img.base + 16], a, 1):
            return ins
        raise Unsupported("undecodable at %08X" % a)

    def tail(self, t):
        return "{ %s %s(c); return; }" % (SYNC, fname(t))

    def goto(self, t):
        if t in self.f.insns:
            return "goto L_%08X;" % t
        if t in self.starts:
            return self.tail(t)
        return "usf_unknown(c, %s, \"jump outside the function\");" % hexu(t)

    def control(self, ins, o):
        """C for a control-flow instruction, or None if it is not one."""
        m, a, nxt = ins.mnemonic, ins.address, ins.address + ins.size
        ops = ins.operands
        if m in JCC:
            return ["if (%s) %s" % (emit_int.cond(m[1:]), self.goto(ops[0].imm & 0xFFFFFFFF))]
        if m in ("loop", "loope", "loopne", "jecxz"):
            t = self.goto(ops[0].imm & 0xFFFFFFFF)
            if m == "jecxz":
                return ["if (!ecx) %s" % t]
            extra = {"loop": "", "loope": " && CC_E(fk, fr, fa, fb)", "loopne": " && !CC_E(fk, fr, fa, fb)"}[m]
            return ["ecx--; if (ecx%s) %s" % (extra, t)]
        if m == "jmp":
            op = ops[0]
            if op.type == X.X86_OP_IMM:
                return [self.goto(op.imm & 0xFFFFFFFF)]
            tab = self.tables.get(a)
            target = o.read(op, 4)
            if tab is not None:
                tg = set(tab.targets)
                if not tab.bounded:                          # no known bound: the index can also be negative
                    tg |= {x for x in (self.img.u32(tab.table + k) for k in range(-64, 0, 4)) if x is not None}
                cases = " ".join("case %s: %s" % (hexu(t), self.goto(t)) for t in sorted(tg)
                                 if t in self.f.insns or t in self.starts)   # a target that is a start: tail call
                return ["{ uint32_t t = %s; switch (t) { %s default: %s usf_icall(c, t); return; } }"
                        % (target, cases, SYNC)]
            m_ = op.mem if op.type == X.X86_OP_MEM else None
            if m_ is not None and m_.base == 0 and m_.index != 0 and m_.scale == 4 and \
                    self.img.in_text(m_.disp & 0xFFFFFFFF):
                # an unrecognised table in .text (e.g. a negative index): every code entry near it is a case,
                # anything else goes through usf_icall
                base = m_.disp & 0xFFFFFFFF
                near = {self.img.u32(base + k) for k in range(-64, 260, 4)}
                cases = " ".join("case %s: %s" % (hexu(t), self.goto(t)) for t in sorted(x for x in near if x is not None)
                                 if t in self.f.insns or t in self.starts)
                if cases:
                    return ["{ uint32_t t = %s; switch (t) { %s default: %s usf_icall(c, t); return; } }"
                            % (target, cases, SYNC)]
            return ["{ uint32_t t = %s; %s usf_icall(c, t); return; }" % (target, SYNC)]
        if m == "call":
            op = ops[0]
            push = "esp -= 4u; W32(esp, %s);" % hexu(nxt)
            if op.type == X.X86_OP_IMM:
                t = op.imm & 0xFFFFFFFF
                if t == nxt:                                        # call $+5: pushes its own address (get EIP)
                    return [push]
                after = " usf_unknown(c, %s, \"returned from a no-return call\");" % hexu(nxt) \
                    if t in self.noreturn else ""
                if t in self.starts:
                    return ["%s %s %s(c); %s%s" % (push, SYNC, fname(t), LOAD, after)]
                return ["%s %s usf_icall(c, %s); %s%s" % (push, SYNC, hexu(t), LOAD, after)]
            return ["{ uint32_t t = %s; %s %s usf_icall(c, t); %s }" % (o.read(op, 4), push, SYNC, LOAD)]
        if m in ("ret", "retn", "repz ret", "rep ret", "repz retn"):
            n = ops[0].imm if ops else 0
            return ["esp += %du; %s return;" % (4 + n, SYNC)]
        return None

    def lift(self):
        f = self.f
        if not f.insns:
            raise Unsupported("empty function")
        addrs = sorted(f.insns)
        decoded = [self.decode(a) for a in addrs]
        targets = {f.start}
        for ins in decoded:
            if ins.mnemonic in JCC or ins.mnemonic in ("jmp", "loop", "loope", "loopne", "jecxz"):
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM:
                    targets.add(op.imm & 0xFFFFFFFF)
        for t in f.tables:
            targets.update(t.targets)
            if not t.bounded:
                targets.update(self.img.u32(t.table + k) for k in range(-64, 0, 4))
        for ins in decoded:                                  # cases of the guarded switch for unrecognised tables
            if ins.mnemonic == "jmp" and ins.operands[0].type == X.X86_OP_MEM and ins.address not in self.tables:
                m_ = ins.operands[0].mem
                if m_.base == 0 and m_.index != 0 and m_.scale == 4 and self.img.in_text(m_.disp & 0xFFFFFFFF):
                    targets.update(self.img.u32((m_.disp & 0xFFFFFFFF) + k) for k in range(-64, 260, 4))
        for i, ins in enumerate(decoded):                    # fall-through to a non-adjacent address needs a label
            nxt = ins.address + ins.size
            if nxt in f.insns and (i + 1 >= len(decoded) or decoded[i + 1].address != nxt):
                targets.add(nxt)
        body = []
        for i, ins in enumerate(decoded):
            o = Ops(ins)
            if ins.address in targets:
                body.append("L_%08X:" % ins.address)
            try:
                stmts = self.control(ins, o)
                terminal = stmts is not None and ins.mnemonic in ("jmp", "ret", "retn", "repz ret", "rep ret", "repz retn")
                if stmts is None:
                    for em in (emit_int, emit_x87, emit_sse):
                        stmts = em.emit(ins, o)
                        if stmts is not None:
                            break
                    if stmts is None:
                        raise Unsupported(ins.mnemonic)
                    if ins.mnemonic in ("int3", "ud2", "hlt"):
                        terminal = True
            except Unsupported as e:
                # only this instruction stops (with its address); the function's other paths stay lifted
                # (e.g. Crypto++ keeps AES-NI and plain paths in one function and picks one by CPUID)
                self.partial.append(str(e))
                stmts = ["usf_unknown(c, %s, \"not lifted: %s\");" % (hexu(ins.address), str(e).replace('"', "'"))]
                terminal = True
            if ins.address in GAME_PATCHES:
                assert ins.mnemonic == "shr", "game patch at %08X: %s %s" % (ins.address, ins.mnemonic, ins.op_str)
                reg = ins.op_str.split(",")[0].strip()
                stmts = ["/* port patch */ { extern int usf_fill_screen; extern unsigned usf_fit_w, usf_fit_h; "
                         + GAME_PATCHES[ins.address].replace("%s", reg) + " }"]
            if ins.mnemonic == "call" and ins.operands[0].type == X.X86_OP_IMM and \
                    (ins.operands[0].imm & 0xFFFFFFFF) in self.noreturn:
                terminal = True
            body.append("    /* %08X %s %s */ " % (ins.address, ins.mnemonic, ins.op_str) + " ".join(stmts))
            if not terminal:
                nxt = ins.address + ins.size
                if i + 1 >= len(decoded) or decoded[i + 1].address != nxt:
                    if nxt in f.insns:
                        body.append("    goto L_%08X;" % nxt)
                    elif nxt in self.starts:
                        body.append("    " + self.tail(nxt))
                    else:
                        body.append("    usf_unknown(c, %s, \"ran off the end of the function\");" % hexu(nxt))
        head = ["void %s(CPU *c) {" % fname(f.start),
                "    uint32_t " + ", ".join("%s = c->%s" % (r, r) for r in REGS) + ";",
                "    uint32_t fk = c->fk, fr = c->fr, fa = c->fa, fb = c->fb;",
                "    (void)fk; (void)fr; (void)fa; (void)fb;"]
        if addrs[0] != f.start:
            head.append("    goto L_%08X;" % f.start)
        return "\n".join(head + body + ["}"]) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=DEFAULT_GAME)
    ap.add_argument("--analysis", default="private/analysis")
    ap.add_argument("--out", default="private/lifted")
    ap.add_argument("--only", help="comma-separated function addresses")
    ap.add_argument("--bucket", type=lambda x: int(x, 0), default=0x10000,
                    help="bytes of .text per output file: a file's functions only change when its range does")
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--table", default="usf_table", help="symbol of the address table (one per module)")
    a = ap.parse_args()
    t0 = time.time()
    img = Image(os.path.join(a.game, a.exe))
    data = json.load(open(os.path.join(a.analysis, "functions.json")))
    starts = {f["start"] for f in data["functions"]}
    noreturn = set(data.get("noreturn", []))
    d = Descent(img)
    d.noreturn = noreturn
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    todo = sorted(starts)
    if a.only:
        todo = [int(x, 16) for x in a.only.split(",")]
    nexts = sorted(starts)
    import bisect
    os.makedirs(a.out, exist_ok=True)
    reasons, lifted, failed = collections.Counter(), 0, []
    buckets = collections.defaultdict(list)
    for s in todo:
        cur = buckets[(s - img.text_start) // a.bucket]
        i = bisect.bisect_right(nexts, s)
        f = d.run(s, starts.__contains__, img.text_end)   # stop only AT other starts: a jump over a false start stays inside
        try:
            fl = FuncLifter(img, md, f, starts, noreturn)
            cur.append(fl.lift())
            lifted += 1
            for why in fl.partial:
                reasons["(one instruction) " + why] += 1
        except (Unsupported, KeyError, IndexError, ValueError, TypeError, AttributeError) as e:
            why = str(e) if isinstance(e, Unsupported) else "lifter error: %s %s" % (type(e).__name__, e)
            reasons[why] += 1
            failed.append((s, why))
            cur.append("void %s(CPU *c) { usf_unknown(c, %s, \"not lifted: %s\"); }\n" % (
                fname(s), hexu(s), why.replace('"', "'")))
    chunks = buckets
    names = set()
    for n, ch in sorted(buckets.items()):          # written only when changed: the build recompiles only those
        path = os.path.join(a.out, "lifted_%03d.c" % n)
        names.add(os.path.basename(path))
        text = HEADER + "\n".join(ch)
        if not os.path.exists(path) or open(path).read() != text:
            with open(path, "w") as fh:
                fh.write(text)
    for old in os.listdir(a.out):                  # files of an earlier layout would define functions twice
        if old.startswith("lifted_") and old[7:10].isdigit() and old.endswith(".c") and old not in names:
            os.remove(os.path.join(a.out, old))
    header = "#pragma once\n#include \"usf/cpu.h\"\n" + "".join("void %s(CPU *c);\n" % fname(s) for s in sorted(starts))
    hp = os.path.join(a.out, "lifted.h")
    if not os.path.exists(hp) or open(hp).read() != header:
        with open(hp, "w") as fh:
            fh.write(header)
    with open(os.path.join(a.out, "lifted_table.c"), "w") as fh:
        fh.write('#include "lifted.h"\n#include "usf/table.h"\nconst USFEntry %s[] = {\n' % a.table)
        fh.write("".join("    {%s, %s},\n" % (hexu(s), fname(s)) for s in sorted(starts)))
        fh.write("};\nconst unsigned %s_count = %d;\n" % (a.table, len(starts)))
    with open(os.path.join(a.out, "census.json"), "w") as fh:
        json.dump({"lifted": lifted, "failed": len(failed), "reasons": reasons.most_common(),
                   "failed_list": [[hex(s), w] for s, w in failed]}, fh, indent=1)
    total = lifted + len(failed)
    print("functions: %d, lifted %d (%.2f%%), stubs %d; %d files in %s; %.0f s" % (
        total, lifted, 100.0 * lifted / max(total, 1), len(failed), len(chunks), a.out, time.time() - t0))
    print("not handled yet (functions):")
    for why, n in reasons.most_common(25):
        print("  %6d  %s" % (n, why))
    return 0


if __name__ == "__main__":
    sys.exit(main())
