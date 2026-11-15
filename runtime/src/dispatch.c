/* Control transfers out of lifted code: indirect calls, thunks (imports and our own functions), calls from the
 * runtime back into game code, and the stop for an address nobody knows.
 *
 * What it does: usf_icall finds a guest address either in the thunk page (runs the HLE function with the stdcall /
 * cdecl stack rules) or in the lifted function table (binary search); guest_call lets HLE code call a game function
 * (window procedures, thread start routines, callbacks) with arguments; usf_unknown prints the state and stops.
 * What it does not do: implement any API (hle_*.c) or unwind exceptions (not yet).
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hle.h"
#include "usf/table.h"

uint32_t g_text_lo, g_text_hi;                                /* the executable's code section (loader.c) */

typedef struct {
    char label[64];
    HleFn fn;
    int nargs, conv;
    uint32_t calls;
} Thunk;

static Thunk thunks[THUNK_SIZE / 16];
static uint32_t nthunks;
static pthread_mutex_t thunk_lock = PTHREAD_MUTEX_INITIALIZER;
int usf_log_on;

uint32_t usf_thunk_fn(const char *label, HleFn fn, int nargs, int conv) {
    pthread_mutex_lock(&thunk_lock);
    for (uint32_t i = 0; i < nthunks; i++)
        if (thunks[i].fn == fn && fn && !strcmp(thunks[i].label, label)) {
            pthread_mutex_unlock(&thunk_lock);
            return THUNK_BASE + 16u * i;
        }
    if (nthunks >= THUNK_SIZE / 16) hle_fatal("out of thunks");
    Thunk *t = &thunks[nthunks];
    snprintf(t->label, sizeof t->label, "%s", label);
    t->fn = fn;
    t->nargs = nargs;
    t->conv = conv;
    uint32_t addr = THUNK_BASE + 16u * nthunks++;
    pthread_mutex_unlock(&thunk_lock);
    return addr;
}

uint32_t usf_thunk(const char *dll, const char *name) {
    const HleImport *imp = hle_find(dll, name);
    char label[64];
    snprintf(label, sizeof label, "%s!%s", dll, name);
    if (!imp) {
        usf_log("import with no entry in the tables: %s (treated as stdcall with 0 arguments)\n", label);
        return usf_thunk_fn(label, NULL, 0, CONV_STDCALL);
    }
    return usf_thunk_fn(label, imp->fn, imp->nargs, imp->conv);
}

int usf_thunk_call(CPU *c, uint32_t target) {
    if (target < THUNK_BASE || target >= THUNK_BASE + 16u * nthunks || (target & 15u)) return 0;
    Thunk *t = &thunks[(target - THUNK_BASE) / 16u];
    uint32_t a = c->esp + 4u;
    t->calls++;
    if (usf_log_on)
        fprintf(stderr, "[%08x] %s(%08x, %08x, %08x, %08x)\n", R32(c->esp), t->label, R32(a), R32(a + 4), R32(a + 8),
                R32(a + 12));
    uint32_t r = 0;
    if (t->fn) {
        r = t->fn(c, a);
    } else if (t->calls == 1) {
        fprintf(stderr, "usf: NOT IMPLEMENTED %s (called from %08x), returning 0\n", t->label, R32(c->esp));
    }
    c->eax = r;
    if (t->conv == CONV_CALLSITE) c->esp += 4u + callsite_args(R32(c->esp));
    else c->esp += 4u + (t->conv == CONV_STDCALL ? 4u * (uint32_t)t->nargs : 0u);
    return 1;
}

static void (*find(const USFEntry *t, unsigned n, uint32_t a))(CPU *) {
    unsigned lo = 0, hi = n;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (t[mid].addr < a) lo = mid + 1; else hi = mid;
    }
    return lo < n && t[lo].addr == a ? t[lo].fn : NULL;
}

static void (*lookup(uint32_t a))(CPU *) {
    void (*fn)(CPU *) = find(usf_table, usf_table_count, a);
    return fn;
}

void usf_icall(CPU *c, uint32_t target) {
    if (usf_thunk_call(c, target)) return;
    void (*fn)(CPU *) = lookup(target);
    if (!fn) usf_unknown(c, target, "indirect call to an address with no lifted function");
    fn(c);
}

/* Calls game code at `addr` with `n` dword arguments, as the game would (pushes the arguments right to left and a
 * return address). conv says who pops the arguments. Returns eax. */
