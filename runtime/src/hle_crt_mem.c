/* msvcr90.dll: memory, strings (narrow, wide, "multibyte"), character classes, number conversions, sorting, rand.
 *
 * What it does: the cdecl functions of the C runtime that work on memory and strings, on guest memory. Wide strings
 * are 16-bit (Windows wchar_t), so the wide functions are written here rather than taken from the host. The
 * "multibyte" (_mbs*) functions are answered for a single-byte code page (1252): no lead bytes. qsort and bsearch
 * call the game's comparison function back.
 * What it does not do: locales (the "C" locale only).
 */
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "hle_crt.h"

#define D(name) uint32_t x_##name(CPU *c, uint32_t a)

/* ---- heap ---- */
D(malloc) { (void)c; return gmalloc(ARG(0) ? ARG(0) : 1); }
D(calloc) { (void)c; uint64_t n = (uint64_t)ARG(0) * ARG(1); return n >> 31 ? 0 : gcalloc(n ? (uint32_t)n : 1); }
D(free) { (void)c; if (ARG(0)) gfree(ARG(0)); return 0; }
D(realloc) {
    (void)c;
    if (!ARG(0)) return gmalloc(ARG(1) ? ARG(1) : 1);
    if (!ARG(1)) { gfree(ARG(0)); return 0; }
    return grealloc(ARG(0), ARG(1));
}
D(_msize) { (void)c; return gsize(ARG(0)); }
D(_aligned_malloc) {                                           /* (size, alignment): the real block is kept before */
    (void)c;
    uint32_t al = ARG(1) < 4 ? 4 : ARG(1), raw = gmalloc(ARG(0) + al + 4);
    if (!raw) return 0;
    uint32_t p = (raw + 4 + al - 1) & ~(al - 1);
    W32(p - 4, raw);
    return p;
}
D(_aligned_free) { (void)c; if (ARG(0)) gfree(R32(ARG(0) - 4)); return 0; }

/* ---- memory ---- */
D(memset) { (void)c; memset(HOST(ARG(0)), (int)ARG(1), ARG(2)); return ARG(0); }
D(memcpy) { (void)c; memmove(HOST(ARG(0)), HOST(ARG(1)), ARG(2)); return ARG(0); }
D(memmove) { (void)c; memmove(HOST(ARG(0)), HOST(ARG(1)), ARG(2)); return ARG(0); }
D(memmove_s) {                                                 /* (dst, dstsize, src, count) */
    (void)c;
    if (ARG(3) > ARG(1)) return 34;                            /* ERANGE */
    memmove(HOST(ARG(0)), HOST(ARG(2)), ARG(3));
    return 0;
}
D(memchr) {
    (void)c;
    const uint8_t *p = memchr(HOST(ARG(0)), (int)(ARG(1) & 0xFF), ARG(2));
    return p ? (uint32_t)(p - usf_mem) : 0;
}

/* ---- narrow strings ---- */
static uint32_t g(const char *p) { return p ? (uint32_t)((const uint8_t *)p - usf_mem) : 0; }
D(strcmp) { (void)c; return (uint32_t)strcmp(STR(ARG(0)), STR(ARG(1))); }
D(strncmp) { (void)c; return (uint32_t)strncmp(STR(ARG(0)), STR(ARG(1)), ARG(2)); }
D(_stricmp) { (void)c; return (uint32_t)strcasecmp(STR(ARG(0)), STR(ARG(1))); }
D(_strnicmp) { (void)c; return (uint32_t)strncasecmp(STR(ARG(0)), STR(ARG(1)), ARG(2)); }
D(_mbsicmp) { (void)c; return (uint32_t)strcasecmp(STR(ARG(0)), STR(ARG(1))); }
D(_mbscmp) { (void)c; return (uint32_t)strcmp(STR(ARG(0)), STR(ARG(1))); }
D(strchr) { (void)c; return g(strchr(STR(ARG(0)), (int)(ARG(1) & 0xFF))); }
D(_mbschr) { (void)c; return g(strchr(STR(ARG(0)), (int)(ARG(1) & 0xFF))); }
D(strrchr) { (void)c; return g(strrchr(STR(ARG(0)), (int)(ARG(1) & 0xFF))); }
D(strstr) { (void)c; return g(strstr(STR(ARG(0)), STR(ARG(1)))); }
D(_mbsstr) { (void)c; return g(strstr(STR(ARG(0)), STR(ARG(1)))); }
D(strncpy) { (void)c; strncpy((char *)HOST(ARG(0)), STR(ARG(1)), ARG(2)); return ARG(0); }
D(_mbsnbcpy) { (void)c; strncpy((char *)HOST(ARG(0)), STR(ARG(1)), ARG(2)); return ARG(0); }
D(strncat) { (void)c; strncat((char *)HOST(ARG(0)), STR(ARG(1)), ARG(2)); return ARG(0); }
D(strcat_s) {                                                  /* (dst, size, src) */
    (void)c;
    size_t dl = strnlen(STR(ARG(0)), ARG(1)), sl = strlen(STR(ARG(2)));
    if (dl + sl + 1 > ARG(1)) { if (ARG(1)) W8(ARG(0), 0); return 34; }
    memcpy(HOST(ARG(0) + dl), STR(ARG(2)), sl + 1);
    return 0;
}
D(_strdup) {
    (void)c;
    if (!ARG(0)) return 0;
    size_t n = strlen(STR(ARG(0))) + 1;
    uint32_t p = gmalloc((uint32_t)n);
    if (p) memcpy(HOST(p), STR(ARG(0)), n);
    return p;
}
D(_ismbblead) { (void)c; (void)a; return 0; }
D(strerror) {
    (void)c;
    static uint32_t buf;
    if (!buf) buf = gmalloc(128);
    snprintf((char *)HOST(buf), 128, "%s", strerror((int)ARG(0)));
    return buf;
}
D(_wcserror) {
    (void)c;
    static uint32_t buf;
    if (!buf) buf = gmalloc(256);
    utf8_to_w(strerror((int)ARG(0)), buf, 128);
    return buf;
}

