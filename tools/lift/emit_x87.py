"""x87 floating-point instructions to C, for the lifter (M4).

What it does: emit(ins, o) returns the C statements of an x87 instruction over the stack in CPU (runtime/include/usf/
x87.h): loads, stores, integer conversions, arithmetic, compares, constants, control and status word, or None when the
instruction is not x87. The register forms of arithmetic (D8/DC/DE with a register operand) are decoded from the
opcode bytes, following the Intel manual: disassemblers have historically swapped fsub/fsubr and fdiv/fdivr there.
Every arithmetic result goes through X87R (precision control).
What it does not do: SSE (emit_sse.py) or 80-bit precision (doubles; see x87.h).
"""
from ops import Unsupported

# reg field of the modrm for D8 (st0 = st0 op sti) and DC/DE (sti = sti op st0)
D8_OPS = {0: "a + b", 1: "a * b", 4: "a - b", 5: "b - a", 6: "a / b", 7: "b / a"}       # a = st0, b = sti
DC_OPS = {0: "s + t", 1: "s * t", 4: "t - s", 5: "s - t", 6: "t / s", 7: "s / t"}       # s = sti, t = st0
MEM_OPS = {"fadd": "a + v", "fmul": "a * v", "fsub": "a - v", "fsubr": "v - a", "fdiv": "a / v", "fdivr": "v / a",
           "fiadd": "a + v", "fimul": "a * v", "fisub": "a - v", "fisubr": "v - a", "fidiv": "a / v",
           "fidivr": "v / a"}
CONSTS = {"fldz": "0.0", "fld1": "1.0", "fldpi": "3.14159265358979323846", "fldl2e": "1.44269504088896340736",
          "fldl2t": "3.32192809488736234787", "fldlg2": "0.301029995663981195214",
          "fldln2": "0.693147180559945309417"}
UNARY = {"fchs": "-a", "fabs": "fabs(a)", "fsqrt": "sqrt(a)", "fsin": "sin(a)", "fcos": "cos(a)",
         "frndint": "x87_frndint(c, a)"}


def _stidx(o, op):
    n = o.regname(op.reg)
    if not n.startswith("st"):
        raise Unsupported("x87 operand " + n)
    return 0 if n == "st(0)" else int(n[3])


def _memval(o, op, integer):
    a = o.addr(op)
    if integer:
        return {2: "(double)(int16_t)R16(%s)", 4: "(double)(int32_t)R32(%s)", 8: "(double)(int64_t)R64(%s)"}[op.size] % a
    if op.size == 4:
        return "(double)RF32(%s)" % a
    if op.size == 8:
        return "RF64(%s)" % a
    if op.size == 10:
        return "usf_f80_load(%s)" % a
    raise Unsupported("x87 memory size %d" % op.size)


def _store(o, op, integer, val="ST(0)"):
    a = o.addr(op)
    if integer:
        bits = op.size * 8
        st = {2: "W16", 4: "W32", 8: "W64"}[op.size]
        return "%s(%s, (uint64_t)x87_to_int(c, %s, %d));" % (st, a, val, bits)
    if op.size == 4:
        return "WF32(%s, (float)%s);" % (a, val)
    if op.size == 8:
        return "WF64(%s, %s);" % (a, val)
    if op.size == 10:
        return "usf_f80_store(%s, %s);" % (a, val)
    raise Unsupported("x87 store size %d" % op.size)


def _regform(ins):
    """(first opcode byte, reg field, i) for a register-operand x87 arithmetic instruction, else None."""
    b0 = ins.opcode[0]
    if b0 in (0xD8, 0xDC, 0xDE) and ins.modrm >= 0xC0:
        return b0, (ins.modrm >> 3) & 7, ins.modrm & 7
    return None


