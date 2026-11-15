/* x87 floating point for lifted code: the register stack, precision control and integer conversions.
 *
 * What it is for: the game's x87 code (float returns in st(0), int<->float conversions, some double math) runs on
 * an 8-entry stack of doubles in CPU. Precision control is honoured: Direct3D 9 switches the x87 unit to 24-bit
 * (single) precision when a device is created without D3DCREATE_FPU_PRESERVE, so after that every x87 result is
 * rounded to float, exactly as the hardware does; with 53-bit precision results stay doubles.
 * What it does not do: 64-bit (extended) precision. A program that sets it gets double results; the lifter's census
 * reports any `fld`/`fstp` of 80-bit memory, which is the only place extended values become visible.
 */
#ifndef USF_X87_H
#define USF_X87_H

#include <math.h>
#include <stdint.h>

#include "cpu.h"

#define ST(i) (c->st[(c->top + (i)) & 7u])
#define FPUSH(v) do { double _v = (v); c->top = (c->top - 1u) & 7u; ST(0) = _v; } while (0)
#define FPOP() (c->top = (c->top + 1u) & 7u)

/* Round to the precision the control word asks for (PC bits 8-9: 00 = 24-bit, 10 = 53-bit, 11 = 64-bit). */
static inline double x87_round(const CPU *c, double v) {
    return ((c->fpu_cw >> 8) & 3u) == 0u ? (double)(float)v : v;
}
#define X87R(v) x87_round(c, (v))

/* Integer conversion with the rounding mode of the control word (RC bits 10-11). Out of range gives the
 * "integer indefinite" value, as the hardware does. */
static inline int64_t x87_to_int(const CPU *c, double v, int bits) {
    double r;
    switch ((c->fpu_cw >> 10) & 3u) {
    case 0: r = nearbyint(v); break;      /* the host default is round-to-nearest-even */
    case 1: r = floor(v); break;
    case 2: r = ceil(v); break;
    default: r = trunc(v); break;
    }
    double lim = bits == 16 ? 32768.0 : bits == 32 ? 2147483648.0 : 9223372036854775808.0;
    if (!(r >= -lim && r < lim))
        return bits == 16 ? INT16_MIN : bits == 32 ? INT32_MIN : INT64_MIN;
    return (int64_t)r;
}

static inline double x87_frndint(const CPU *c, double v) {
    switch ((c->fpu_cw >> 10) & 3u) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}

/* fxam: C3 C2 C0 class bits (zero, normal, infinity, NaN) and C1 = sign. */
static inline uint32_t x87_examine(double v) {
    uint32_t sign = signbit(v) ? 0x0200u : 0u;
    if (v != v) return sign | 0x0100u;
    if (isinf(v)) return sign | 0x0500u;
    if (v == 0.0) return sign | 0x4000u;
    return sign | 0x0400u;
}

/* fcom family: condition bits in the status word (C3 = equal, C0 = less, all of C0/C2/C3 = unordered). */
static inline void x87_compare(CPU *c, double a, double b) {
    uint16_t sw = c->fpu_sw & ~0x4500u;
    if (a != a || b != b) sw |= 0x4500u;
    else if (a < b) sw |= 0x0100u;
    else if (a == b) sw |= 0x4000u;
    c->fpu_sw = sw;
}

/* fnstsw: the status word with TOP in bits 11-13. */
static inline uint32_t x87_status(const CPU *c) { return (c->fpu_sw & ~0x3800u) | ((c->top & 7u) << 11); }

/* 80-bit extended values in memory (fld/fstp tbyte). */
double usf_f80_load(uint32_t addr);
void usf_f80_store(uint32_t addr, double v);

/* SSE truncating conversion: out of range or NaN gives 0x80000000. */
static inline uint32_t sse_cvtt(double v) {
    return (v > -2147483649.0 && v < 2147483648.0) ? (uint32_t)(int32_t)v : 0x80000000u;
}
static inline uint32_t sse_cvt(double v) {   /* round to nearest even (MXCSR default) */
    return sse_cvtt(nearbyint(v));
}

/* comiss / ucomiss / comisd / ucomisd, and fcomi: ZF PF CF as EFLAGS bits (OF SF AF cleared). */
static inline uint32_t fp_compare_eflags(double a, double b) {
    if (a != a || b != b) return 0x45u;      /* ZF | PF | CF */
    if (a < b) return 0x01u;
    if (a == b) return 0x40u;
    return 0u;
}

#endif