uint32_t guest_call(CPU *c, uint32_t addr, int conv, int n, ...) {
    uint32_t args[16];
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    uint32_t esp0 = c->esp;
    for (int i = n - 1; i >= 0; i--) { c->esp -= 4u; W32(c->esp, args[i]); }
    c->esp -= 4u;
    W32(c->esp, THUNK_BASE - 16u);         /* a return address that is never jumped to: the C call returns */
    usf_icall(c, addr);
    if (conv == CONV_CDECL) c->esp += 4u * (uint32_t)n;
    if (c->esp != esp0)
        usf_log("guest_call %08x: stack moved by %d bytes (wrong convention?)\n", addr, (int)(c->esp - esp0));
    c->esp = esp0;
    return c->eax;
}

uint32_t guest_thiscall(CPU *c, uint32_t obj, uint32_t addr, int n, ...) {
    uint32_t args[16];
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    uint32_t esp0 = c->esp, ecx0 = c->ecx;
    for (int i = n - 1; i >= 0; i--) { c->esp -= 4u; W32(c->esp, args[i]); }
    c->esp -= 4u;
    W32(c->esp, THUNK_BASE - 16u);
    c->ecx = obj;
    usf_icall(c, addr);
    c->esp = esp0;
    c->ecx = ecx0;
    return c->eax;
}

void usf_unknown(CPU *c, uint32_t at, const char *why) {
    fprintf(stderr, "\nusf: STOP at %08x: %s\n", at, why);
    if (strstr(why, "no lifted function") && (at >= g_text_lo && at < g_text_hi)) {
        /* inside a code section: feed it back to function discovery */
        const char *path = getenv("USF_EXTRA_STARTS");
        FILE *f = fopen(path ? path : "/tmp/rayman_extra_starts.txt", "a");
        if (f) { fprintf(f, "%x  indirect target seen by the runtime\n", at); fclose(f); }
    }
    fprintf(stderr, "  eax %08x ecx %08x edx %08x ebx %08x\n  esp %08x ebp %08x esi %08x edi %08x\n", c->eax, c->ecx,
            c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi);
    fprintf(stderr, "  stack:");
    for (int i = 0; i < 8; i++) fprintf(stderr, " %08x", R32(c->esp + 4u * i));
    fprintf(stderr, "\n");
    fflush(stderr);
    _exit(70);
}

void usf_cpuid(CPU *c, uint32_t *a, uint32_t *b, uint32_t *cc, uint32_t *d) {
    (void)c;
    uint32_t leaf = *a;
    *a = *b = *cc = *d = 0;
    if (leaf == 0) { *a = 1; *b = 0x756E6547; *d = 0x49656E69; *cc = 0x6C65746E; }      /* GenuineIntel */
    else if (leaf == 1) { *a = 0x000306A9; *b = 0x00040800; *d = 0x078BFBFF; *cc = 0x00000001; }  /* SSE, SSE2, SSE3 */
    else if (leaf == 0x80000000u) { *a = 0x80000000u; }
}

double usf_f80_load(uint32_t addr) {
    const uint8_t *p = HOST(addr);
    uint64_t m;
    memcpy(&m, p, 8);
    uint16_t se = (uint16_t)(p[8] | p[9] << 8);
    int e = se & 0x7FFF;
    double sign = (se & 0x8000) ? -1.0 : 1.0;
    if (e == 0 && m == 0) return sign * 0.0;
    if (e == 0x7FFF) return (m << 1) ? __builtin_nan("") : sign * __builtin_inf();
    return sign * __builtin_ldexp((double)m, e - 16383 - 63);
}

void usf_f80_store(uint32_t addr, double v) {
    uint8_t *p = HOST(addr);
    uint64_t m = 0;
    uint16_t se = 0;
    if (v != v) { se = 0x7FFF; m = 0xC000000000000000ull; }
    else {
        if (__builtin_signbit(v)) { se = 0x8000; v = -v; }
        if (v == __builtin_inf()) { se |= 0x7FFF; m = 0x8000000000000000ull; }
        else if (v != 0.0) {
            int e;
            double f = __builtin_frexp(v, &e);              /* v = f * 2^e, 0.5 <= f < 1 */
            m = (uint64_t)__builtin_ldexp(f, 64);
            se |= (uint16_t)(e - 1 + 16383);
        }
    }
    memcpy(p, &m, 8);
    p[8] = (uint8_t)se;
    p[9] = (uint8_t)(se >> 8);
}
