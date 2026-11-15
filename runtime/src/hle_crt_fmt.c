/* msvcr90.dll: printf and scanf on guest arguments, and the functions built on them (sprintf, _vsnprintf,
 * swprintf_s, sscanf, swscanf...).
 *
 * What it does: crt_format walks a format string and formats each conversion with the host snprintf, reading its
 * argument from the guest stack the way the 32-bit Windows CRT lays them out (int and pointers 4 bytes, double and
 * __int64 8 bytes; `l` is 32 bits, `I64` / `ll` 64). %s in a wide function is a UTF-16 string, %S the other width.
 * crt_scan rewrites the format for the host (Windows `l` on an integer is 32 bits; `I64` -> `ll`) and passes the
 * game's own pointers (guest memory is host memory, offset by usf_mem), so the host sscanf writes the results
 * straight into the game's variables.
 * What it does not do: %n in scanf, wide %s / %c in swscanf (converted as UTF-8, reported), locale-specific output.
 */
#include <stdlib.h>
#include <string.h>

#include "hle_crt.h"

#define D(name) uint32_t x_##name(CPU *c, uint32_t a)

static void put(char *out, size_t cap, size_t *o, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++, (*o)++)
        if (*o + 1 < cap) out[*o] = s[i];
}

size_t crt_format(char *out, size_t cap, const char *fmt, uint32_t va, int wide) {
    size_t o = 0;
    const char *p = fmt;
    while (*p) {
        if (*p != '%') { put(out, cap, &o, p++, 1); continue; }
        if (p[1] == '%') { put(out, cap, &o, "%", 1); p += 2; continue; }
        char spec[48], *sp = spec;
        *sp++ = *p++;
        while (*p && strchr("-+ #0", *p) && sp < spec + 8) *sp++ = *p++;
        if (*p == '*') { sp += sprintf(sp, "%d", (int32_t)R32(va)); va += 4; p++; }
        else while (*p >= '0' && *p <= '9' && sp < spec + 20) *sp++ = *p++;
        if (*p == '.') {
            *sp++ = *p++;
            if (*p == '*') { sp += sprintf(sp, "%d", (int32_t)R32(va)); va += 4; p++; }
            else while (*p >= '0' && *p <= '9' && sp < spec + 36) *sp++ = *p++;
        }
        int big = 0, h = 0, l = 0;                              /* argument size modifiers */
        for (;;) {
            if (p[0] == 'I' && p[1] == '6' && p[2] == '4') { big = 1; p += 3; }
            else if (p[0] == 'I' && p[1] == '3' && p[2] == '2') p += 3;
            else if (p[0] == 'l' && p[1] == 'l') { big = 1; p += 2; }
            else if (*p == 'I' || *p == 'L' || *p == 'z' || *p == 't' || *p == 'j') p++;
            else if (*p == 'h') { h = 1; p++; }
            else if (*p == 'l' || *p == 'w') { l = 1; p++; }
            else break;
        }
        char conv = *p ? *p++ : 0;
        char tmp[512];
        int n = 0;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
            uint64_t v = big ? R64(va) : R32(va);
            va += big ? 8 : 4;
            if (!big && (conv == 'd' || conv == 'i')) v = (uint64_t)(int64_t)(int32_t)v;
            if (h) v = (conv == 'd' || conv == 'i') ? (uint64_t)(int64_t)(int16_t)v : (uint16_t)v;
            *sp++ = 'l'; *sp++ = 'l'; *sp++ = conv; *sp = 0;
            n = snprintf(tmp, sizeof tmp, spec, v);
            break;
        }
        case 'p': n = snprintf(tmp, sizeof tmp, "%08X", R32(va)); va += 4; break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            *sp++ = conv; *sp = 0;
            n = snprintf(tmp, sizeof tmp, spec, RF64(va));
            va += 8;
            break;
        case 'c': case 'C': {
            uint32_t ch = R32(va) & 0xFFFF;
            va += 4;
            char u[8] = {0};
            if (ch < 0x80) u[0] = (char)ch;
            else if (ch < 0x800) { u[0] = (char)(0xC0 | ch >> 6); u[1] = (char)(0x80 | (ch & 63)); }
            else { u[0] = (char)(0xE0 | ch >> 12); u[1] = (char)(0x80 | ((ch >> 6) & 63)); u[2] = (char)(0x80 | (ch & 63)); }
            *sp++ = 's'; *sp = 0;
            n = snprintf(tmp, sizeof tmp, spec, u);
            break;
        }
        case 's': case 'S': {
            uint32_t s = R32(va);
            va += 4;
            int is_wide = (conv == 's') ? (wide ? !h : l) : !wide ? 1 : 0;
            if (conv == 'S' && wide) is_wide = 0;
            static __thread char str[8192];
            if (!s) snprintf(str, sizeof str, "(null)");
            else if (is_wide) w2utf8(s, str, sizeof str);
            else snprintf(str, sizeof str, "%s", STR(s));
            *sp++ = 's'; *sp = 0;
            char *big_out = NULL;
            size_t need = strlen(str) + 64;
            if (need > sizeof tmp) big_out = malloc(need + 512);
            n = snprintf(big_out ? big_out : tmp, big_out ? need + 512 : sizeof tmp, spec, str);
            if (big_out) { put(out, cap, &o, big_out, (size_t)n); free(big_out); n = 0; continue; }
            break;
        }
        case 'n': W32(R32(va), (uint32_t)o); va += 4; break;
        default: put(out, cap, &o, spec, (size_t)(sp - spec)); if (conv) put(out, cap, &o, &conv, 1); continue;
        }
        if (n > (int)sizeof tmp - 1) n = (int)sizeof tmp - 1;
        if (n > 0) put(out, cap, &o, tmp, (size_t)n);
    }
    if (cap) out[o < cap ? o : cap - 1] = 0;
    return o;
}

