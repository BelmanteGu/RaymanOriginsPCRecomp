"""SSE / SSE2 instructions to C, for the lifter (M4).

What it does: emit(ins, o) returns the C statements of an SSE instruction over the xmm registers in CPU (c->xmm[i],
a union of floats, doubles and integers): moves, scalar and packed float arithmetic, logic, compares, conversions,
shuffles and the few integer SIMD forms the game uses; or None when the instruction has no xmm operand.
The generated C must be compiled with -ffp-contract=off and without fast-math, so that every operation rounds exactly
as the SSE unit does (no fused multiply-add): the game's simulation must give the same numbers on every machine.
What it does not do: MMX (6 instructions in the whole game: reported as unsupported) or the x87 stack (emit_x87.py).
"""
from ops import Unsupported

# SSE instructions that can have no xmm operand (memory source, general register destination)
SSE_NO_XMM = {"cvttss2si", "cvtss2si", "cvttsd2si", "cvtsd2si"}
SCALAR = {"add": "a + b", "sub": "a - b", "mul": "a * b", "div": "a / b", "min": "(a < b) ? a : b",
          "max": "(a > b) ? a : b"}
CMP = {"eq": "a == b", "lt": "a < b", "le": "a <= b", "unord": "a != a || b != b", "neq": "!(a == b)",
       "nlt": "!(a < b)", "nle": "!(a <= b)", "ord": "a == a && b == b"}


def _x(o, op):
    return "c->xmm[%d]" % o.xmm(op)


def _is_xmm(o, op):
    return op.type == 1 and o.regname(op.reg).startswith("xmm")


def _src128(o, op):
    return _x(o, op) if _is_xmm(o, op) else "xmm_load(%s)" % o.addr(op)


def _scal(o, op, d):
    """Scalar float (d=False) or double (d=True) source."""
    if _is_xmm(o, op):
        return "%s.%s" % (_x(o, op), "d[0]" if d else "f[0]")
    return ("RF64(%s)" if d else "RF32(%s)") % o.addr(op)


def _move(m, ins, o):
    if len(ins.operands) != 2:
        return None
    d, s = ins.operands
    if m in ("movaps", "movups", "movdqa", "movdqu", "movapd", "movupd", "lddqu"):
        if _is_xmm(o, d):
            return ["%s = %s;" % (_x(o, d), _src128(o, s))]
        return ["xmm_store(%s, %s);" % (o.addr(d), _x(o, s))]
    if m in ("movss", "movsd"):
        dbl = m == "movsd"
        if _is_xmm(o, d) and _is_xmm(o, s):
            return ["%s.%s = %s.%s;" % (_x(o, d), "q[0]" if dbl else "u[0]", _x(o, s), "q[0]" if dbl else "u[0]")]
        if _is_xmm(o, d):
            if dbl:
                return ["%s.q[0] = R64(%s); %s.q[1] = 0;" % (_x(o, d), o.addr(s), _x(o, d))]
            return ["{ XMM t = {0}; t.u[0] = R32(%s); %s = t; }" % (o.addr(s), _x(o, d))]
        return ["%s(%s, %s.%s);" % ("W64" if dbl else "W32", o.addr(d), _x(o, s), "q[0]" if dbl else "u[0]")]
    if m == "movq":
        if _is_xmm(o, d):
            src = "%s.q[0]" % _x(o, s) if _is_xmm(o, s) else "R64(%s)" % o.addr(s)
            return ["{ uint64_t t = %s; %s.q[0] = t; %s.q[1] = 0; }" % (src, _x(o, d), _x(o, d))]
        return ["W64(%s, %s.q[0]);" % (o.addr(d), _x(o, s))]
    if m == "movd":
        if _is_xmm(o, d):
            return ["{ XMM t = {0}; t.u[0] = %s; %s = t; }" % (o.read(s, 4), _x(o, d))]
        return [o.write(d, "%s.u[0]" % _x(o, s), 4)]
    if m in ("movlps", "movlpd", "movhps", "movhpd"):
        lane = "q[0]" if "l" in m[3] else "q[1]"
        if _is_xmm(o, d):
            return ["%s.%s = R64(%s);" % (_x(o, d), lane, o.addr(s))]
        return ["W64(%s, %s.%s);" % (o.addr(d), _x(o, s), lane)]
    if m == "movlhps":
        return ["%s.q[1] = %s.q[0];" % (_x(o, d), _x(o, s))]
    if m == "movhlps":
        return ["%s.q[0] = %s.q[1];" % (_x(o, d), _x(o, s))]
    return None


