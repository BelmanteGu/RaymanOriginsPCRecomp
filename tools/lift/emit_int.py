"""Integer x86-32 instructions to C, for the lifter (M4).

What it does: one function, emit(ins, o), that returns the C statements of an integer instruction (data movement,
arithmetic, logic, shifts, multiply/divide, setcc/cmov, string operations, atomics) or None when the instruction is not
an integer one (the x87/SSE emitters get it next). Flags go through the lazy locals fk/fr/fa/fb (runtime/include/usf/
flags.h). Raises ops.Unsupported for a form not handled yet.
What it does not do: control flow (jcc, jmp, call, ret, loop: lift.py), floating point (emit_sse.py, emit_x87.py).
"""
from ops import Unsupported, hexu

CONDS = ("o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g")
ALIASES = {"c": "b", "nae": "b", "nb": "ae", "nc": "ae", "z": "e", "nz": "ne", "na": "be", "nbe": "a", "pe": "p",
           "po": "np", "nge": "l", "nl": "ge", "ng": "le", "nle": "g"}
STRING_OPS = {p + s for p in ("movs", "stos", "scas", "cmps", "lods") for s in "bwd"}
MASK = {1: "0xFFu", 2: "0xFFFFu", 4: "0xFFFFFFFFu"}
BITS = {1: 8, 2: 16, 4: 32}


def cond(name):
    name = ALIASES.get(name, name)
    if name not in CONDS:
        raise Unsupported("condition " + name)
    return "CC_%s(fk, fr, fa, fb)" % name.upper()


def setf(kind, sz, r, a, b):
    return "fk = FK(%s, %d); fr = %s; fa = %s; fb = %s;" % (kind, sz, r, a, b)


CF = "fl_cf(fk, fr, fa, fb)"


def _alu(m, ins, o):
    d, s = ins.operands
    sz = d.size
    a, b = o.read(d), o.read(s, sz)
    if m in ("xor", "sub") and d.type == s.type == 1 and d.reg == s.reg:   # xor r,r / sub r,r: the zero idiom
        return [o.write(d, "0u"), setf("FK_LOGIC", sz, "0u", "0u", "0u")]
    expr = {"add": "a + b", "sub": "a - b", "and": "a & b", "or": "a | b", "xor": "a ^ b", "cmp": "a - b",
            "test": "a & b", "adc": "a + b + %s" % CF, "sbb": "a - b - %s" % CF}[m]
    kind = {"add": "FK_ADD", "sub": "FK_SUB", "cmp": "FK_SUB", "adc": "FK_ADC", "sbb": "FK_SBB"}.get(m, "FK_LOGIC")
    wr = "" if m in ("cmp", "test") else o.write(d, "r")
    return ["{ uint32_t a = %s, b = %s; uint32_t r = %s; %s %s }" % (a, b, expr, setf(kind, sz, "r", "a", "b"), wr)]


def _shift(m, ins, o):
    d = ins.operands[0]
    sz = d.size
    if len(ins.operands) == 1:
        cnt = "1u"
    else:
        cnt = o.read(ins.operands[1], 1)
    a = o.read(d)
    mask = MASK[sz]
    if m in ("shl", "sal"):
        r, kind = "a << n", "FK_SHL"
    elif m == "shr":
        r, kind = "(a & %s) >> n" % mask, "FK_SHR"
    elif m == "sar":
        r, kind = "(uint32_t)(%s >> n)" % o.signed("a", sz), "FK_SAR"
    elif m in ("rol", "ror"):
        bits = BITS[sz]
        if m == "rol":
            r = "((a << (n %% %d)) | ((a & %s) >> ((%d - n %% %d) %% %d)))" % (bits, mask, bits, bits, bits)
            newcf = "(r & 1u)"
        else:
            r = "(((a & %s) >> (n %% %d)) | (a << ((%d - n %% %d) %% %d)))" % (mask, bits, bits, bits, bits)
            newcf = "((r >> %d) & 1u)" % (bits - 1)
        return ["{ uint32_t n = %s & 31u; if (n) { uint32_t a = %s; uint32_t r = %s; %s "
                "fr = (fl_eflags(fk, fr, fa, fb) & ~(EF_CF | EF_OF)) | %s; fk = FK(FK_RAW, 4); } }"
                % (cnt, a, r, o.write(d, "r"), newcf)]
    else:
        raise Unsupported(m)
    return ["{ uint32_t n = %s & 31u; if (n) { uint32_t a = %s; uint32_t r = %s; %s %s } }"
            % (cnt, a, r, o.write(d, "r"), setf(kind, sz, "r", "a & %s" % mask, "n"))]