def emit(ins, o):
    m = ins.mnemonic
    if not (m.startswith("f") or m == "wait"):
        return None
    ops = ins.operands
    rf = _regform(ins)
    if rf is not None:
        b0, reg, i = rf
        if reg in (2, 3):                                    # fcom / fcomp st(i) (DC/DE forms are aliases)
            out = ["x87_compare(c, ST(0), ST(%d));" % i]
            if reg == 3 or b0 == 0xDE:
                out.append("FPOP();")
                if b0 == 0xDE:                               # DE D9: fcompp
                    out.append("FPOP();")
            return out
        if b0 == 0xD8:
            return ["{ double a = ST(0), b = ST(%d); ST(0) = X87R(%s); }" % (i, D8_OPS[reg])]
        out = ["{ double s = ST(%d), t = ST(0); ST(%d) = X87R(%s); }" % (i, i, DC_OPS[reg])]
        if b0 == 0xDE:
            out.append("FPOP();")
        return out
    if m in ("fld", "fild"):
        if ops[0].type == 1:
            return ["FPUSH(ST(%d));" % _stidx(o, ops[0])]
        return ["FPUSH(%s);" % _memval(o, ops[0], m == "fild")]
    if m in ("fst", "fstp", "fist", "fistp", "fisttp"):
        pop = ["FPOP();"] if m.endswith("p") else []
        if ops[0].type == 1:
            return ["ST(%d) = ST(0);" % _stidx(o, ops[0])] + pop
        if m == "fisttp":
            st = {2: "W16", 4: "W32", 8: "W64"}[ops[0].size]
            return ["%s(%s, (uint64_t)(int64_t)trunc(ST(0)));" % (st, o.addr(ops[0]))] + pop
        return [_store(o, ops[0], m.startswith("fi"))] + pop
    if m in MEM_OPS and ops and ops[0].type == 3:
        v = _memval(o, ops[0], m.startswith("fi"))
        return ["{ double a = ST(0), v = %s; ST(0) = X87R(%s); }" % (v, MEM_OPS[m])]
    if m in ("fcom", "fcomp", "fucom", "fucomp", "ficom", "ficomp") and ops and ops[0].type == 3:
        out = ["x87_compare(c, ST(0), %s);" % _memval(o, ops[0], m.startswith("fi"))]
        return out + (["FPOP();"] if m.endswith("p") else [])
    if m in ("fucom", "fucomp") and ops:
        out = ["x87_compare(c, ST(0), ST(%d));" % _stidx(o, ops[-1])]
        return out + (["FPOP();"] if m.endswith("p") else [])
    if m in ("fcompp", "fucompp"):
        return ["x87_compare(c, ST(0), ST(1));", "FPOP();", "FPOP();"]
    if m in ("fcomi", "fucomi", "fcompi", "fucompi", "fcomip", "fucomip"):
        i = _stidx(o, ops[-1])
        out = ["fr = fp_compare_eflags(ST(0), ST(%d)); fk = FK(FK_RAW, 4);" % i]
        return out + (["FPOP();"] if "p" in m[4:] else [])
    if m == "ftst":
        return ["x87_compare(c, ST(0), 0.0);"]
    if m == "fxch":
        i = _stidx(o, ops[-1]) if ops else 1
        return ["{ double t = ST(0); ST(0) = ST(%d); ST(%d) = t; }" % (i, i)]
    if m in CONSTS:
        return ["FPUSH(%s);" % CONSTS[m]]
    if m in UNARY:
        return ["{ double a = ST(0); ST(0) = X87R(%s); }" % UNARY[m]]
    if m == "fpatan":
        return ["{ double v = X87R(atan2(ST(1), ST(0))); FPOP(); ST(0) = v; }"]
    if m == "fptan":
        return ["ST(0) = X87R(tan(ST(0)));", "FPUSH(1.0);"]
    if m == "fscale":
        return ["ST(0) = X87R(ldexp(ST(0), (int)trunc(ST(1))));"]
    if m == "f2xm1":
        return ["ST(0) = X87R(exp2(ST(0)) - 1.0);"]
    if m == "fyl2x":
        return ["{ double v = X87R(ST(1) * log2(ST(0))); FPOP(); ST(0) = v; }"]
    if m == "fprem":
        return ["{ double q = trunc(ST(0) / ST(1)); ST(0) = ST(0) - q * ST(1); c->fpu_sw &= ~0x0400u; }"]
    if m == "fprem1":
        return ["{ ST(0) = remainder(ST(0), ST(1)); c->fpu_sw &= ~0x0400u; }"]
    if m == "fnstsw":
        if ops and ops[0].type == 1:
            return [o.write_reg("ax", "x87_status(c)")]
        return ["W16(%s, x87_status(c));" % o.addr(ops[0])]
    if m == "fnstcw":
        return ["W16(%s, c->fpu_cw);" % o.addr(ops[0])]
    if m == "fldcw":
        return ["c->fpu_cw = R16(%s);" % o.addr(ops[0])]
    if m in ("fnclex", "fninit", "wait", "fwait", "ffree", "fincstp", "fdecstp"):
        if m == "fninit":
            return ["c->fpu_cw = 0x037Fu; c->fpu_sw = 0u; c->top = 0u;"]
        if m == "fincstp":
            return ["FPOP();"]
        if m == "fdecstp":
            return ["c->top = (c->top - 1u) & 7u;"]
        return []
    if m == "fxam":
        return ["c->fpu_sw = (c->fpu_sw & ~0x4700u) | x87_examine(ST(0));"]
    if m.startswith("fcmov"):
        raise Unsupported(m)
    raise Unsupported("x87 " + m)