/* ---- wide strings (UTF-16) ---- */
size_t crt_wlen(uint32_t w) { size_t n = 0; while (R16(w + 2 * n)) n++; return n; }
void crt_w2u(uint32_t w, char *out, size_t n) { w2utf8(w, out, n); }

D(wcscpy_s) {                                                  /* (dst, count, src) */
    (void)c;
    size_t n = crt_wlen(ARG(2));
    if (n + 1 > ARG(1)) { if (ARG(1)) W16(ARG(0), 0); return 34; }
    memmove(HOST(ARG(0)), HOST(ARG(2)), 2 * (n + 1));
    return 0;
}
D(wcsncpy_s) {                                                 /* (dst, count, src, max); _TRUNCATE = ~0 */
    (void)c;
    size_t n = crt_wlen(ARG(2));
    if (n > ARG(3)) n = ARG(3);
    if (n + 1 > ARG(1)) {
        if (ARG(3) != 0xFFFFFFFFu) { if (ARG(1)) W16(ARG(0), 0); return 34; }
        n = ARG(1) ? ARG(1) - 1 : 0;
    }
    memmove(HOST(ARG(0)), HOST(ARG(2)), 2 * n);
    W16(ARG(0) + 2 * n, 0);
    return 0;
}
D(wcsrchr) {
    (void)c;
    uint32_t r = 0, ch = ARG(1) & 0xFFFF;
    for (uint32_t p = ARG(0);; p += 2) {
        if (R16(p) == ch) r = p;
        if (!R16(p)) return r;
    }
}
D(wcsstr) {
    (void)c;
    size_t n = crt_wlen(ARG(1));
    if (!n) return ARG(0);
    for (uint32_t p = ARG(0); R16(p); p += 2)
        if (!memcmp(HOST(p), HOST(ARG(1)), 2 * n)) return p;
    return 0;
}
static uint32_t wlower(uint32_t ch) { return ch < 0x80 ? (uint32_t)tolower((int)ch) : (ch >= 0xC0 && ch <= 0xDE && ch != 0xD7) ? ch + 32 : ch; }
D(towlower) { (void)c; return wlower(ARG(0) & 0xFFFF); }
D(wcstombs_s) {                                                /* (&converted, dst, dstsize, src, count) */
    (void)c;
    char tmp[4096];
    w2utf8(ARG(3), tmp, sizeof tmp);
    size_t n = strlen(tmp);
    if (ARG(4) != 0xFFFFFFFFu && n > ARG(4)) n = ARG(4);
    if (ARG(1)) {
        if (n + 1 > ARG(2)) n = ARG(2) ? ARG(2) - 1 : 0;
        memcpy(HOST(ARG(1)), tmp, n);
        W8(ARG(1) + n, 0);
    }
    if (ARG(0)) W32(ARG(0), (uint32_t)n + 1);
    return 0;
}

/* ---- character classes ("C" locale) ---- */
D(isupper) { (void)c; return ARG(0) < 256 && isupper((int)ARG(0)) ? 1 : 0; }
D(isalpha) { (void)c; return ARG(0) < 256 && isalpha((int)ARG(0)) ? 2 : 0; }
D(isalnum) { (void)c; return ARG(0) < 256 && isalnum((int)ARG(0)) ? 1 : 0; }
D(isdigit) { (void)c; return ARG(0) < 256 && isdigit((int)ARG(0)) ? 4 : 0; }
D(isxdigit) { (void)c; return ARG(0) < 256 && isxdigit((int)ARG(0)) ? 0x80 : 0; }
D(isspace) { (void)c; return ARG(0) < 256 && isspace((int)ARG(0)) ? 8 : 0; }
D(tolower) { (void)c; return ARG(0) < 256 ? (uint32_t)tolower((int)ARG(0)) : ARG(0); }

