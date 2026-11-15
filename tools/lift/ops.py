"""Operands of an x86-32 instruction as C expressions, for the lifter (M4).

What it does: turns a capstone operand into a C read expression and a C write statement over the lifted function's
locals (eax..edi, 8/16-bit parts included) and guest memory (R8/R16/R32 and W8/W16/W32 from runtime/include/usf/cpu.h),
with fs-relative addresses going through the TEB base. Values are uint32_t in C; sizes are carried separately.
What it does not do: translate instructions (emit_*.py) or decide control flow (lift.py).
"""
from capstone import x86 as X


class Unsupported(Exception):
    """The instruction (or one of its operands) is not handled yet; the function is reported, not lifted."""


REG32 = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi")
SUB = {
    "al": ("eax", 0, 8), "ah": ("eax", 8, 8), "ax": ("eax", 0, 16),
    "cl": ("ecx", 0, 8), "ch": ("ecx", 8, 8), "cx": ("ecx", 0, 16),
    "dl": ("edx", 0, 8), "dh": ("edx", 8, 8), "dx": ("edx", 0, 16),
    "bl": ("ebx", 0, 8), "bh": ("ebx", 8, 8), "bx": ("ebx", 0, 16),
    "sp": ("esp", 0, 16), "bp": ("ebp", 0, 16), "si": ("esi", 0, 16), "di": ("edi", 0, 16),
}
for r in REG32:
    SUB[r] = (r, 0, 32)

LOAD = {1: "R8", 2: "R16", 4: "R32", 8: "R64"}
STORE = {1: "W8", 2: "W16", 4: "W32", 8: "W64"}


def hexu(v):
    return "0x%Xu" % (v & 0xFFFFFFFF)


class Ops:
    def __init__(self, ins):
        self.ins = ins

    def regname(self, r):
        return self.ins.reg_name(r)

    def addr(self, op):
        """The guest address of a memory operand, as a uint32_t C expression."""
        m = op.mem
        parts = []
        if m.base != 0:
            parts.append(self.read_reg(self.regname(m.base)))
        if m.index != 0:
            idx = self.read_reg(self.regname(m.index))
            parts.append(idx if m.scale == 1 else "%s * %du" % (idx, m.scale))
        if m.disp != 0 or not parts:
            parts.append(hexu(m.disp))
        if m.segment != 0:
            seg = self.regname(m.segment)
            if seg == "fs":
                parts.append("c->fs_base")
            elif seg not in ("ds", "ss", "es", "cs"):
                raise Unsupported("segment " + seg)
        return "(uint32_t)(%s)" % " + ".join(parts)

    def read_reg(self, name):
        if name not in SUB:
            raise Unsupported("register " + name)
        full, shift, bits = SUB[name]
        if bits == 32:
            return full
        if shift:
            return "((%s >> %d) & 0xFFu)" % (full, shift)
        return "(%s & 0x%Xu)" % (full, (1 << bits) - 1)

    def write_reg(self, name, val):
        if name not in SUB:
            raise Unsupported("register " + name)
        full, shift, bits = SUB[name]
        if bits == 32:
            return "%s = (uint32_t)(%s);" % (full, val)
        mask = ((1 << bits) - 1) << shift
        return "%s = (%s & 0x%Xu) | (((uint32_t)(%s) & 0x%Xu) << %d);" % (
            full, full, ~mask & 0xFFFFFFFF, val, (1 << bits) - 1, shift)

    def read(self, op, size=None):
        """Value of an integer operand (register, memory or immediate), zero-extended to uint32_t."""
        size = size or op.size
        if op.type == X.X86_OP_REG:
            return self.read_reg(self.regname(op.reg))
        if op.type == X.X86_OP_IMM:
            return hexu(op.imm & ((1 << (8 * size)) - 1) if size < 4 else op.imm)
        if op.type == X.X86_OP_MEM:
            if size not in LOAD or size == 8:
                raise Unsupported("memory read of %d bytes" % size)
            return "%s(%s)" % (LOAD[size], self.addr(op))
        raise Unsupported("operand type")

    def write(self, op, val, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            return self.write_reg(self.regname(op.reg), val)
        if op.type == X.X86_OP_MEM:
            if size not in STORE or size == 8:
                raise Unsupported("memory write of %d bytes" % size)
            return "%s(%s, %s);" % (STORE[size], self.addr(op), val)
        raise Unsupported("write to operand type")

    def signed(self, val, size):
        """The value sign-extended from `size` bytes, as int32_t."""
        return {1: "(int32_t)(int8_t)(%s)", 2: "(int32_t)(int16_t)(%s)", 4: "(int32_t)(%s)"}[size] % val

    def is_reg(self, op, name=None):
        return op.type == X.X86_OP_REG and (name is None or self.regname(op.reg) == name)

    def xmm(self, op):
        """Index of an xmm register operand."""
        n = self.regname(op.reg)
        if not n.startswith("xmm"):
            raise Unsupported("not an xmm register: " + n)
        return int(n[3:])
