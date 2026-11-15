/* Direct3D 9 shader bytecode -> GLSL 450 (see sm3.h for the binding contract).
 *
 * What it does: walks the token stream once to collect declarations (dcl, def/defi/defb, labels), then again to emit
 * one GLSL statement per instruction: arithmetic with swizzles, source modifiers, write masks and saturation, the
 * matrix ops, texture sampling (texld/texldp/texldb/texldl/texldd), texkill, flow control (if/else, rep, loop, break,
 * call/label/ret as GLSL functions). Every register is a global so subroutines can use them. Vertex outputs get the
 * D3D9 half-pixel offset; pixel outputs get the alpha test and the sRGB write that D3D9 does in fixed function.
 * What it does not do: shader model 1.x pixel shaders, predication (setp + predicated ops), texbem-family ops.
 */
#include "sm3.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *s; size_t n, cap; } Str;

static void sp(Str *b, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        size_t room = b->cap - b->n;
        int k = vsnprintf(b->s ? b->s + b->n : NULL, room, fmt, ap);
        va_end(ap);
        if (k >= 0 && (size_t)k < room) { b->n += (size_t)k; return; }
        b->cap = b->cap ? b->cap * 2 + (size_t)k : 4096 + (size_t)k;
        b->s = realloc(b->s, b->cap);
    }
}

enum { R_TEMP = 0, R_INPUT, R_CONST, R_ADDR, R_RASTOUT, R_ATTROUT, R_OUTPUT, R_CONSTINT, R_COLOROUT, R_DEPTHOUT,
       R_SAMPLER, R_CONST2, R_CONST3, R_CONST4, R_CONSTBOOL, R_LOOP, R_TEMPF16, R_MISC, R_LABEL, R_PRED };

typedef struct {
    const uint32_t *t, *end;
    int ps, major;
    Sm3Shader *out;
    uint8_t def_f[256], def_i[16], def_b[16];
    uint32_t in_usage[16], in_index[16], in_used[16], in_centroid[16];
    uint32_t out_usage[12], out_index[12], out_used[12];
    int in_label, depth;
    Str head, body, funcs, init;          /* init: the def constants, set at the start of main() */
    Str *cur;
} Ctx;

static uint32_t rtype(uint32_t p) { return ((p >> 28) & 7u) | ((p >> 8) & 0x18u); }
static uint32_t rnum(uint32_t p) { return p & 0x7FFu; }

/* VS attribute and varying locations (see sm3.h) */
int sm3_attr_location(uint32_t usage, uint32_t index) {
    switch (usage) {
    case 0: return index == 0 ? 0 : -1;          /* POSITION */
    case 1: return index == 0 ? 1 : -1;          /* BLENDWEIGHT */
    case 2: return index == 0 ? 2 : -1;          /* BLENDINDICES */
    case 3: return index == 0 ? 3 : -1;          /* NORMAL */
    case 10: return index < 2 ? 4 + (int)index : -1;   /* COLOR0/1 */
    case 5: return index < 8 ? 6 + (int)index : -1;    /* TEXCOORD0..7 */
    case 6: return index == 0 ? 14 : -1;         /* TANGENT */
    case 7: return index == 0 ? 15 : -1;         /* BINORMAL */
    default: return -1;
    }
}
int sm3_varying_location(uint32_t usage, uint32_t index) {
    switch (usage) {
    case 5: return index < 10 ? (int)index : -1;       /* TEXCOORD0..9 */
    case 10: return index < 2 ? 10 + (int)index : -1;  /* COLOR0/1 */
    case 11: return 12;                                /* FOG */
    case 3: return index == 0 ? 13 : 9;                /* NORMAL */
    case 6: return 14;                                 /* TANGENT */
    case 7: return 15;                                 /* BINORMAL */
    case 0: return index ? 9 : -1;                     /* POSITION1+ as a varying */
    default: return -1;
    }
}

static const char *SW = "xyzw";

static void swz(char *o, uint32_t p) {
    uint32_t s = (p >> 16) & 0xFF;
    o[0] = '.';
    for (int i = 0; i < 4; i++) o[1 + i] = SW[(s >> (2 * i)) & 3];
    o[5] = 0;
    if (s == 0xE4) o[0] = 0;
}

