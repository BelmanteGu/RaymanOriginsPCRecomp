"""Switch-table recovery for MSVC 2010 x86-32 code (used by discover.py; later by the lifter).

MSVC emits two shapes for a `switch`:
  * direct:   cmp R, N ; ja default ; jmp dword ptr [R*4 + TABLE]                 (N+1 dword targets)
  * indexed:  cmp R, N ; ja default ; movzx R2, byte ptr [R + INDEX] ;
              jmp dword ptr [R2*4 + TABLE]                                        (N+1 byte indices into TABLE)
TABLE holds absolute code addresses (no relocations needed: the image has a fixed base). The tables sit in .text right
after the function's code, which is why linear sweep shows "undecodable" bytes there.

What it does: from the instructions of the block that ends in the indirect jump, finds the table, its entry count and
the targets. When the bound (`cmp`) is not found, entries are read while they point into .text (capped).
What it does not do: decide the targets are right; the lifter keeps a range check and traps on an unknown target.
"""
from capstone import x86 as X

MAX_ENTRIES = 1024


class Table:
    def __init__(self, jump_at, table, count, targets, index_table=None, bounded=True):
        self.jump_at = jump_at
        self.table = table
        self.count = count
        self.targets = targets
        self.index_table = index_table
        self.bounded = bounded

    def data_ranges(self):
        """The byte ranges of .text that are table data, not code."""
        out = [(self.table, self.table + 4 * self.count)]
        if self.index_table is not None:
            out.append((self.index_table, self.index_table + self.index_count))
        return out


def _bound(recent, reg_names):
    """N from the last `cmp R, N` (or `and R, 2^k-1`) on one of the index registers before the jump
    (entries = N + 1)."""
    for ins in reversed(recent[:-1]):
        ops = ins.operands
        if ins.mnemonic in ("cmp", "and") and len(ops) == 2 and ops[0].type == X.X86_OP_REG and \
                ops[1].type == X.X86_OP_IMM and ins.reg_name(ops[0].reg) in reg_names:
            v = ops[1].imm & 0xFFFFFFFF
            if ins.mnemonic == "cmp" or (v & (v + 1)) == 0:
                return v
    return None


def _family(name):
    """The 32/16/8-bit names of one register, so `cmp cl, 5` bounds an index held in ecx."""
    fam = {"eax": ("eax", "ax", "al"), "ebx": ("ebx", "bx", "bl"), "ecx": ("ecx", "cx", "cl"), "edx": ("edx", "dx", "dl"),
           "esi": ("esi", "si"), "edi": ("edi", "di"), "ebp": ("ebp", "bp"), "esp": ("esp", "sp")}
    return fam.get(name, (name,))


def find_table(img, recent):
    """recent: the decoded instructions of the block, the last one being `jmp dword ptr [R*4 + disp]`. None if not a table."""
    j = recent[-1]
    if j.mnemonic != "jmp" or not j.operands or j.operands[0].type != X.X86_OP_MEM:
        return None
    m = j.operands[0].mem
    if m.base != 0 or m.index == 0 or m.scale != 4 or m.segment != 0:
        return None
    table = m.disp & 0xFFFFFFFF
    if img.section_of(table) is None:
        return None
    idx_reg = j.reg_name(m.index)

    index_table, index_src = None, None
    for ins in reversed(recent[:-1]):
        ops = ins.operands
        if ins.mnemonic == "movzx" and len(ops) == 2 and ops[0].type == X.X86_OP_REG and \
                ins.reg_name(ops[0].reg) == idx_reg and ops[1].type == X.X86_OP_MEM and ops[1].size == 1:
            mm = ops[1].mem
            if img.section_of(mm.disp & 0xFFFFFFFF) is None:
                break                                    # `movzx eax, byte [esi]`: data, not an index table
            if mm.base != 0 and mm.index == 0:
                index_table, index_src = mm.disp & 0xFFFFFFFF, ins.reg_name(mm.base)
            elif mm.index != 0 and mm.base == 0 and mm.scale == 1:
                index_table, index_src = mm.disp & 0xFFFFFFFF, ins.reg_name(mm.index)
            break
        if idx_reg in {ins.reg_name(r) for r in ins.regs_access()[1]}:
            break                                        # the index was computed by something else: a direct table

    if index_table is not None:
        n = _bound(recent, _family(index_src))
        if n is None or n >= 4096:
            return None
        idx = [img.u8(index_table + i) for i in range(n + 1)]
        count = max(idx) + 1
        t = Table(j.address, table, count, [], index_table, True)
        t.index_count = n + 1
    else:
        n = _bound(recent, _family(idx_reg))
        bounded = n is not None and n < MAX_ENTRIES
        count = n + 1 if bounded else MAX_ENTRIES
        t = Table(j.address, table, count, [], None, bounded)

    targets = []
    for i in range(t.count):
        v = img.u32(table + 4 * i)
        if v is None or not img.in_text(v):
            if t.bounded:
                continue     # an entry the code can never select (e.g. index 0 of memcpy's alignment table)
            break
        targets.append(v)
    if not t.bounded:
        t.count = len(targets)
        for i in range(1, 17):                       # no bound: `neg ecx; jmp [ecx*4+T]` indexes backwards too
            v = img.u32(table - 4 * i)
            if v is None or not img.in_text(v):
                break
            targets.append(v)
    if not targets:
        return None
    t.targets = targets
    return t