def _shxd(m, ins, o):
    d, s, c = ins.operands
    if d.size != 4:
        raise Unsupported(m + " 16-bit")
    a, b, cnt = o.read(d), o.read(s), o.read(c, 1)
    if m == "shld":
        r, kind = "(a << n) | (b >> (32 - n))", "FK_SHL"
    else:
        r, kind = "(a >> n) | (b << (32 - n))", "FK_SHR"
    return ["{ uint32_t n = %s & 31u; if (n) { uint32_t a = %s, b = %s; uint32_t r = %s; %s %s } }"
            % (cnt, a, b, r, o.write(d, "r"), setf(kind, 4, "r", "a", "n"))]


def _muldiv(m, ins, o):
    ops = ins.operands
    if m == "imul" and len(ops) >= 2:
        d = ops[0]
        x, y = (o.read(ops[0]), o.read(ops[1])) if len(ops) == 2 else (o.read(ops[1]), o.read(ops[2], ops[1].size))
        sz = d.size
        return ["{ int64_t p = (int64_t)%s * (int64_t)%s; uint32_t r = (uint32_t)p; uint32_t ov = p != (int64_t)%s; "
                "%s %s }" % (o.signed(x, sz), o.signed(y, sz), o.signed("r", sz), o.write(d, "r"),
                             setf("FK_MUL", sz, "r", "ov", "0u"))]
    s = ops[0]
    sz = s.size
    v = o.read(s)
    if m in ("mul", "imul"):
        sign = m == "imul"
        if sz == 4:
            prod = "(uint64_t)((int64_t)(int32_t)eax * (int64_t)(int32_t)%s)" % v if sign else \
                "(uint64_t)eax * (uint64_t)%s" % v
            ov = "(edx != (uint32_t)((int32_t)eax >> 31))" if sign else "(edx != 0u)"
            return ["{ uint64_t p = %s; eax = (uint32_t)p; edx = (uint32_t)(p >> 32); %s }" % (
                prod, setf("FK_MUL", 4, "eax", ov, "0u"))]
        if sz == 1:
            prod = "(uint32_t)((int32_t)(int8_t)eax * (int32_t)(int8_t)%s)" % v if sign else "(eax & 0xFFu) * %s" % v
            ov = "((uint32_t)(int32_t)(int8_t)p != (p & 0xFFFFu))" if sign else "((p >> 8) & 0xFFu) != 0u"
            return ["{ uint32_t p = %s; %s %s }" % (prod, o.write_reg("ax", "p"), setf("FK_MUL", 1, "p", ov, "0u"))]
        raise Unsupported(m + " 16-bit")
    # div / idiv (a zero divisor is undefined behaviour here; the original would fault)
    if sz == 4:
        if m == "div":
            return ["{ uint64_t n = ((uint64_t)edx << 32) | eax; uint32_t dv = %s; eax = (uint32_t)(n / dv); "
                    "edx = (uint32_t)(n %% dv); }" % v]
        return ["{ int64_t n = (int64_t)(((uint64_t)edx << 32) | eax); int32_t dv = (int32_t)%s; "
                "eax = (uint32_t)(int32_t)(n / dv); edx = (uint32_t)(int32_t)(n %% dv); }" % v]
    if sz == 1:
        if m == "div":
            return ["{ uint32_t n = eax & 0xFFFFu, dv = %s; %s }" % (v, o.write_reg("ax", "((n % dv) << 8) | (n / dv & 0xFFu)"))]
        return ["{ int32_t n = (int16_t)eax, dv = (int8_t)%s; %s }" % (
            v, o.write_reg("ax", "(((uint32_t)(n % dv) & 0xFFu) << 8) | ((uint32_t)(n / dv) & 0xFFu)"))]
    raise Unsupported(m + " 16-bit")