/* ---- numbers ---- */
#include "usf/x87.h"
D(atoi) { (void)c; return (uint32_t)atoi(STR(ARG(0))); }
D(atol) { (void)c; return (uint32_t)atoi(STR(ARG(0))); }
D(strtol) {
    const char *s = STR(ARG(0));
    char *end;
    errno = 0;
    long long v = strtoll(s, &end, (int)ARG(2));
    if (v > 0x7FFFFFFFLL) { v = 0x7FFFFFFF; errno = ERANGE; }
    if (v < -0x80000000LL) { v = -0x80000000LL; errno = ERANGE; }
    if (errno) crt_set_errno(c, errno);
    if (ARG(1)) W32(ARG(1), ARG(0) + (uint32_t)(end - s));
    return (uint32_t)(int32_t)v;
}
D(strtoul) {
    const char *s = STR(ARG(0));
    char *end;
    unsigned long long v = strtoull(s, &end, (int)ARG(2));
    if (v > 0xFFFFFFFFull && v < 0xFFFFFFFF00000000ull) { v = 0xFFFFFFFFu; crt_set_errno(c, ERANGE); }
    if (ARG(1)) W32(ARG(1), ARG(0) + (uint32_t)(end - s));
    return (uint32_t)v;
}
D(_strtoi64) {
    const char *s = STR(ARG(0));
    char *end;
    long long v = strtoll(s, &end, (int)ARG(2));
    if (ARG(1)) W32(ARG(1), ARG(0) + (uint32_t)(end - s));
    return ret64(c, (uint64_t)v);
}
D(_itoa) {                                                     /* (value, buffer, radix) */
    (void)c;
    char tmp[40], *o = tmp + sizeof tmp - 1;
    int32_t v = (int32_t)ARG(0);
    uint32_t r = ARG(2) ? ARG(2) : 10, u = (r == 10 && v < 0) ? (uint32_t)-(int64_t)v : ARG(0);
    *o = 0;
    do { uint32_t d = u % r; *--o = (char)(d < 10 ? '0' + d : 'a' + d - 10); u /= r; } while (u);
    if (r == 10 && v < 0) *--o = '-';
    strcpy((char *)HOST(ARG(1)), o);
    return ARG(1);
}
D(atof) { FPUSH(atof(STR(ARG(0)))); return 0; }
D(strtod) {
    const char *s = STR(ARG(0));
    char *end;
    double v = strtod(s, &end);
    if (ARG(1)) W32(ARG(1), ARG(0) + (uint32_t)(end - s));
    FPUSH(v);
    return 0;
}
D(div) {                                                       /* div_t in edx:eax */
    int32_t n = (int32_t)ARG(0), d = (int32_t)ARG(1);
    if (!d) return 0;
    c->edx = (uint32_t)(n % d);
    return (uint32_t)(n / d);
}
D(floor) { FPUSH(floor(RF64(a))); return 0; }
D(ceil) { FPUSH(ceil(RF64(a))); return 0; }
D(_isnan) { (void)c; double v = RF64(a); return v != v; }

static uint32_t rand_state = 1;
D(rand) {
    (void)c; (void)a;
    uint32_t s = __atomic_load_n(&rand_state, __ATOMIC_RELAXED) * 214013u + 2531011u;
    __atomic_store_n(&rand_state, s, __ATOMIC_RELAXED);
    return (s >> 16) & 0x7FFFu;
}

/* ---- sorting and searching: the comparison function is the game's (cdecl) ---- */
static int cmp(CPU *c, uint32_t fn, uint32_t x, uint32_t y) { return (int32_t)guest_call(c, fn, CONV_CDECL, 2, x, y); }

static void swap_mem(uint32_t x, uint32_t y, uint32_t w) {
    uint8_t t[256];
    while (w) {
        uint32_t k = w < sizeof t ? w : (uint32_t)sizeof t;
        memcpy(t, HOST(x), k); memcpy(HOST(x), HOST(y), k); memcpy(HOST(y), t, k);
        x += k; y += k; w -= k;
    }
}

/* heap sort: in place, no extra guest memory, O(n log n) comparisons */
static void sift(CPU *c, uint32_t base, uint32_t w, uint32_t fn, uint32_t i, uint32_t n) {
    for (;;) {
        uint32_t l = 2 * i + 1, m = i;
        if (l < n && cmp(c, fn, base + l * w, base + m * w) > 0) m = l;
        if (l + 1 < n && cmp(c, fn, base + (l + 1) * w, base + m * w) > 0) m = l + 1;
        if (m == i) return;
        swap_mem(base + i * w, base + m * w, w);
        i = m;
    }
}
D(qsort) {                                                     /* (base, num, width, compare) */
    uint32_t base = ARG(0), n = ARG(1), w = ARG(2), fn = ARG(3);
    if (n < 2 || !w) return 0;
    for (uint32_t i = n / 2; i-- > 0;) sift(c, base, w, fn, i, n);
    for (uint32_t e = n - 1; e > 0; e--) {
        swap_mem(base, base + e * w, w);
        sift(c, base, w, fn, 0, e);
    }
    return 0;
}
D(bsearch) {                                                   /* (key, base, num, width, compare) */
    uint32_t lo = 0, hi = ARG(2);
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2, p = ARG(1) + mid * ARG(3);
        int r = cmp(c, ARG(4), ARG(0), p);
        if (!r) return p;
        if (r < 0) hi = mid; else lo = mid + 1;
    }
    return 0;
}