/* ---- narrow printf family ---- */
static uint32_t to_buf(uint32_t dst, uint32_t cap, const char *fmt, uint32_t va, int trunc_neg) {
    char *tmp = malloc(65536);
    size_t n = crt_format(tmp, 65536, fmt, va, 0);
    uint32_t r;
    if (n < cap) { memcpy(HOST(dst), tmp, n + 1); r = (uint32_t)n; }
    else {                                                     /* does not fit: _snprintf writes cap bytes, no NUL */
        if (cap) memcpy(HOST(dst), tmp, cap);
        if (trunc_neg == 2 && cap) W8(dst + cap - 1, 0);       /* the _s forms always terminate */
        r = 0xFFFFFFFFu;
    }
    free(tmp);
    return r;
}
D(sprintf) { (void)c; return to_buf(ARG(0), 0x7FFFFFFF, STR(ARG(1)), a + 8, 0); }
D(sprintf_s) { (void)c; return to_buf(ARG(0), ARG(1), STR(ARG(2)), a + 12, 2); }
D(_snprintf) { (void)c; return to_buf(ARG(0), ARG(1), STR(ARG(2)), a + 12, 1); }
D(_vsnprintf) { (void)c; return to_buf(ARG(0), ARG(1), STR(ARG(2)), ARG(3), 1); }
D(_vsnprintf_s) {                                              /* (dst, size, count, fmt, va); count _TRUNCATE = ~0 */
    (void)c;
    uint32_t cap = ARG(1);
    if (ARG(2) != 0xFFFFFFFFu && ARG(2) + 1 < cap) cap = ARG(2) + 1;
    return to_buf(ARG(0), cap, STR(ARG(3)), ARG(4), 2);
}