def _string(m, ins, o):
    rep = m.startswith("rep ")
    repcond = m.split(" ")[0] if m.split(" ")[0] in ("repe", "repne") else None
    base = m.split(" ")[-1]
    op, sizech = base[:-1], base[-1]
    sz = {"b": 1, "w": 2, "d": 4}.get(sizech)
    if op not in ("movs", "stos", "scas", "cmps", "lods") or sz is None:
        return None
    step = "(c->df ? -%du : %du)" % (sz, sz)
    ld, st = {1: ("R8", "W8"), 2: ("R16", "W16"), 4: ("R32", "W32")}[sz]
    acc = {1: "(eax & 0xFFu)", 2: "(eax & 0xFFFFu)", 4: "eax"}[sz]
    if op == "movs":
        body = "%s(edi, %s(esi)); esi += %s; edi += %s;" % (st, ld, step, step)
    elif op == "stos":
        body = "%s(edi, eax); edi += %s;" % (st, step)
    elif op == "lods":
        body = "%s esi += %s;" % (o.write_reg({1: "al", 2: "ax", 4: "eax"}[sz], "%s(esi)" % ld), step)
    elif op == "scas":
        body = "{ uint32_t a = %s, b = %s(edi); %s } edi += %s;" % (acc, ld, setf("FK_SUB", sz, "a - b", "a", "b"), step)
    else:
        body = "{ uint32_t a = %s(esi), b = %s(edi); %s } esi += %s; edi += %s;" % (
            ld, ld, setf("FK_SUB", sz, "a - b", "a", "b"), step, step)
    if not rep and not repcond:
        return [body]
    if repcond:
        stopif = "if (!CC_E(fk, fr, fa, fb)) break;" if repcond == "repe" else "if (CC_E(fk, fr, fa, fb)) break;"
        return ["while (ecx) { %s ecx--; %s }" % (body, stopif)]
    return ["while (ecx) { %s ecx--; }" % body]


def _atomic(m, ins, o):
    """lock-prefixed and implicitly locked (xchg with memory) read-modify-write on guest memory."""
    d = ins.operands[0]
    sz = d.size
    if sz not in (1, 2, 4):
        raise Unsupported("atomic of %d bytes" % sz)
    ty = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t"}[sz]
    ptr = "(%s *)HOST(%s)" % (ty, o.addr(d))
    if sz != 4 and m in ("cmpxchg", "xadd"):
        raise Unsupported("lock %s of %d bytes" % (m, sz))
    if m == "cmpxchg":
        s = o.read(ins.operands[1])
        return ["{ uint32_t exp = eax, des = %s; uint32_t old = eax; "
                "if (!__atomic_compare_exchange_n(%s, &exp, des, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) eax = exp; "
                "%s }" % (s, ptr, setf("FK_SUB", 4, "old - exp", "old", "exp"))]
    if m == "xadd":
        s = ins.operands[1]
        return ["{ uint32_t b = %s; uint32_t a = __atomic_fetch_add(%s, b, __ATOMIC_SEQ_CST); %s %s }" % (
            o.read(s), ptr, o.write(s, "a"), setf("FK_ADD", 4, "a + b", "a", "b"))]
    if m == "xchg":
        s = ins.operands[1]
        return ["{ uint32_t v = __atomic_exchange_n(%s, (%s)%s, __ATOMIC_SEQ_CST); %s }" % (
            ptr, ty, o.read(s), o.write(s, "v"))]
    fn = {"add": ("__atomic_fetch_add", "a + b", "FK_ADD"), "sub": ("__atomic_fetch_sub", "a - b", "FK_SUB"),
          "or": ("__atomic_fetch_or", "a | b", "FK_LOGIC"), "and": ("__atomic_fetch_and", "a & b", "FK_LOGIC"),
          "xor": ("__atomic_fetch_xor", "a ^ b", "FK_LOGIC"), "inc": ("__atomic_fetch_add", "a + 1u", "FK_INC"),
          "dec": ("__atomic_fetch_sub", "a - 1u", "FK_DEC")}.get(m)
    if fn is None:
        raise Unsupported("lock " + m)
    if m in ("inc", "dec"):
        return ["{ uint32_t cf = %s; uint32_t a = %s(%s, 1u, __ATOMIC_SEQ_CST); %s }" % (
            CF, fn[0], ptr, setf(fn[2], sz, fn[1], "a", "cf"))]
    return ["{ uint32_t b = %s; uint32_t a = %s(%s, (%s)b, __ATOMIC_SEQ_CST); %s }" % (
        o.read(ins.operands[1]), fn[0], ptr, ty, setf(fn[2], sz, fn[1], "a", "b"))]