/* register name; `rel` is the relative-address expression or NULL */
static void regname(Ctx *x, uint32_t p, const char *rel, char *o, size_t n) {
    uint32_t t = rtype(p), r = rnum(p);
    switch (t) {
    case R_TEMP: snprintf(o, n, "r%u", r); break;
    case R_INPUT: snprintf(o, n, "v%u", r); break;
    case R_CONST:
        if (rel) snprintf(o, n, "K.c[clamp(%u + %s, 0, %d)]", r, rel, x->ps ? 223 : 255);
        else if (x->def_f[r]) snprintf(o, n, "cd%u", r);
        else snprintf(o, n, "K.c[%u]", r);
        break;
    case R_ADDR: snprintf(o, n, x->ps ? "t%u" : "vec4(a0)", r); break;
    case R_RASTOUT: snprintf(o, n, r == 0 ? "oPos" : r == 1 ? "oFog" : "oPts"); break;
    case R_ATTROUT: snprintf(o, n, "oD%u", r); break;
    case R_OUTPUT: snprintf(o, n, x->major >= 3 ? "o%u" : "oT%u", r); break;
    case R_CONSTINT: snprintf(o, n, x->def_i[r] ? "id%u" : "K.i[%u]", r); break;
    case R_COLOROUT: snprintf(o, n, "oC%u", r); break;
    case R_DEPTHOUT: snprintf(o, n, "oDepth"); break;
    case R_SAMPLER: snprintf(o, n, x->ps ? "s%u" : "vs%u", r); break;
    case R_CONSTBOOL: snprintf(o, n, x->def_b[r] ? "bd%u" : "(((K.b.x >> %u) & 1) != 0)", r); break;
    case R_LOOP: snprintf(o, n, "aL"); break;
    case R_MISC: snprintf(o, n, r == 0 ? "vPos" : "vFace"); break;
    case R_PRED: snprintf(o, n, "p0"); break;
    default: snprintf(o, n, "UNSUPPORTED_REG%u", t); break;
    }
}

/* Reads a source parameter at *pp (and its relative-address token); returns the GLSL vec4 expression. */
static void src(Ctx *x, const uint32_t **pp, char *o, size_t n) {
    uint32_t p = *(*pp)++;
    char rel[64] = {0}, name[128], sw[8];
    if (p & 0x2000u) {
        if (x->major >= 2) {
            uint32_t a = *(*pp)++;
            uint32_t comp = (a >> 16) & 3;
            if (rtype(a) == R_LOOP) snprintf(rel, sizeof rel, "aL");
            else snprintf(rel, sizeof rel, "a0.%c", SW[comp]);
        } else {
            snprintf(rel, sizeof rel, "a0.x");
        }
    }
    regname(x, p, rel[0] ? rel : NULL, name, sizeof name);
    swz(sw, p);
    char v[200];
    uint32_t t = rtype(p);
    if (t == R_CONSTINT) snprintf(v, sizeof v, "vec4(%s)%s", name, sw);
    else if (t == R_LOOP) snprintf(v, sizeof v, "vec4(float(aL))");
    else snprintf(v, sizeof v, "%s%s", name, sw);
    switch ((p >> 24) & 0xF) {
    case 0: snprintf(o, n, "%s", v); break;
    case 1: snprintf(o, n, "(-%s)", v); break;
    case 2: snprintf(o, n, "(%s - 0.5)", v); break;
    case 3: snprintf(o, n, "(0.5 - %s)", v); break;
    case 4: snprintf(o, n, "(2.0 * %s - 1.0)", v); break;
    case 5: snprintf(o, n, "(1.0 - 2.0 * %s)", v); break;
    case 6: snprintf(o, n, "(1.0 - %s)", v); break;
    case 7: snprintf(o, n, "(2.0 * %s)", v); break;
    case 8: snprintf(o, n, "(-2.0 * %s)", v); break;
    case 11: snprintf(o, n, "abs(%s)", v); break;
    case 12: snprintf(o, n, "(-abs(%s))", v); break;
    case 13: snprintf(o, n, "(%s)", v); break;     /* NOT: only on bool/predicate sources */
    default: snprintf(o, n, "%s /*mod*/", v); break;
    }
}

typedef struct { char name[64]; char mask[6]; int sat, type; } Dst;

static void dst(Ctx *x, const uint32_t **pp, Dst *d) {
    uint32_t p = *(*pp)++;
    regname(x, p, NULL, d->name, sizeof d->name);
    uint32_t m = (p >> 16) & 0xF;
    int k = 0;
    d->mask[k++] = '.';
    for (int i = 0; i < 4; i++) if (m & (1u << i)) d->mask[k++] = SW[i];
    d->mask[k] = 0;
    if (m == 0xF || m == 0) d->mask[0] = 0;
    d->sat = (p >> 20) & 1;
    d->type = (int)rtype(p);
    if (d->type == R_DEPTHOUT) d->mask[0] = 0;
}