def _is_mm(o, op):
    return op.type == 1 and o.regname(op.reg).startswith("mm")


def _mm(o, op):
    return "c->mm[%d]" % int(o.regname(op.reg)[2:])


def _mmx(m, ins, o):
    """MMX on 64-bit registers c->mm[]: the forms Crypto++ uses."""
    ops = ins.operands
    d = ops[0]
    s = ops[1] if len(ops) > 1 else None
    def src64(op):
        if _is_mm(o, op):
            return _mm(o, op)
        if op.type == 3:
            return "R64(%s)" % o.addr(op)
        if _is_xmm(o, op):
            return "%s.q[0]" % _x(o, op)
        raise Unsupported("MMX source")
    if m in ("movd", "movq"):
        if _is_mm(o, d):
            v = o.read(s, 4) if (m == "movd" and not _is_mm(o, s)) else src64(s)
            return ["%s = (uint64_t)%s;" % (_mm(o, d), v if m == "movq" or _is_mm(o, s) else "(uint32_t)" + v)]
        if m == "movd":
            return [o.write(d, "(uint32_t)%s" % _mm(o, s), 4)]
        if d.type == 3:
            return ["W64(%s, %s);" % (o.addr(d), _mm(o, s))]
        if _is_xmm(o, d):
            return ["%s.q[0] = %s; %s.q[1] = 0;" % (_x(o, d), _mm(o, s), _x(o, d))]
    D = _mm(o, d)
    if m in ("pxor", "por", "pand", "pandn", "paddq", "psubq"):
        op = {"pxor": "%s ^ %s", "por": "%s | %s", "pand": "%s & %s", "pandn": "~%s & %s", "paddq": "%s + %s",
              "psubq": "%s - %s"}[m]
        return ["%s = %s;" % (D, op % (D, src64(s)))]
    if m in ("psrlq", "psllq"):
        k = ("(uint64_t)%d" % s.imm) if s.type == 2 else src64(s)
        return ["{ uint64_t k = %s; %s = k > 63 ? 0 : %s %s k; }" % (k, D, D, ">>" if m == "psrlq" else "<<")]
    if m == "pmuludq":
        return ["%s = (uint64_t)(uint32_t)%s * (uint64_t)(uint32_t)%s;" % (D, D, src64(s))]
    if m == "movdq2q":
        return ["%s = %s.q[0];" % (D, _x(o, s))]
    if m == "paddb":
        return ["{ uint64_t y = %s, r = 0; for (int i = 0; i < 8; i++) r |= (uint64_t)(uint8_t)((%s >> (8 * i)) + (y >> (8 * i))) << (8 * i); "
                "%s = r; }" % (src64(s), D, D)]
    if m == "punpckldq":
        return ["%s = (%s & 0xFFFFFFFFull) | (%s << 32);" % (D, D, src64(s))]
    if m in ("paddd", "psubd"):
        sign = "+" if m == "paddd" else "-"
        return ["{ uint64_t y = %s; uint32_t lo = (uint32_t)%s %s (uint32_t)y, hi = (uint32_t)(%s >> 32) %s (uint32_t)(y >> 32); "
                "%s = lo | ((uint64_t)hi << 32); }" % (src64(s), D, sign, D, sign, D)]
    raise Unsupported("MMX " + m)