/* ---- wide printf family: format and output are UTF-16 ---- */
static uint32_t to_wbuf(uint32_t dst, uint32_t cap, uint32_t wfmt, uint32_t va) {
    char fmt[4096];
    w2utf8(wfmt, fmt, sizeof fmt);
    char *tmp = malloc(65536);
    crt_format(tmp, 65536, fmt, va, 1);
    uint32_t tw = gmalloc(2 * 65536);
    utf8_to_w(tmp, tw, 65536);
    size_t n = crt_wlen(tw);
    uint32_t r;
    if (n < cap) { memcpy(HOST(dst), HOST(tw), 2 * (n + 1)); r = (uint32_t)n; }
    else { if (cap) { memcpy(HOST(dst), HOST(tw), 2 * (cap - 1)); W16(dst + 2 * (cap - 1), 0); } r = 0xFFFFFFFFu; }
    gfree(tw);
    free(tmp);
    return r;
}
D(swprintf_s) { (void)c; return to_wbuf(ARG(0), ARG(1), ARG(2), a + 12); }
D(vswprintf_s) { (void)c; return to_wbuf(ARG(0), ARG(1), ARG(2), ARG(3)); }
D(_snwprintf_s) {                                              /* (dst, size, count, fmt, ...) */
    (void)c;
    uint32_t cap = ARG(1);
    if (ARG(2) != 0xFFFFFFFFu && ARG(2) + 1 < cap) cap = ARG(2) + 1;
    return to_wbuf(ARG(0), cap, ARG(3), a + 16);
}

/* ---- scanf ---- */
int crt_scan(const char *in, const char *fmt, uint32_t va, int secure, int wide) {
    char hf[1024], *o = hf;
    void *ptr[24];
    int np = 0;
    const char *p = fmt;
    while (*p && o < hf + sizeof hf - 16) {
        if (*p != '%') { *o++ = *p++; continue; }
        *o++ = *p++;
        if (*p == '%') { *o++ = *p++; continue; }
        int skip = 0;
        if (*p == '*') { skip = 1; *o++ = *p++; }
        while (*p >= '0' && *p <= '9') *o++ = *p++;
        int big = 0, h = 0, l = 0;
        for (;;) {
            if (p[0] == 'I' && p[1] == '6' && p[2] == '4') { big = 1; p += 3; }
            else if (p[0] == 'l' && p[1] == 'l') { big = 1; p += 2; }
            else if (*p == 'h') { h = 1; p++; }
            else if (*p == 'l' || *p == 'L') { l = 1; p++; }
            else break;
        }
        char conv = *p;
        if (conv == '[') {                                     /* copy the scan set as it is */
            *o++ = *p++;
            if (*p == '^') *o++ = *p++;
            if (*p == ']') *o++ = *p++;
            while (*p && *p != ']') *o++ = *p++;
            if (*p) *o++ = *p++;
        } else if (conv) {
            if (strchr("dioux", conv)) { if (big) { *o++ = 'l'; *o++ = 'l'; } else if (h) *o++ = 'h'; }
            else if (strchr("efgEG", conv) && l) *o++ = 'l';
            *o++ = conv;
            p++;
        }
        if (conv == 'n' && !skip) { fprintf(stderr, "usf: scanf %%n not handled\n"); return 0; }
        if (wide && (conv == 's' || conv == 'c' || conv == '['))
            fprintf(stderr, "usf: swscanf %%%c into a wide buffer is converted as UTF-8\n", conv);
        if (!skip && conv) {
            if (np >= 24) break;
            ptr[np++] = HOST(R32(va));
            va += 4;
            if (secure && (conv == 's' || conv == 'c' || conv == '[')) va += 4;   /* the buffer size */
        }
    }
    *o = 0;
    for (int i = np; i < 24; i++) ptr[i] = NULL;
    return sscanf(in, hf, ptr[0], ptr[1], ptr[2], ptr[3], ptr[4], ptr[5], ptr[6], ptr[7], ptr[8], ptr[9], ptr[10],
                  ptr[11], ptr[12], ptr[13], ptr[14], ptr[15], ptr[16], ptr[17], ptr[18], ptr[19], ptr[20], ptr[21],
                  ptr[22], ptr[23]);
}
D(sscanf) { (void)c; return (uint32_t)crt_scan(STR(ARG(0)), STR(ARG(1)), a + 8, 0, 0); }
D(sscanf_s) { (void)c; return (uint32_t)crt_scan(STR(ARG(0)), STR(ARG(1)), a + 8, 1, 0); }
D(swscanf) {
    (void)c;
    char in[4096], fmt[1024];
    w2utf8(ARG(0), in, sizeof in);
    w2utf8(ARG(1), fmt, sizeof fmt);
    return (uint32_t)crt_scan(in, fmt, a + 8, 0, 1);
}