static void assign(Ctx *x, const Dst *d, const char *expr) {
    if (d->type == R_DEPTHOUT) { sp(x->cur, "  oDepth = (%s).x;\n", expr); return; }
    if (d->type == R_ADDR && !x->ps) {                  /* mova */
        sp(x->cur, "  a0%s = ivec4(floor((%s) + 0.5))%s;\n", d->mask, expr, d->mask);
        return;
    }
    if (d->sat) sp(x->cur, "  %s%s = clamp(%s, 0.0, 1.0)%s;\n", d->name, d->mask, expr, d->mask);
    else sp(x->cur, "  %s%s = (%s)%s;\n", d->name, d->mask, expr, d->mask);
}

static const char *cmpop(uint32_t ctl) {
    static const char *ops[] = {"/*?*/>", ">", "==", ">=", "<", "!=", "<="};
    return ctl < 7 ? ops[ctl] : ">";
}

static void sample(Ctx *x, const Dst *d, const char *coord, uint32_t sreg, int kind, const char *e1, const char *e2) {
    uint32_t st = x->out->sampler_type[sreg & 15];
    const char *cc = st == 3 || st == 4 ? ".xyz" : ".xy";
    char s[32], e[600];
    snprintf(s, sizeof s, x->ps ? "s%u" : "vs%u", sreg);
    switch (kind) {
    case 1:                                                           /* texldp */
        if (st == 2 || st == 0) snprintf(e, sizeof e, "textureProj(%s, %s)", s, coord);
        else snprintf(e, sizeof e, "texture(%s, (%s).xyz / (%s).w)", s, coord, coord);
        break;
    case 2: snprintf(e, sizeof e, "texture(%s, (%s)%s, (%s).w)", s, coord, cc, coord); break;          /* texldb */
    case 3: snprintf(e, sizeof e, "textureLod(%s, (%s)%s, (%s).w)", s, coord, cc, coord); break;       /* texldl */
    case 4: snprintf(e, sizeof e, "textureGrad(%s, (%s)%s, (%s)%s, (%s)%s)", s, coord, cc, e1, cc, e2, cc); break;
    default:
        if (x->ps) snprintf(e, sizeof e, "texture(%s, (%s)%s)", s, coord, cc);
        else snprintf(e, sizeof e, "textureLod(%s, (%s)%s, 0.0)", s, coord, cc);
        break;
    }
    assign(x, d, e);
}