def emit(ins, o):
    m = ins.mnemonic
    ops = ins.operands
    if any(_is_mm(o, op) for op in ops):
        return _mmx(m, ins, o)
    if not any(_is_xmm(o, op) for op in ops if op.type == 1) and m not in SSE_NO_XMM:
        if m in ("ldmxcsr", "stmxcsr"):
            return ["c->mxcsr = R32(%s);" % o.addr(ops[0])] if m == "ldmxcsr" else ["W32(%s, c->mxcsr);" % o.addr(ops[0])]
        if any(op.type == 1 and o.regname(op.reg).startswith("mm") for op in ops):
            raise Unsupported("MMX " + m)
        return None
    r = _move(m, ins, o)
    if r is not None:
        return r
    d = ops[0]
    s = ops[1] if len(ops) > 1 else None
    D = _x(o, d) if _is_xmm(o, d) else None
    # scalar arithmetic: addss, subsd, sqrtss ...
    for op, expr in SCALAR.items():
        if m in (op + "ss", op + "sd"):
            dbl = m.endswith("sd")
            lane = "d[0]" if dbl else "f[0]"
            return ["{ %s a = %s.%s, b = %s; %s.%s = %s; }" % ("double" if dbl else "float", D, lane, _scal(o, s, dbl),
                                                               D, lane, expr)]
        if m in (op + "ps", op + "pd"):
            dbl = m.endswith("pd")
            n, lane, ty = (2, "d", "double") if dbl else (4, "f", "float")
            return ["{ XMM x = %s, y = %s; for (int i = 0; i < %d; i++) { %s a = x.%s[i], b = y.%s[i]; x.%s[i] = %s; } "
                    "%s = x; }" % (D, _src128(o, s), n, ty, lane, lane, lane, expr, D)]
    if m in ("sqrtss", "sqrtsd"):
        dbl = m == "sqrtsd"
        return ["%s.%s = %s(%s);" % (D, "d[0]" if dbl else "f[0]", "sqrt" if dbl else "sqrtf", _scal(o, s, dbl))]
    if m == "sqrtps":
        return ["{ XMM y = %s; for (int i = 0; i < 4; i++) %s.f[i] = sqrtf(y.f[i]); }" % (_src128(o, s), D)]
    if m in ("rcpss", "rsqrtss"):
        # the hardware gives a 12-bit approximation; the exact value differs in the low bits (see the study)
        expr = "1.0f / v" if m == "rcpss" else "1.0f / sqrtf(v)"
        return ["{ float v = %s; %s.f[0] = %s; }" % (_scal(o, s, False), D, expr)]
    if m in ("andps", "andpd", "pand", "orps", "orpd", "por", "xorps", "xorpd", "pxor", "andnps", "andnpd", "pandn"):
        if s is not None and _is_xmm(o, s) and s.reg == d.reg and m in ("xorps", "xorpd", "pxor"):
            return ["%s = (XMM){0};" % D]
        expr = {"and": "x.q[i] & y.q[i]", "or": "x.q[i] | y.q[i]", "xor": "x.q[i] ^ y.q[i]",
                "andn": "~x.q[i] & y.q[i]"}[m.lstrip("p")[:-2] if not m.startswith("p") else m[1:]]
        return ["{ XMM x = %s, y = %s; for (int i = 0; i < 2; i++) x.q[i] = %s; %s = x; }" % (D, _src128(o, s), expr, D)]
    if m in ("comiss", "ucomiss", "comisd", "ucomisd"):
        dbl = m.endswith("sd")
        return ["fr = fp_compare_eflags(%s.%s, %s); fk = FK(FK_RAW, 4);" % (D, "d[0]" if dbl else "f[0]",
                                                                           _scal(o, s, dbl))]
    if m.startswith("cmp") and m[-2:] in ("ss", "ps", "sd", "pd") and m[3:-2] in CMP:
        pred, kind = CMP[m[3:-2]], m[-2:]
        dbl = kind[1] == "d"
        lane, ty, n, mask = ("d", "double", 2, "q") if dbl else ("f", "float", 4, "u")
        full = "~(uint64_t)0" if dbl else "~0u"
        if kind[0] == "s":
            return ["{ %s a = %s.%s[0], b = %s; %s.%s[0] = (%s) ? %s : 0; }" % (
                ty, D, lane, _scal(o, s, dbl), D, mask, pred, full)]
        return ["{ XMM x = %s, y = %s; for (int i = 0; i < %d; i++) { %s a = x.%s[i], b = y.%s[i]; "
                "x.%s[i] = (%s) ? %s : 0; } %s = x; }" % (D, _src128(o, s), n, ty, lane, lane, mask, pred, full, D)]
    if m == "cvtsi2ss":
        return ["%s.f[0] = (float)(int32_t)%s;" % (D, o.read(s, 4))]
    if m == "cvtsi2sd":
        return ["%s.d[0] = (double)(int32_t)%s;" % (D, o.read(s, 4))]
    if m in ("cvttss2si", "cvtss2si", "cvttsd2si", "cvtsd2si"):
        dbl = m.endswith("sd2si")
        fn = "sse_cvtt" if m.startswith("cvtt") else "sse_cvt"
        return [o.write(d, "%s(%s)" % (fn, _scal(o, s, dbl)), 4)]
    if m == "cvtss2sd":
        return ["%s.d[0] = (double)%s;" % (D, _scal(o, s, False))]
    if m == "cvtsd2ss":
        return ["%s.f[0] = (float)%s;" % (D, _scal(o, s, True))]
    if m == "cvtps2pd":
        src = "%s.f" % _x(o, s) if _is_xmm(o, s) else None
        if src is None:
            return ["{ float a = RF32(%s), b = RF32(%s + 4u); %s.d[0] = a; %s.d[1] = b; }" % (
                o.addr(s), o.addr(s), D, D)]
        return ["{ float a = %s[0], b = %s[1]; %s.d[0] = a; %s.d[1] = b; }" % (src, src, D, D)]
    if m == "cvtpd2ps":
        return ["{ XMM y = %s; XMM t = {0}; t.f[0] = (float)y.d[0]; t.f[1] = (float)y.d[1]; %s = t; }" % (
            _src128(o, s), D)]
    if m == "cvtdq2pd":
        src = "R64(%s)" % o.addr(s) if s.type == 3 else "%s.q[0]" % _x(o, s)
        return ["{ uint64_t v = %s; %s.d[0] = (double)(int32_t)(uint32_t)v; %s.d[1] = (double)(int32_t)(v >> 32); }"
                % (src, D, D)]
    if m in ("cvttpd2dq", "cvtpd2dq"):
        fn = "sse_cvtt" if m == "cvttpd2dq" else "sse_cvt"
        return ["{ XMM y = %s, t = {0}; t.u[0] = %s(y.d[0]); t.u[1] = %s(y.d[1]); %s = t; }" % (_src128(o, s), fn, fn, D)]
    if m in ("cvtdq2ps", "cvttps2dq", "cvtps2dq"):
        expr = {"cvtdq2ps": "x.f[i] = (float)y.i[i]", "cvttps2dq": "x.u[i] = sse_cvtt(y.f[i])",
                "cvtps2dq": "x.u[i] = sse_cvt(y.f[i])"}[m]
        return ["{ XMM y = %s, x; for (int i = 0; i < 4; i++) %s; %s = x; }" % (_src128(o, s), expr, D)]
    if m in ("unpcklps", "unpckhps"):
        h = 2 if m == "unpckhps" else 0
        return ["{ XMM x = %s, y = %s, t; t.u[0] = x.u[%d]; t.u[1] = y.u[%d]; t.u[2] = x.u[%d]; t.u[3] = y.u[%d]; "
                "%s = t; }" % (D, _src128(o, s), h, h, h + 1, h + 1, D)]
    if m in ("unpcklpd", "unpckhpd", "punpcklqdq", "punpckhqdq"):
        h = 1 if "h" in m[5:7] or m == "punpckhqdq" else 0
        return ["{ XMM x = %s, y = %s, t; t.q[0] = x.q[%d]; t.q[1] = y.q[%d]; %s = t; }" % (D, _src128(o, s), h, h, D)]
    if m == "punpckldq":
        return ["{ XMM x = %s, y = %s, t; t.u[0] = x.u[0]; t.u[1] = y.u[0]; t.u[2] = x.u[1]; t.u[3] = y.u[1]; "
                "%s = t; }" % (D, _src128(o, s), D)]
    if m == "shufps":
        imm = ops[2].imm
        sel = [(imm >> (2 * i)) & 3 for i in range(4)]
        return ["{ XMM x = %s, y = %s, t; t.u[0] = x.u[%d]; t.u[1] = x.u[%d]; t.u[2] = y.u[%d]; t.u[3] = y.u[%d]; "
                "%s = t; }" % (D, _src128(o, s), sel[0], sel[1], sel[2], sel[3], D)]
    if m == "shufpd":
        imm = ops[2].imm
        return ["{ XMM x = %s, y = %s, t; t.q[0] = x.q[%d]; t.q[1] = y.q[%d]; %s = t; }" % (
            D, _src128(o, s), imm & 1, (imm >> 1) & 1, D)]
    if m == "pshufd":
        imm = ops[2].imm
        sel = [(imm >> (2 * i)) & 3 for i in range(4)]
        return ["{ XMM y = %s, t; t.u[0] = y.u[%d]; t.u[1] = y.u[%d]; t.u[2] = y.u[%d]; t.u[3] = y.u[%d]; %s = t; }"
                % (_src128(o, s), sel[0], sel[1], sel[2], sel[3], D)]
    if m in ("paddd", "psubd", "paddq", "psubq", "pcmpeqd", "pcmpgtd"):
        lane, n = ("q", 2) if m.endswith("q") else ("u", 4)
        expr = {"paddd": "x.u[i] + y.u[i]", "psubd": "x.u[i] - y.u[i]", "paddq": "x.q[i] + y.q[i]",
                "psubq": "x.q[i] - y.q[i]", "pcmpeqd": "(x.u[i] == y.u[i]) ? ~0u : 0u",
                "pcmpgtd": "(x.i[i] > y.i[i]) ? ~0u : 0u"}[m]
        return ["{ XMM x = %s, y = %s; for (int i = 0; i < %d; i++) x.%s[i] = %s; %s = x; }" % (
            D, _src128(o, s), n, lane, expr, D)]
    if m in ("pslld", "psrld", "psrad", "psllq", "psrlq", "pslldq", "psrldq") and ops[1].type == 2:
        k = ops[1].imm
        if m in ("pslldq", "psrldq"):
            k = min(k, 16)
            if m == "pslldq":
                return ["{ XMM y = %s, t = {0}; for (int i = %d; i < 16; i++) t.b[i] = y.b[i - %d]; %s = t; }" % (D, k, k, D)]
            return ["{ XMM y = %s, t = {0}; for (int i = 0; i < 16 - %d; i++) t.b[i] = y.b[i + %d]; %s = t; }" % (D, k, k, D)]
        if m.endswith("q"):
            op = "<<" if m == "psllq" else ">>"
            return ["for (int i = 0; i < 2; i++) %s.q[i] = %s;" % (D, "0" if k > 63 else "%s.q[i] %s %d" % (D, op, k))]
        if m == "psrad":
            return ["for (int i = 0; i < 4; i++) %s.i[i] = %s.i[i] >> %d;" % (D, D, min(k, 31))]
        op = "<<" if m == "pslld" else ">>"
        return ["for (int i = 0; i < 4; i++) %s.u[i] = %s;" % (D, "0u" if k > 31 else "%s.u[i] %s %d" % (D, op, k))]
    if m == "pmuludq":                                           # low 32 bits of each 64-bit lane, 64-bit products
        return ["{ XMM x = %s, y = %s; for (int i = 0; i < 2; i++) x.q[i] = (uint64_t)x.u[2 * i] * (uint64_t)y.u[2 * i]; %s = x; }"
                % (D, _src128(o, s), D)]
    if m == "pmovmskb":
        return [o.write(d, "({ XMM y = %s; uint32_t r = 0; for (int i = 0; i < 16; i++) r |= (uint32_t)(y.b[i] >> 7) << i; r; })"
                % _x(o, s), 4)]
    if m in ("pcmpeqb", "pcmpeqw", "pmaxsw", "pminsw", "pmaxub", "pminub", "paddw", "psubw", "paddb", "psubb"):
        lane, n = ("b", 16) if m.endswith("b") else ("w", 8)
        expr = {"pcmpeqb": "(x.b[i] == y.b[i]) ? 0xFF : 0", "pcmpeqw": "(x.w[i] == y.w[i]) ? 0xFFFF : 0",
                "pmaxsw": "((int16_t)x.w[i] > (int16_t)y.w[i]) ? x.w[i] : y.w[i]",
                "pminsw": "((int16_t)x.w[i] < (int16_t)y.w[i]) ? x.w[i] : y.w[i]",
                "pmaxub": "(x.b[i] > y.b[i]) ? x.b[i] : y.b[i]", "pminub": "(x.b[i] < y.b[i]) ? x.b[i] : y.b[i]",
                "paddw": "x.w[i] + y.w[i]", "psubw": "x.w[i] - y.w[i]", "paddb": "x.b[i] + y.b[i]",
                "psubb": "x.b[i] - y.b[i]"}[m]
        return ["{ XMM x = %s, y = %s; for (int i = 0; i < %d; i++) x.%s[i] = %s; %s = x; }" % (
            D, _src128(o, s), n, lane, expr, D)]
    if m in ("psllq", "psrlq", "pslld", "psrld", "psrad") and ops[1].type != 2:
        cnt = "%s.q[0]" % _x(o, s) if _is_xmm(o, s) else "R64(%s)" % o.addr(s)
        if m.endswith("q"):
            op = "<<" if m == "psllq" else ">>"
            return ["{ uint64_t k = %s; for (int i = 0; i < 2; i++) %s.q[i] = k > 63 ? 0 : %s.q[i] %s k; }" % (cnt, D, D, op)]
        if m == "psrad":
            return ["{ uint64_t k = %s; for (int i = 0; i < 4; i++) %s.i[i] = %s.i[i] >> (k > 31 ? 31 : k); }" % (cnt, D, D)]
        op = "<<" if m == "pslld" else ">>"
        return ["{ uint64_t k = %s; for (int i = 0; i < 4; i++) %s.u[i] = k > 31 ? 0 : %s.u[i] %s k; }" % (cnt, D, D, op)]
    if m == "movmskpd":
        return [o.write(d, "(uint32_t)((%s.q[0] >> 63) | ((%s.q[1] >> 63) << 1))" % (_x(o, s), _x(o, s)), 4)]
    if m == "movmskps":
        return [o.write(d, "((%s.u[0] >> 31) | ((%s.u[1] >> 31) << 1) | ((%s.u[2] >> 31) << 2) | ((%s.u[3] >> 31) << 3))"
                % ((_x(o, s),) * 4), 4)]
    if m == "pextrw":
        return [o.write(d, "%s.w[%d]" % (_x(o, s), ops[2].imm & 7), 4)]
    if m == "pinsrw":
        return ["%s.w[%d] = (uint16_t)%s;" % (D, ops[2].imm & 7, o.read(s, 2))]
    raise Unsupported("SSE " + m)
