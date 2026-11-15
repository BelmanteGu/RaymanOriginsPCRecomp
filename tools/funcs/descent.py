"""Recursive descent over one x86-32 function (used by discover.py).

What it does: starting at a function's first instruction, follows every path (fall-through, conditional and
unconditional jumps, switch tables) and stops each path at a `ret`, a trap (`int3`, `ud2`, `hlt`), a jump to another
function's start (a tail call), a call to a function that never returns, or the next known function start (a path that
runs into it is recorded as an overrun). It records the instructions it decoded, the calls it saw, the code addresses
used as values (`push offset f`, `mov [x], offset f`: callbacks and vtables), and the switch tables.
What it does not do: decide which addresses are function starts (discover.py does) or translate anything.
"""
from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from capstone import x86 as X

from jumptables import find_table

CHUNK = 256
NORETURN_IMPORTS = {"KERNEL32.dll!ExitProcess", "KERNEL32.dll!ExitThread", "KERNEL32.dll!FatalAppExitA",
                    "KERNEL32.dll!FatalAppExitW"}   # not RaiseException: handled exceptions return (SetThreadName)
TRAPS = {"int3", "ud2", "hlt"}
COND = {"jo", "jno", "jb", "jae", "je", "jne", "jbe", "ja", "js", "jns", "jp", "jnp", "jl", "jge", "jle", "jg",
        "jcxz", "jecxz", "loop", "loope", "loopne"}


class Func:
    __slots__ = ("start", "insns", "end", "returns", "calls", "tails", "code_refs", "tables", "overruns",
                 "indirect_tail", "bad", "traps", "noreturn_calls", "spans", "flows_into")

    def __init__(self, start):
        self.start = start
        self.insns = {}          # address -> size
        self.end = start
        self.returns = False
        self.calls = set()
        self.tails = set()
        self.code_refs = set()
        self.tables = []
        self.overruns = 0
        self.indirect_tail = 0
        self.bad = False
        self.traps = 0           # paths ending in int3 / ud2 / hlt
        self.noreturn_calls = 0  # paths ending in a call that does not return
        self.spans = set()       # other starts that fall INSIDE one of our instructions (so they are not starts)
        self.flows_into = set()  # other starts reached by plain fall-through (not after a call) or by a jcc

    def never_returns(self):
        """Every path ends in a trap or a call that does not return, and none returns, tail-calls or is unknown."""
        return not (self.returns or self.tails or self.indirect_tail or self.overruns or self.bad) and \
            (self.traps or self.noreturn_calls) > 0


class Descent:
    def __init__(self, img):
        self.img = img
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)
        self.md.detail = True
        self.text = img.text_bytes()
        self.noreturn = set()    # function starts known never to return

    def _decode_from(self, addr):
        off = addr - self.img.text_start
        return self.md.disasm(self.text[off: off + CHUNK], addr)

    def _value_refs(self, ins, f):
        for op in ins.operands:
            v = None
            if op.type == X.X86_OP_IMM:
                v = op.imm & 0xFFFFFFFF
            elif op.type == X.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
                v = op.mem.disp & 0xFFFFFFFF
            if v is not None and self.img.in_text(v):
                f.code_refs.add(v)

    def run(self, start, is_start, limit):
        """is_start(a) -> True when a is another function's start; limit = the next known start above `start`."""
        img, f = self.img, Func(start)
        work = [start]
        while work:
            addr = work.pop()
            if addr in f.insns:
                continue
            recent = []
            stop = False
            while not stop:
                if addr in f.insns:
                    break
                if addr != start and is_start(addr):
                    f.overruns += 1          # fell through into the next function
                    break
                if addr >= limit or not img.in_text(addr):
                    f.overruns += 1
                    break
                progressed = False
                for ins in self._decode_from(addr):
                    progressed = True
                    if ins.address != start and (ins.address in f.insns or is_start(ins.address)):
                        if ins.address not in f.insns:
                            f.overruns += 1  # fell through into the next function
                            if not recent or recent[-1].mnemonic != "call":
                                f.flows_into.add(ins.address)   # ordinary flow: that "start" is our own label
                        stop = True
                        break
                    if ins.address >= limit:
                        f.overruns += 1
                        stop = True
                        break
                    if ins.address + ins.size > limit:
                        f.spans.add(limit)   # the "next start" is inside this instruction
                        f.overruns += 1
                        stop = True
                        break
                    f.insns[addr] = ins.size
                    recent.append(ins)
                    if len(recent) > 12:
                        recent.pop(0)
                    m = ins.mnemonic
                    nxt = addr + ins.size
                    if m in ("ret", "retn", "repz ret", "rep ret", "repz retn"):   # `rep ret`: AMD branch hint form
                        f.returns = True
                        stop = True
                    elif m in ("retf", "iret", "iretd", "int1", "into", "in", "out", "insb", "insd", "outsb", "outsd"):
                        f.bad = True         # privileged / far control flow: not user-mode game code
                        stop = True
                    elif m in TRAPS:
                        f.traps += 1
                        stop = True
                    elif m == "jmp":
                        op = ins.operands[0]
                        if op.type == X.X86_OP_IMM:
                            t = op.imm & 0xFFFFFFFF
                            if t != start and (is_start(t) or (img.in_text(t - 1) and img.u8(t - 1) == 0xCC)):
                                f.tails.add(t)           # a known start, or code right after int3 padding: a tail call
                            elif img.in_text(t):
                                work.append(t)
                        else:
                            tab = find_table(img, recent)
                            if tab is not None:
                                f.tables.append(tab)
                                work.extend(tab.targets)
                            else:
                                f.indirect_tail += 1   # `jmp [IAT]` thunk or indirect tail call
                        stop = True
                    elif m in COND:
                        t = ins.operands[0].imm & 0xFFFFFFFF
                        if t != start and is_start(t):
                            f.tails.add(t)
                            f.flows_into.add(t)          # MSVC 2010 does not tail-call through a jcc
                        elif img.in_text(t):
                            work.append(t)
                    elif m == "call":
                        op = ins.operands[0]
                        if op.type == X.X86_OP_IMM:
                            t = op.imm & 0xFFFFFFFF
                            if img.in_text(t):
                                f.calls.add(t)
                                if t in self.noreturn:
                                    f.noreturn_calls += 1
                                    stop = True
                        elif op.type == X.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
                            if img.iat.get(op.mem.disp & 0xFFFFFFFF) in NORETURN_IMPORTS:
                                f.noreturn_calls += 1
                                stop = True
                            else:
                                self._value_refs(ins, f)
                    else:
                        self._value_refs(ins, f)
                    addr = nxt
                    if stop:
                        break
                if not progressed:
                    f.bad = True     # bytes that do not decode on a path we were sent to
                    break
                if not stop and addr in f.insns:
                    break
        if f.insns:
            last = max(f.insns)
            f.end = last + f.insns[last]
        return f