static int instr(Ctx *x, uint32_t tok, const uint32_t *p, const uint32_t *end) {
    uint32_t op = tok & 0xFFFF, ctl = (tok >> 16) & 0xFF;
    char a[256], b[256], c[256], d4[256], e[1200];
    Dst d;
#define S(v) src(x, &p, v, sizeof v)
    if (tok & 0x10000000u) { snprintf(x->out->error, sizeof x->out->error, "predicated instruction %u", op); return -1; }
    switch (op) {
    case 0: return 0;                                                       /* nop */
    case 1: dst(x, &p, &d); S(a); assign(x, &d, a); return 0;               /* mov */
    case 2: dst(x, &p, &d); S(a); S(b); snprintf(e, sizeof e, "%s + %s", a, b); assign(x, &d, e); return 0;
    case 3: dst(x, &p, &d); S(a); S(b); snprintf(e, sizeof e, "%s - %s", a, b); assign(x, &d, e); return 0;
    case 4: dst(x, &p, &d); S(a); S(b); S(c); snprintf(e, sizeof e, "%s * %s + %s", a, b, c); assign(x, &d, e); return 0;
    case 5: dst(x, &p, &d); S(a); S(b); snprintf(e, sizeof e, "%s * %s", a, b); assign(x, &d, e); return 0;
    case 6: dst(x, &p, &d); S(a); snprintf(e, sizeof e, "vec4(1.0 / (%s).x)", a); assign(x, &d, e); return 0;
    case 7: dst(x, &p, &d); S(a); snprintf(e, sizeof e, "vec4(inversesqrt(abs((%s).x)))", a); assign(x, &d, e); return 0;
    case 8: dst(x, &p, &d); S(a); S(b); snprintf(e, sizeof e, "vec4(dot((%s).xyz, (%s).xyz))", a, b); assign(x, &d, e); return 0;
    case 9: dst(x, &p, &d); S(a); S(b); snprintf(e, sizeof e, "vec4(dot(%s, %s))", a, b); assign(x, &d, e); return 0;
    case 10: dst(x, &p, &d); S(a); S(b); snprintf(e, sizeof e, "min(%s, %s)", a, b); assign(x, &d, e); return 0;
    case 11: dst(x, &p, &d); S(a); S(b); snprintf(e, sizeof e, "max(%s, %s)", a, b); assign(x, &d, e); return 0;
    case 12: dst(x, &p, &d); S(a); S(b); snprintf(e, sizeof e, "vec4(lessThan(%s, %s))", a, b); assign(x, &d, e); return 0;
    case 13: dst(x, &p, &d); S(a); S(b); snprintf(e, sizeof e, "vec4(greaterThanEqual(%s, %s))", a, b); assign(x, &d, e); return 0;
    case 14: case 78: dst(x, &p, &d); S(a); snprintf(e, sizeof e, "vec4(exp2((%s).x))", a); assign(x, &d, e); return 0;
    case 15: case 79: dst(x, &p, &d); S(a); snprintf(e, sizeof e, "vec4(log2(abs((%s).x)))", a); assign(x, &d, e); return 0;
    case 16:                                                                /* lit */
        dst(x, &p, &d); S(a);
        snprintf(e, sizeof e, "vec4(1.0, max((%s).x, 0.0), ((%s).x > 0.0) ? pow(max((%s).y, 0.0), clamp((%s).w, -127.9961, 127.9961)) : 0.0, 1.0)", a, a, a, a);
        assign(x, &d, e); return 0;
    case 17: dst(x, &p, &d); S(a); S(b); snprintf(e, sizeof e, "vec4(1.0, (%s).y * (%s).y, (%s).z, (%s).w)", a, b, a, b); assign(x, &d, e); return 0;
    case 18: dst(x, &p, &d); S(a); S(b); S(c); snprintf(e, sizeof e, "mix(%s, %s, %s)", c, b, a); assign(x, &d, e); return 0;
    case 19: dst(x, &p, &d); S(a); snprintf(e, sizeof e, "fract(%s)", a); assign(x, &d, e); return 0;
    case 20: case 21: case 22: case 23: case 24: {                           /* m4x4 m4x3 m3x4 m3x3 m3x2 */
        dst(x, &p, &d); S(a);
        int rows = op == 20 || op == 22 ? 4 : op == 21 || op == 23 ? 3 : 2, three = op >= 22;
        char comp[4][300];
        for (int r = 0; r < 4; r++) {
            if (r >= rows) { snprintf(comp[r], sizeof comp[r], "0.0"); continue; }
            uint32_t row[2] = {p[0] + (uint32_t)r, p[1]};                    /* matrix row: register number + r */
            const uint32_t *q = row;
            src(x, &q, b, sizeof b);
            snprintf(comp[r], sizeof comp[r], three ? "dot((%s).xyz, (%s).xyz)" : "dot(%s, %s)", a, b);
        }
        snprintf(e, sizeof e, "vec4(%s, %s, %s, %s)", comp[0], comp[1], comp[2], comp[3]);
        assign(x, &d, e);
        return 0;
    }
    case 25: sp(x->cur, "  label_%u();\n", rnum(*p)); return 0;              /* call */
    case 26: { uint32_t l = rnum(*p++); S(a); sp(x->cur, "  if ((%s).x != 0.0) label_%u();\n", a, l); return 0; }
    case 27: {                                                              /* loop aL, i# */
        p++;
        S(b);
        sp(x->cur, "  { int aL_s%d = aL; ivec4 li%d = ivec4(%s); aL = li%d.y; for (int k%d = 0; k%d < li%d.x; k%d++, aL += li%d.z) {\n",
           x->depth, x->depth, b, x->depth, x->depth, x->depth, x->depth, x->depth, x->depth);
        x->depth++;
        return 0;
    }
    case 29: x->depth--; sp(x->cur, "  } aL = aL_s%d; }\n", x->depth); return 0;    /* endloop */
    case 28:                                                                /* ret */
        if (x->in_label) { sp(x->cur, "}\n"); x->in_label = 0; x->cur = &x->funcs; }
        else sp(x->cur, "  finish(); return;\n");
        return 0;
    case 30:                                                                /* label */
        if (x->cur == &x->body) { sp(&x->body, "  finish();\n}\n"); }
        x->cur = &x->funcs;
        sp(x->cur, "void label_%u() {\n", rnum(*p));
        x->in_label = 1;
        return 0;
    case 32: dst(x, &p, &d); S(a); S(b); snprintf(e, sizeof e, "vec4(pow(abs((%s).x), (%s).x))", a, b); assign(x, &d, e); return 0;
    case 33: dst(x, &p, &d); S(a); S(b); snprintf(e, sizeof e, "vec4(cross((%s).xyz, (%s).xyz), 0.0)", a, b); assign(x, &d, e); return 0;
    case 34: dst(x, &p, &d); S(a); if (x->major < 3) { S(b); S(c); } snprintf(e, sizeof e, "sign(%s)", a); assign(x, &d, e); return 0;
    case 35: dst(x, &p, &d); S(a); snprintf(e, sizeof e, "abs(%s)", a); assign(x, &d, e); return 0;
    case 36: dst(x, &p, &d); S(a); snprintf(e, sizeof e, "((%s) * inversesqrt(max(dot((%s).xyz, (%s).xyz), 1e-30)))", a, a, a); assign(x, &d, e); return 0;
    case 37:                                                                /* sincos */
        dst(x, &p, &d); S(a); if (x->major < 3) { S(b); S(c); }
        snprintf(e, sizeof e, "vec4(cos((%s).x), sin((%s).x), 0.0, 0.0)", a, a); assign(x, &d, e); return 0;
    case 38: S(a); sp(x->cur, "  for (int rp%d = 0; rp%d < int((%s).x); rp%d++) {\n", x->depth, x->depth, a, x->depth); x->depth++; return 0;
    case 39: x->depth--; sp(x->cur, "  }\n"); return 0;
    case 40: {                                                              /* if b# */
        uint32_t q = *p;
        char nm[128];
        regname(x, q, NULL, nm, sizeof nm);
        if (rtype(q) == R_CONSTBOOL) sp(x->cur, "  if (%s) {\n", nm);
        else { S(a); sp(x->cur, "  if ((%s).x != 0.0) {\n", a); }
        return 0;
    }
    case 41: S(a); S(b); sp(x->cur, "  if ((%s).x %s (%s).x) {\n", a, cmpop(ctl), b); return 0;
    case 42: sp(x->cur, "  } else {\n"); return 0;
    case 43: sp(x->cur, "  }\n"); return 0;
    case 44: sp(x->cur, "  break;\n"); return 0;
    case 45: S(a); S(b); sp(x->cur, "  if ((%s).x %s (%s).x) break;\n", a, cmpop(ctl), b); return 0;
    case 46: dst(x, &p, &d); d.type = R_ADDR; S(a); assign(x, &d, a); return 0;     /* mova */
    case 31: case 81: case 47: case 48: return 0;                         /* dcl / def*: collected before */
    case 65: S(a); if (x->ps) sp(x->cur, "  if (any(lessThan((%s).xyz, vec3(0.0)))) discard;\n", a); return 0;   /* texkill */
    case 66: {                                                              /* texld / texldp / texldb */
        dst(x, &p, &d);
        if (x->major < 2) {
            if (((tok >> 24) & 0xF) == 2) {                                 /* ps 1.4 texld rN, tM */
                uint32_t r = rnum(p[-1]), t = rnum(p[0]);
                snprintf(e, sizeof e, "texture(s%u, in_t%u.xy)", r, t);
                assign(x, &d, e);
                return 0;
            }
            uint32_t t = rnum(p[-1]);                                       /* ps 1.1-1.3 tex tN */
            snprintf(e, sizeof e, "texture(s%u, in_t%u.xy)", t, t);
            sp(x->cur, "  t%u = %s;\n", t, e);
            return 0;
        }
        S(a);
        uint32_t sr = rnum(*p);
        sample(x, &d, a, sr, ctl == 1 ? 1 : ctl == 2 ? 2 : 0, NULL, NULL);
        return 0;
    }
    case 95: { dst(x, &p, &d); S(a); uint32_t sr = rnum(*p); sample(x, &d, a, sr, 3, NULL, NULL); return 0; }   /* texldl */
    case 93: { dst(x, &p, &d); S(a); uint32_t sr = rnum(*p++); S(b); S(c); sample(x, &d, a, sr, 4, b, c); return 0; }
    case 80: dst(x, &p, &d); S(a); S(b); S(c); snprintf(e, sizeof e, "mix(%s, %s, vec4(greaterThan(%s, vec4(0.5))))", c, b, a); assign(x, &d, e); return 0;
    case 88: dst(x, &p, &d); S(a); S(b); S(c); snprintf(e, sizeof e, "mix(%s, %s, vec4(greaterThanEqual(%s, vec4(0.0))))", c, b, a); assign(x, &d, e); return 0;
    case 90: dst(x, &p, &d); S(a); S(b); S(c); snprintf(e, sizeof e, "vec4(dot((%s).xy, (%s).xy) + (%s).x)", a, b, c); assign(x, &d, e); return 0;
    case 91: dst(x, &p, &d); S(a); snprintf(e, sizeof e, "dFdx(%s)", a); assign(x, &d, e); return 0;
    case 92: dst(x, &p, &d); S(a); snprintf(e, sizeof e, "dFdy(%s)", a); assign(x, &d, e); return 0;
    case 0xFFFD: return 0;                                                  /* phase (ps 1.4) */
    default:
        (void)d4; (void)end;
        snprintf(x->out->error, sizeof x->out->error, "opcode %u not translated", op);
        return -1;
    }
#undef S
}

