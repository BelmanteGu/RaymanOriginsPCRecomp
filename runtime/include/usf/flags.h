/* Lazy x86 flags for lifted code.
 *
 * What it is for: an instruction that sets flags does not compute them. It records what it did in four locals of
 * the lifted function (the operation kind with its size, the result and the two operands); a consumer (jcc, setcc,
 * cmovcc, adc/sbb) asks for the one condition it needs. Inside a block the kind is a constant, so clang folds the
 * switch below to the one comparison the original code meant (`cmp eax,ebx; jl` becomes `(int32_t)a < (int32_t)b`),
 * and flags nobody reads are dead stores that disappear.
 * What it does not do: keep flags across calls or returns (MSVC never relies on that), or model AF (the BCD flag;
 * only `daa`/`aaa` read it and the game has none).
 */
#ifndef USF_FLAGS_H
#define USF_FLAGS_H

#include <stdint.h>

/* kind = op | size<<4, size in bytes (1, 2, 4) */
enum {
    FK_ADD = 1, FK_SUB, FK_LOGIC, FK_INC, FK_DEC, FK_ADC, FK_SBB, FK_SHL, FK_SHR, FK_SAR, FK_MUL, FK_NEG,
    FK_RAW,   /* res holds materialised EFLAGS bits (comiss, sahf, popf, bt...) */
};
#define FK(op, sz) ((uint32_t)(op) | ((uint32_t)(sz) << 4))

#define EF_CF 0x001u
#define EF_PF 0x004u
#define EF_ZF 0x040u
#define EF_SF 0x080u
#define EF_OF 0x800u

static inline uint32_t fl_mask(uint32_t sz) { return sz == 4 ? 0xFFFFFFFFu : sz == 2 ? 0xFFFFu : 0xFFu; }
static inline uint32_t fl_sign(uint32_t sz) { return sz == 4 ? 0x80000000u : sz == 2 ? 0x8000u : 0x80u; }

/* For FK_INC / FK_DEC, b holds the carry flag from before (inc/dec leave CF alone).
 * For shifts, b holds the count (1..31) and a the value before the shift. */
static inline uint32_t fl_cf(uint32_t k, uint32_t r, uint32_t a, uint32_t b) {
    uint32_t sz = k >> 4, m = fl_mask(sz);
    switch (k & 15) {
    case FK_ADD: return (r & m) < (a & m);
    case FK_SUB: return (a & m) < (b & m);
    case FK_ADC: return ((uint64_t)(a & m) + (b & m) + ((r & m) != ((a + b) & m))) > m;
    case FK_SBB: return (uint64_t)(a & m) < (uint64_t)(b & m) + (((a - b) & m) != (r & m));
    case FK_INC: case FK_DEC: return b & 1;
    case FK_SHL: return (a >> (sz * 8 - b)) & 1;
    case FK_SHR: case FK_SAR: return (a >> (b - 1)) & 1;
    case FK_MUL: return a != 0;     /* a = "upper half is not the sign/zero extension of the lower half" */
    case FK_NEG: return (a & m) != 0;
    case FK_RAW: return r & EF_CF;
    default: return 0;              /* FK_LOGIC */
    }
}

static inline uint32_t fl_zf(uint32_t k, uint32_t r) {
    if ((k & 15) == FK_RAW) return (r & EF_ZF) != 0;
    return (r & fl_mask(k >> 4)) == 0;
}

static inline uint32_t fl_sf(uint32_t k, uint32_t r) {
    if ((k & 15) == FK_RAW) return (r & EF_SF) != 0;
    return (r & fl_sign(k >> 4)) != 0;
}

static inline uint32_t fl_of(uint32_t k, uint32_t r, uint32_t a, uint32_t b) {
    uint32_t s = fl_sign(k >> 4);
    switch (k & 15) {
    case FK_ADD: case FK_ADC: return ((a ^ r) & (b ^ r) & s) != 0;
    case FK_SUB: case FK_SBB: return ((a ^ b) & (a ^ r) & s) != 0;
    case FK_INC: return (r & fl_mask(k >> 4)) == s;
    case FK_DEC: return (r & fl_mask(k >> 4)) == s - 1;
    case FK_NEG: return (r & fl_mask(k >> 4)) == s;
    case FK_SHL: return (((r & s) != 0) ^ fl_cf(k, r, a, b));          /* defined for count 1 */
    case FK_SHR: return (a & s) != 0;                                   /* defined for count 1 */
    case FK_MUL: return a != 0;
    case FK_RAW: return (r & EF_OF) != 0;
    default: return 0;              /* FK_LOGIC, FK_SAR */
    }
}

static inline uint32_t fl_pf(uint32_t k, uint32_t r) {
    if ((k & 15) == FK_RAW) return (r & EF_PF) != 0;
    return !__builtin_parity(r & 0xFF);
}

/* The 16 conditions, by their x86 encoding order. */
#define CC_O(k, r, a, b) fl_of(k, r, a, b)
#define CC_NO(k, r, a, b) (!fl_of(k, r, a, b))
#define CC_B(k, r, a, b) fl_cf(k, r, a, b)
#define CC_AE(k, r, a, b) (!fl_cf(k, r, a, b))
#define CC_E(k, r, a, b) fl_zf(k, r)
#define CC_NE(k, r, a, b) (!fl_zf(k, r))
#define CC_BE(k, r, a, b) (fl_cf(k, r, a, b) | fl_zf(k, r))
#define CC_A(k, r, a, b) (!(fl_cf(k, r, a, b) | fl_zf(k, r)))
#define CC_S(k, r, a, b) fl_sf(k, r)
#define CC_NS(k, r, a, b) (!fl_sf(k, r))
#define CC_P(k, r, a, b) fl_pf(k, r)
#define CC_NP(k, r, a, b) (!fl_pf(k, r))
#define CC_L(k, r, a, b) (fl_sf(k, r) != fl_of(k, r, a, b))
#define CC_GE(k, r, a, b) (fl_sf(k, r) == fl_of(k, r, a, b))
#define CC_LE(k, r, a, b) (fl_zf(k, r) | (fl_sf(k, r) != fl_of(k, r, a, b)))
#define CC_G(k, r, a, b) (!fl_zf(k, r) & (fl_sf(k, r) == fl_of(k, r, a, b)))

/* All flags as EFLAGS bits (pushfd, lahf, and before an instruction that changes only some of them). */
static inline uint32_t fl_eflags(uint32_t k, uint32_t r, uint32_t a, uint32_t b) {
    if ((k & 15) == FK_RAW) return r | 2;
    return 2 | fl_cf(k, r, a, b) * EF_CF | fl_pf(k, r) * EF_PF | fl_zf(k, r) * EF_ZF | fl_sf(k, r) * EF_SF |
           fl_of(k, r, a, b) * EF_OF;
}

#endif
