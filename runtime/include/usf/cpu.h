/* The x86-32 machine state the recompiled code runs on, and how it reaches guest memory.
 *
 * What it is for: every lifted function is `void f_<addr>(CPU *c)`. It loads the registers it uses from `c` into
 * locals at entry, keeps them in locals (so clang allocates host registers), and stores them back before a call,
 * an indirect jump and its return. Guest memory is one 4 GiB reservation: guest address `a` is host `usf_mem + a`,
 * so every 32-bit pointer the game stores stays valid and the image sits at its own base (0x00400000).
 * What it does not do: decide where the stack or heap live (the runtime does) or model segments other than fs
 * (the TEB, used by SEH and TLS).
 */
#ifndef USF_CPU_H
#define USF_CPU_H

#include <stdint.h>
#include <string.h>

typedef union {
    float f[4];
    double d[2];
    uint8_t b[16];
    uint16_t w[8];
    uint32_t u[4];
    int32_t i[4];
    uint64_t q[2];
} XMM;

typedef struct CPU {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t df;            /* direction flag (rep movs/stos direction) */
    uint32_t fk, fr, fa, fb; /* lazy flags (flags.h) across calls and returns: the CRT's hand-written assembly
                               (math helpers) tests flags its callee set */
    uint32_t fs_base;       /* guest address of the current thread's TEB */
    XMM xmm[8];
    uint64_t mm[8];          /* MMX registers (kept apart from the x87 stack; only Crypto++ uses MMX) */
    uint32_t mxcsr;
    /* x87: values are kept as doubles (see x87.h for what that changes) */
    double st[8];
    uint32_t top;           /* index of st(0) in st[] */
    uint16_t fpu_cw;        /* control word (rounding mode for fist/fistp, precision) */
    uint16_t fpu_sw;        /* status word condition bits C0..C3 from the last compare */
} CPU;

/* Base of the 4 GiB guest memory reservation (set by the runtime before any lifted code runs). */
extern uint8_t *usf_mem;

typedef uint32_t u32a __attribute__((aligned(1), may_alias));
typedef uint16_t u16a __attribute__((aligned(1), may_alias));
typedef uint64_t u64a __attribute__((aligned(1), may_alias));
typedef float f32a __attribute__((aligned(1), may_alias));
typedef double f64a __attribute__((aligned(1), may_alias));

#define HOST(a) (usf_mem + (uint32_t)(a))
#define R8(a) (*(uint8_t *)HOST(a))
#define R16(a) (*(u16a *)HOST(a))
#define R32(a) (*(u32a *)HOST(a))
#define R64(a) (*(u64a *)HOST(a))
#define RF32(a) (*(f32a *)HOST(a))
#define RF64(a) (*(f64a *)HOST(a))
#define W8(a, v) (*(uint8_t *)HOST(a) = (uint8_t)(v))
#define W16(a, v) (*(u16a *)HOST(a) = (uint16_t)(v))
#define W32(a, v) (*(u32a *)HOST(a) = (uint32_t)(v))
#define W64(a, v) (*(u64a *)HOST(a) = (uint64_t)(v))
#define WF32(a, v) (*(f32a *)HOST(a) = (float)(v))
#define WF64(a, v) (*(f64a *)HOST(a) = (double)(v))

static inline XMM xmm_load(uint32_t a) { XMM x; memcpy(&x, HOST(a), 16); return x; }
static inline void xmm_store(uint32_t a, XMM x) { memcpy(HOST(a), &x, 16); }

/* Calls that leave lifted code: indirect calls/jumps through the dispatch table, import thunks, and the trap for
 * an address the lifter never saw (reported with the address so it can be added to the function list). */
void usf_icall(CPU *c, uint32_t target);
void usf_unknown(CPU *c, uint32_t at, const char *why) __attribute__((noreturn));
/* cpuid: the runtime answers with a fixed, conservative x86 feature set (SSE2, no later extensions). */
void usf_cpuid(CPU *c, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx);

#endif