/* ---- first pass: declarations ---- */
static uint32_t ilen(Ctx *x, const uint32_t *p) {
    uint32_t tok = *p, op = tok & 0xFFFF;
    if (op == 0xFFFE) return 1 + ((tok >> 16) & 0x7FFF);
    if (op == 81) return 6;                                  /* def: dst + 4 values (also correct for SM1) */
    if (x->major >= 2) return 1 + ((tok >> 24) & 0xF);
    const uint32_t *q = p + 1;                               /* SM1: parameters until the next instruction token */
    while (q < x->end && (*q & 0x80000000u)) q++;
    return (uint32_t)(q - p);
}

static void collect(Ctx *x, uint32_t *out_mask, uint32_t *labels, int *nlabels, int *depth_used) {
    for (const uint32_t *p = x->t + 1; p < x->end && *p != 0x0000FFFFu; p += ilen(x, p)) {
        uint32_t op = *p & 0xFFFF;
        if (op == 31) {
            uint32_t u = p[1], dreg = p[2], t = rtype(dreg), n = rnum(dreg);
            if (t == R_SAMPLER && n < 16) x->out->sampler_type[n] = (u >> 27) & 0xF;
            else if (t == R_INPUT && n < 16) {
                x->in_used[n] |= 1; x->in_usage[n] = u & 0x1F; x->in_index[n] = (u >> 16) & 0xF;
                x->in_centroid[n] = (dreg >> 22) & 1;
                if (x->ps && x->major < 3) { x->in_usage[n] = 10; x->in_index[n] = n; }   /* ps 2: v# = COLOR# */
            } else if (t == R_ADDR && x->ps && n < 16) {
                x->in_used[n] |= 2;                                                      /* ps 2: t# = TEXCOORD# */
            } else if (t == R_OUTPUT && !x->ps && n < 12) {
                x->out_used[n] = 1; x->out_usage[n] = u & 0x1F; x->out_index[n] = (u >> 16) & 0xF;
            }
        } else if (op == 81 && rnum(p[1]) < 256 && rtype(p[1]) == R_CONST) {
            x->def_f[rnum(p[1])] = 1;
            /* a global set in main(), not a const: older glslang (the NDK's shaderc) does not fold uintBitsToFloat */
            sp(&x->head, "vec4 cd%u;\n", rnum(p[1]));
            sp(&x->init, "  cd%u = uintBitsToFloat(uvec4(0x%08Xu, 0x%08Xu, 0x%08Xu, 0x%08Xu));\n", rnum(p[1]),
               p[2], p[3], p[4], p[5]);
        } else if (op == 48 && rnum(p[1]) < 16) {
            x->def_i[rnum(p[1])] = 1;
            sp(&x->head, "const ivec4 id%u = ivec4(%d, %d, %d, %d);\n", rnum(p[1]), (int)p[2], (int)p[3], (int)p[4], (int)p[5]);
        } else if (op == 47 && rnum(p[1]) < 16) {
            x->def_b[rnum(p[1])] = 1;
            sp(&x->head, "const bool bd%u = %s;\n", rnum(p[1]), p[2] ? "true" : "false");
        } else if (op == 30 && *nlabels < 64) {
            labels[(*nlabels)++] = rnum(p[1]);
        } else if (op != 0xFFFE && op != 31 && op != 81 && (*p & 0xFFFF) != 0) {
            uint32_t d = p[1];                                   /* first parameter is the destination for most ops */
            if (rtype(d) == R_COLOROUT && rnum(d) < 4) *out_mask |= 1u << rnum(d);
            if (rtype(d) == R_DEPTHOUT) *out_mask |= 16u;
        }
        if (op == 27 || op == 38) (*depth_used)++;
        if (op == 66 && x->ps && x->major < 2 && rnum(p[1]) < 8) {        /* ps 1.x tex / 1.4 texld: sampler + texcoord */
            x->out->sampler_type[rnum(p[1])] = 2;
            uint32_t t = ((*p >> 24) & 0xF) == 2 ? rnum(p[2]) : rnum(p[1]);
            if (t < 8) x->in_used[t] |= 2;
        }
    }
}