def emit(ins, o):
    m = ins.mnemonic
    ops = ins.operands
    locked = m.startswith("lock ")
    if locked:
        m = m[5:]
    if locked or (m == "xchg" and ops[0].type == 3):
        if m == "xchg" and ops[0].type != 3:
            ops = ops[::-1]
        return _atomic(m, ins, o) if ops[0].type == 3 else None
    if m.startswith(("rep ", "repe ", "repne ")) or m in STRING_OPS:
        if not any(o.regname(op.reg).startswith("xmm") for op in ops if op.type == 1):
            return _string(m, ins, o)
    if any(op.type == 1 and o.regname(op.reg).startswith(("xmm", "st", "mm")) for op in ops):
        return None
    if m == "mov":
        if any(op.type == 1 and o.regname(op.reg) in ("cs", "ds", "es", "fs", "gs", "ss") for op in ops):
            raise Unsupported("segment register move")
        return [o.write(ops[0], o.read(ops[1], ops[0].size))]
    if m == "movzx":
        return [o.write(ops[0], o.read(ops[1]))]
    if m == "movsx":
        return [o.write(ops[0], "(uint32_t)%s" % o.signed(o.read(ops[1]), ops[1].size))]
    if m == "lea":
        return [o.write(ops[0], o.addr(ops[1]))]
    if m == "push":
        if ops[0].size == 2:
            raise Unsupported("push 16-bit")
        return ["{ uint32_t t = %s; esp -= 4u; W32(esp, t); }" % o.read(ops[0], 4)]
    if m == "pop":
        return ["{ uint32_t t = R32(esp); esp += 4u; %s }" % o.write(ops[0], "t")]
    if m == "xchg":
        a, b = ops
        return ["{ uint32_t t = %s; %s %s }" % (o.read(a), o.write(a, o.read(b)), o.write(b, "t"))]
    if m in ("add", "sub", "and", "or", "xor", "cmp", "test", "adc", "sbb"):
        return _alu(m, ins, o)
    if m in ("inc", "dec"):
        d = ops[0]
        return ["{ uint32_t cf = %s; uint32_t a = %s; uint32_t r = a %s 1u; %s %s }" % (
            CF, o.read(d), "+" if m == "inc" else "-", o.write(d, "r"),
            setf("FK_INC" if m == "inc" else "FK_DEC", d.size, "r", "a", "cf"))]
    if m == "neg":
        d = ops[0]
        return ["{ uint32_t a = %s; uint32_t r = 0u - a; %s %s }" % (o.read(d), o.write(d, "r"),
                                                                    setf("FK_NEG", d.size, "r", "a", "0u"))]
    if m == "not":
        return [o.write(ops[0], "~%s" % o.read(ops[0]))]
    if m in ("shl", "sal", "shr", "sar", "rol", "ror"):
        return _shift(m, ins, o)
    if m in ("shld", "shrd"):
        return _shxd(m, ins, o)
    if m in ("imul", "mul", "div", "idiv"):
        return _muldiv(m, ins, o)
    if m == "cdq":
        return ["edx = (uint32_t)((int32_t)eax >> 31);"]
    if m == "cwde":
        return ["eax = (uint32_t)(int32_t)(int16_t)eax;"]
    if m == "cbw":
        return [o.write_reg("ax", "(uint32_t)(int32_t)(int8_t)eax")]
    if m == "cwd":
        return [o.write_reg("dx", "((int16_t)eax < 0) ? 0xFFFFu : 0u")]
    if m.startswith("set"):
        return [o.write(ops[0], cond(m[3:]))]
    if m.startswith("cmov"):
        return ["if (%s) { %s }" % (cond(m[4:]), o.write(ops[0], o.read(ops[1])))]
    if m == "bswap":
        return [o.write(ops[0], "__builtin_bswap32(%s)" % o.read(ops[0]))]
    if m in ("bt", "bts", "btr", "btc"):
        d, s = ops
        if d.type == 3 and s.type == 1:                    # bit string: the offset can reach past the dword
            new = {"bt": None, "bts": "v | (1u << (n & 7))", "btr": "v & ~(1u << (n & 7))", "btc": "v ^ (1u << (n & 7))"}[m]
            return ["{ int32_t n = (int32_t)%s; uint32_t p = %s + (uint32_t)(n >> 3); uint32_t v = R8(p); "
                    "fr = (fl_eflags(fk, fr, fa, fb) & ~EF_CF) | ((v >> (n & 7)) & 1u); fk = FK(FK_RAW, 4); %s }"
                    % (o.read(s), o.addr(d), "W8(p, %s);" % new if new else "")]
        bit = "(%s & %du)" % (o.read(s, 1), d.size * 8 - 1)
        new = {"bt": None, "bts": "a | (1u << n)", "btr": "a & ~(1u << n)", "btc": "a ^ (1u << n)"}[m]
        wr = o.write(d, new) if new else ""
        return ["{ uint32_t n = %s; uint32_t a = %s; fr = (fl_eflags(fk, fr, fa, fb) & ~EF_CF) | ((a >> n) & 1u); "
                "fk = FK(FK_RAW, 4); %s }" % (bit, o.read(d), wr)]
    if m in ("bsf", "bsr"):
        d, s = ops
        fn = "__builtin_ctz(v)" if m == "bsf" else "(31 - __builtin_clz(v))"
        return ["{ uint32_t v = %s; if (v) { %s } fk = FK(FK_LOGIC, 4); fr = v; fa = fb = 0u; }" % (
            o.read(s), o.write(d, fn))]
    if m in ("pushal", "pushad"):
        return ["{ uint32_t t = esp; esp -= 32u; W32(esp + 28u, eax); W32(esp + 24u, ecx); W32(esp + 20u, edx); "
                "W32(esp + 16u, ebx); W32(esp + 12u, t); W32(esp + 8u, ebp); W32(esp + 4u, esi); W32(esp, edi); }"]
    if m in ("popal", "popad"):
        return ["edi = R32(esp); esi = R32(esp + 4u); ebp = R32(esp + 8u); ebx = R32(esp + 16u); edx = R32(esp + 20u); "
                "ecx = R32(esp + 24u); eax = R32(esp + 28u); esp += 32u;"]
    if m in ("rcl", "rcr") and ops[0].size == 4:
        d = ops[0]
        cnt = "1u" if len(ops) == 1 else o.read(ops[1], 1)
        step = ("{ uint32_t out = a >> 31; a = (a << 1) | cf; cf = out; }" if m == "rcl" else
                "{ uint32_t out = a & 1u; a = (a >> 1) | (cf << 31); cf = out; }")
        return ["{ uint32_t n = %s & 31u; if (n) { uint32_t a = %s, cf = %s; for (uint32_t i = 0; i < n; i++) %s "
                "%s fr = (fl_eflags(fk, fr, fa, fb) & ~EF_CF) | cf; fk = FK(FK_RAW, 4); } }" % (cnt, o.read(d), CF, step, o.write(d, "a"))]
    if m == "xlatb":
        return [o.write_reg("al", "R8(ebx + (eax & 0xFFu))")]
    if m == "leave":
        return ["esp = ebp; ebp = R32(esp); esp += 4u;"]
    if m == "lahf":
        return [o.write_reg("ah", "(fl_eflags(fk, fr, fa, fb) & 0xD5u) | 2u")]
    if m == "sahf":
        return ["{ uint32_t of = fl_of(fk, fr, fa, fb); fr = ((eax >> 8) & 0xD5u) | (of ? EF_OF : 0u); "
                "fk = FK(FK_RAW, 4); }"]
    if m in ("clc", "stc", "cmc"):
        new = {"clc": "0u", "stc": "1u", "cmc": "(%s ^ 1u)" % CF}[m]
        return ["fr = (fl_eflags(fk, fr, fa, fb) & ~EF_CF) | %s; fk = FK(FK_RAW, 4);" % new]
    if m in ("cld", "std"):
        return ["c->df = %du;" % (m == "std")]
    if m == "pushfd":
        return ["esp -= 4u; W32(esp, fl_eflags(fk, fr, fa, fb) | (c->df << 10));"]
    if m == "popfd":
        return ["fr = R32(esp) & 0x8D5u; fk = FK(FK_RAW, 4); c->df = (R32(esp) >> 10) & 1u; esp += 4u;"]
    if m in ("nop", "fnop", "pause", "prefetchnta", "prefetcht0", "prefetcht1", "prefetcht2", "lfence", "mfence",
             "sfence", "emms"):
        return []
    if m in ("int3", "ud2", "hlt"):
        return ["usf_unknown(c, %s, \"%s\");" % (hexu(ins.address), m)]
    if m == "rdtsc":
        return ["{ uint64_t t = __builtin_readcyclecounter(); eax = (uint32_t)t; edx = (uint32_t)(t >> 32); }"]
    if m == "cpuid":
        return ["usf_cpuid(c, &eax, &ebx, &ecx, &edx);"]
    return None