int sm3_translate(const uint32_t *tokens, uint32_t ntokens, Sm3Shader *out) {
    memset(out, 0, sizeof *out);
    Ctx x;
    memset(&x, 0, sizeof x);
    x.t = tokens;
    x.end = tokens + ntokens;
    x.out = out;
    uint32_t ver = tokens[0];
    x.ps = (ver >> 16) == 0xFFFF;
    x.major = (int)((ver >> 8) & 0xFF);
    out->is_ps = x.ps;
    out->major = x.major;
    out->minor = (int)(ver & 0xFF);
    int ps1 = x.ps && x.major < 2;                              /* ps 1.x: r0 is the colour output */
    if (ps1)                                                    /* and v0 / v1 are the colours, without dcl */
        for (int n = 0; n < 2; n++) { x.in_used[n] |= 1; x.in_usage[n] = 10; x.in_index[n] = (uint32_t)n; }
    uint32_t omask = 0, labels[64];
    int nlabels = 0, loops = 0;
    collect(&x, &omask, labels, &nlabels, &loops);

    /* body */
    x.cur = &x.body;
    sp(&x.body, "void main() {\n");
    if (x.init.s) sp(&x.body, "%s", x.init.s);
    if (x.ps) {
        sp(&x.body, "  vPos = vec4(gl_FragCoord.xy - 0.5, 0.0, 0.0);\n  vFace = vec4(gl_FrontFacing ? 1.0 : -1.0);\n");
        for (int n = 0; n < 16; n++) {
            if (x.in_used[n] & 1) sp(&x.body, "  v%d = in_v%d;\n", n, n);
            if (x.in_used[n] & 2) sp(&x.body, "  t%d = in_t%d;\n", n, n);
        }
    } else {
        for (int n = 0; n < 16; n++) if (x.in_used[n] & 1) sp(&x.body, "  v%d = in_v%d;\n", n, n);
    }
    for (const uint32_t *p = tokens + 1; p < x.end && *p != 0x0000FFFFu; p += ilen(&x, p)) {
        if ((*p & 0xFFFF) == 0xFFFE) continue;
        if (instr(&x, *p, p + 1, x.end)) { free(x.head.s); free(x.body.s); free(x.funcs.s); free(x.init.s); return -1; }
    }
    if (ps1) { omask |= 1; sp(&x.body, "  oC0 = r0;\n"); }
    if (x.cur == &x.body) sp(&x.body, "  finish();\n}\n");
    else if (x.in_label) sp(&x.funcs, "}\n");

    /* header */
    uint32_t pad_mask = 0;
    Str g = {0};
    sp(&g, "#version 450\n");
    sp(&g, "layout(std140, set = 0, binding = %d) uniform U { vec4 c[%d]; ivec4 i[16]; ivec4 b; vec4 misc; } K;\n",
       x.ps ? 1 : 0, x.ps ? 224 : 256);
    for (int s = 0; s < 16; s++) {
        uint32_t st = out->sampler_type[s];
        if (!st) continue;
        const char *ty = st == 3 ? "samplerCube" : st == 4 ? "sampler3D" : "sampler2D";
        if (x.ps) sp(&g, "layout(set = 0, binding = %d) uniform %s s%d;\n", 2 + s, ty, s);
        else if (s < 4) sp(&g, "layout(set = 0, binding = %d) uniform %s vs%d;\n", 18 + s, ty, s);
    }
    for (int n = 0; n < 16; n++) {
        if (x.in_used[n] & 1) {
            int loc = x.ps ? sm3_varying_location(x.in_usage[n], x.in_index[n]) : sm3_attr_location(x.in_usage[n], x.in_index[n]);
            if (loc >= 0) {
                sp(&g, "layout(location = %d) %sin vec4 in_v%d;\n", loc, x.in_centroid[n] ? "centroid " : "", n);
                if (!x.ps) out->inputs_mask |= 1u << loc;
            } else {
                sp(&g, "const vec4 in_v%d = vec4(0.0, 0.0, 0.0, 1.0);\n", n);
            }
        }
        if (x.in_used[n] & 2) sp(&g, "layout(location = %d) in vec4 in_t%d;\n", sm3_varying_location(5, (uint32_t)n), n);
    }
    sp(&g, "vec4 r0");
    for (int i = 1; i < 32; i++) sp(&g, ", r%d", i);
    sp(&g, ";\nvec4 v0");
    for (int i = 1; i < 16; i++) sp(&g, ", v%d", i);
    sp(&g, ";\nivec4 a0 = ivec4(0); int aL = 0; bvec4 p0;\n");
    if (x.ps) {
        sp(&g, "vec4 vPos, vFace, oC0, oC1, oC2, oC3; float oDepth;\nvec4 t0, t1, t2, t3, t4, t5, t6, t7, t8, t9;\n");
        for (int i = 0; i < 4; i++) if (omask & (1u << i)) sp(&g, "layout(location = %d) out vec4 out_oC%d;\n", i, i);
    } else {
        sp(&g, "vec4 oPos = vec4(0.0), oFog = vec4(0.0), oPts = vec4(1.0), oD0 = vec4(0.0), oD1 = vec4(0.0);\n");
        sp(&g, "vec4 oT0, oT1, oT2, oT3, oT4, oT5, oT6, oT7;\nvec4 o0, o1, o2, o3, o4, o5, o6, o7, o8, o9, o10, o11;\n");
        if (x.major >= 3) {
            for (int n = 0; n < 12; n++) {
                if (!x.out_used[n] || (x.out_usage[n] == 0 && x.out_index[n] == 0) || x.out_usage[n] == 4) continue;
                int loc = sm3_varying_location(x.out_usage[n], x.out_index[n]);
                if (loc >= 0) sp(&g, "layout(location = %d) out vec4 out_o%d;\n", loc, n);
            }
        } else {
            sp(&g, "layout(location = 10) out vec4 out_oD0;\nlayout(location = 11) out vec4 out_oD1;\n");
            sp(&g, "layout(location = 12) out vec4 out_oFog;\n");
            for (int n = 0; n < 8; n++) sp(&g, "layout(location = %d) out vec4 out_oT%d;\n", n, n);
        }
        /* every varying location a pixel shader may read is written (zero if this shader has none there): a
         * fragment input with no vertex output fails pipeline creation on some drivers (Adreno) */
        uint32_t written = x.major >= 3 ? 0 : 0x1CFFu;
        if (x.major >= 3)
            for (int n = 0; n < 12; n++)
                if (x.out_used[n] && !(x.out_usage[n] == 0 && x.out_index[n] == 0) && x.out_usage[n] != 4) {
                    int loc = sm3_varying_location(x.out_usage[n], x.out_index[n]);
                    if (loc >= 0) written |= 1u << loc;
                }
        for (int l = 0; l < 16; l++)
            if (!(written & (1u << l))) { sp(&g, "layout(location = %d) out vec4 out_pad%d;\n", l, l); pad_mask |= 1u << l; }
    }
    if (x.head.s) sp(&g, "%s", x.head.s);
    for (int i = 0; i < nlabels; i++) sp(&g, "void label_%u();\n", labels[i]);
    /* finish(): what the fixed-function end of the pipeline does after the shader */
    sp(&g, "void finish() {\n");
    if (x.ps) {
        sp(&g, "  int af = int(K.misc.y + 0.5);\n  if (af > 0 && af != 8) { float a = oC0.a, r = K.misc.x; bool ok =\n"
               "    af == 2 ? a < r : af == 3 ? a == r : af == 4 ? a <= r : af == 5 ? a > r : af == 6 ? a != r :\n"
               "    af == 7 ? a >= r : false; if (!ok) discard; }\n");
        sp(&g, "  if (K.misc.z > 0.5) oC0.rgb = mix(1.055 * pow(clamp(oC0.rgb, 0.0, 1.0), vec3(1.0 / 2.4)) - 0.055,\n"
               "    clamp(oC0.rgb, 0.0, 1.0) * 12.92, lessThanEqual(oC0.rgb, vec3(0.0031308)));\n");
        for (int i = 0; i < 4; i++) if (omask & (1u << i)) sp(&g, "  out_oC%d = oC%d;\n", i, i);
        if (omask & 16u) sp(&g, "  gl_FragDepth = oDepth;\n");
    } else {
        if (x.major >= 3) {
            for (int n = 0; n < 12; n++) {
                if (!x.out_used[n]) continue;
                if (x.out_usage[n] == 0 && x.out_index[n] == 0) sp(&g, "  gl_Position = o%d;\n", n);
                else if (x.out_usage[n] == 4) sp(&g, "  gl_PointSize = o%d.x;\n", n);
                else if (sm3_varying_location(x.out_usage[n], x.out_index[n]) >= 0) sp(&g, "  out_o%d = o%d;\n", n, n);
            }
        } else {
            sp(&g, "  gl_Position = oPos; out_oD0 = oD0; out_oD1 = oD1; out_oFog = oFog;\n");
            for (int n = 0; n < 8; n++) sp(&g, "  out_oT%d = oT%d;\n", n, n);
        }
        for (int l = 0; l < 16; l++) if (pad_mask & (1u << l)) sp(&g, "  out_pad%d = vec4(0.0);\n", l);
        sp(&g, "  gl_PointSize = 1.0;\n  gl_Position.xy *= K.misc.zw;\n  gl_Position.xy += K.misc.xy * gl_Position.w;\n");
    }
    sp(&g, "}\n");
    sp(&g, "%s", x.body.s);
    if (x.funcs.s) sp(&g, "%s", x.funcs.s);
    free(x.head.s); free(x.body.s); free(x.funcs.s); free(x.init.s);
    out->glsl = g.s;
    (void)loops;
    return 0;
}
