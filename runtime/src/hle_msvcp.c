/* msvcp90.dll: the std::string and std::wstring members the game imports (Visual C++ 2008 layout).
 *
 * What it does: basic_string<char> / <wchar_t> as the 2008 library lays them out (28 bytes: allocator dword, a
 * 16-byte buffer that holds short strings in place or a heap pointer, size at +20, capacity at +24; the buffer holds
 * 15 chars or 7 wide chars). Methods are thiscall (`this` in ecx, the callee pops its arguments); functions returning
 * a string by value take the address of the result as a hidden first argument. Memory is the game's heap (gmalloc),
 * as the library's allocator is operator new -> malloc.
 * What it does not do: the rest of the C++ library (streams, locale): the game does not import it. std::_Throw stops.
 */
#include <stdlib.h>
#include <string.h>

#include "hle_crt.h"

#define SIZE 20u
#define RES 24u

/* w: bytes per character (1 or 2) */
static uint32_t cap_inplace(int w) { return w == 1 ? 15u : 7u; }
static uint32_t sdata(uint32_t s) { return R32(s + RES) > cap_inplace(1) ? R32(s + 4) : s + 4; }
static uint32_t wdata(uint32_t s) { return R32(s + RES) > cap_inplace(2) ? R32(s + 4) : s + 4; }
static uint32_t data(uint32_t s, int w) { return w == 1 ? sdata(s) : wdata(s); }
static uint32_t length(uint32_t p, int w) {
    uint32_t n = 0;
    if (w == 1) return (uint32_t)strlen(STR(p));
    while (R16(p + 2 * n)) n++;
    return n;
}

static void init(uint32_t s, int w) {
    W32(s, 0);
    W32(s + SIZE, 0);
    W32(s + RES, cap_inplace(w));
    if (w == 1) W8(s + 4, 0); else W16(s + 4, 0);
}
static void tidy(uint32_t s, int w) {
    if (R32(s + RES) > cap_inplace(w)) gfree(R32(s + 4));
    init(s, w);
}
/* room for n characters (+ terminator), keeping the current contents */
static void reserve(uint32_t s, uint32_t n, int w) {
    uint32_t cap = R32(s + RES);
    if (n <= cap) return;
    uint32_t ncap = n | (w == 1 ? 15u : 7u);
    if (ncap < cap + cap / 2) ncap = cap + cap / 2;
    uint32_t p = gmalloc((ncap + 1) * (uint32_t)w), old = data(s, w), len = R32(s + SIZE);
    memcpy(HOST(p), HOST(old), (len + 1) * (uint32_t)w);
    if (cap > cap_inplace(w)) gfree(old);
    W32(s + 4, p);
    W32(s + RES, ncap);
}
static void set_len(uint32_t s, uint32_t n, int w) {
    W32(s + SIZE, n);
    if (w == 1) W8(data(s, w) + n, 0); else W16(data(s, w) + 2 * n, 0);
}
/* replace the contents with n characters from p (p may point into the string itself) */
static void assign(uint32_t s, uint32_t p, uint32_t n, int w) {
    if (n > R32(s + RES)) {
        uint32_t tmp = gmalloc(n * (uint32_t)w + 2);
        memcpy(HOST(tmp), HOST(p), n * (uint32_t)w);
        reserve(s, n, w);
        memcpy(HOST(data(s, w)), HOST(tmp), n * (uint32_t)w);
        gfree(tmp);
    } else {
        memmove(HOST(data(s, w)), HOST(p), n * (uint32_t)w);
    }
    set_len(s, n, w);
}
static void append(uint32_t s, uint32_t p, uint32_t n, int w) {
    uint32_t len = R32(s + SIZE);
    if (len + n > R32(s + RES)) {
        uint32_t tmp = gmalloc(n * (uint32_t)w + 2);
        memcpy(HOST(tmp), HOST(p), n * (uint32_t)w);
        reserve(s, len + n, w);
        memcpy(HOST(data(s, w) + len * (uint32_t)w), HOST(tmp), n * (uint32_t)w);
        gfree(tmp);
    } else {
        memmove(HOST(data(s, w) + len * (uint32_t)w), HOST(p), n * (uint32_t)w);
    }
    set_len(s, len + n, w);
}

#define THIS (c->ecx)
/* ---- std::string ---- */
static uint32_t s_ctor(CPU *c, uint32_t a) { (void)a; init(THIS, 1); return THIS; }
static uint32_t s_ctor_cstr(CPU *c, uint32_t a) { init(THIS, 1); assign(THIS, ARG(0), length(ARG(0), 1), 1); return THIS; }
static uint32_t s_ctor_copy(CPU *c, uint32_t a) { init(THIS, 1); assign(THIS, sdata(ARG(0)), R32(ARG(0) + SIZE), 1); return THIS; }
static uint32_t s_dtor(CPU *c, uint32_t a) { (void)a; tidy(THIS, 1); return 0; }
static uint32_t s_assign_str(CPU *c, uint32_t a) {
    if (ARG(0) != THIS) assign(THIS, sdata(ARG(0)), R32(ARG(0) + SIZE), 1);
    return THIS;
}
static uint32_t s_assign_cstr(CPU *c, uint32_t a) { assign(THIS, ARG(0), length(ARG(0), 1), 1); return THIS; }
static uint32_t s_append_cstr(CPU *c, uint32_t a) { append(THIS, ARG(0), length(ARG(0), 1), 1); return THIS; }
static uint32_t s_append_n(CPU *c, uint32_t a) { append(THIS, ARG(0), ARG(1), 1); return THIS; }
static uint32_t s_append_chars(CPU *c, uint32_t a) {          /* (count, ch) */
    uint32_t len = R32(THIS + SIZE), n = ARG(0);
    reserve(THIS, len + n, 1);
    memset(HOST(sdata(THIS) + len), (int)(ARG(1) & 0xFF), n);
    set_len(THIS, len + n, 1);
    return THIS;
}
static uint32_t s_size(CPU *c, uint32_t a) { (void)a; return R32(THIS + SIZE); }
static uint32_t s_c_str(CPU *c, uint32_t a) { (void)a; return sdata(THIS); }
static uint32_t s_index(CPU *c, uint32_t a) { return sdata(THIS) + ARG(0); }
static uint32_t s_resize(CPU *c, uint32_t a) {
    uint32_t len = R32(THIS + SIZE), n = ARG(0);
    if (n > len) { reserve(THIS, n, 1); memset(HOST(sdata(THIS) + len), 0, n - len); }
    set_len(THIS, n, 1);
    return 0;
}
static uint32_t s_swap(CPU *c, uint32_t a) {
    uint8_t t[28];
    memcpy(t, HOST(THIS), 28);
    memcpy(HOST(THIS), HOST(ARG(0)), 28);
    memcpy(HOST(ARG(0)), t, 28);
    return 0;
}
static uint32_t s_find(CPU *c, uint32_t a) {                  /* (const char *, pos, count) */
    uint32_t len = R32(THIS + SIZE), pos = ARG(1), n = ARG(2);
    const char *h = STR(sdata(THIS)), *nd = STR(ARG(0));
    if (n == 0) return pos <= len ? pos : 0xFFFFFFFFu;
    for (uint32_t i = pos; i + n <= len && i < len; i++)
        if (!memcmp(h + i, nd, n)) return i;
    return 0xFFFFFFFFu;
}
static uint32_t s_substr(CPU *c, uint32_t a) {                /* (hidden result, pos, count) */
    uint32_t r = ARG(0), len = R32(THIS + SIZE), pos = ARG(1) > len ? len : ARG(1), n = ARG(2);
    if (n > len - pos) n = len - pos;
    init(r, 1);
    assign(r, sdata(THIS) + pos, n, 1);
    return r;
}
/* operator+(const string &, char) and operator<(const string &, const string &) are free (cdecl) functions */
static uint32_t s_plus_char(CPU *c, uint32_t a) {             /* (hidden result, left, ch) */
    (void)c;
    uint32_t r = ARG(0);
    init(r, 1);
    assign(r, sdata(ARG(1)), R32(ARG(1) + SIZE), 1);
    uint8_t ch = (uint8_t)ARG(2);
    uint32_t len = R32(r + SIZE);
    reserve(r, len + 1, 1);
    W8(sdata(r) + len, ch);
    set_len(r, len + 1, 1);
    return r;
}
static uint32_t s_less(CPU *c, uint32_t a) {
    (void)c;
    uint32_t la = R32(ARG(0) + SIZE), lb = R32(ARG(1) + SIZE);
    int r = memcmp(HOST(sdata(ARG(0))), HOST(sdata(ARG(1))), la < lb ? la : lb);
    return r < 0 || (r == 0 && la < lb);
}

/* ---- std::wstring ---- */
static uint32_t w_ctor(CPU *c, uint32_t a) { (void)a; init(THIS, 2); return THIS; }
static uint32_t w_ctor_cstr(CPU *c, uint32_t a) { init(THIS, 2); assign(THIS, ARG(0), length(ARG(0), 2), 2); return THIS; }
static uint32_t w_dtor(CPU *c, uint32_t a) { (void)a; tidy(THIS, 2); return 0; }
static uint32_t w_assign_cstr(CPU *c, uint32_t a) { assign(THIS, ARG(0), length(ARG(0), 2), 2); return THIS; }
static uint32_t w_assign_str(CPU *c, uint32_t a) {
    if (ARG(0) != THIS) assign(THIS, wdata(ARG(0)), R32(ARG(0) + SIZE), 2);
    return THIS;
}
static uint32_t w_c_str(CPU *c, uint32_t a) { (void)a; return wdata(THIS); }

static uint32_t std_throw(CPU *c, uint32_t a) {               /* std::_Throw(const stdext::exception &) */
    uint32_t e = ARG(0), w = R32(e + 4);
    fprintf(stderr, "usf: STOP: std::_Throw: %s (called from %08x)\n", w ? STR(w) : "?", R32(c->esp));
    fflush(NULL);
    _Exit(73);
}

#define M(name, n, fn) {"msvcp90.dll", name, n, CONV_STDCALL, fn},
const HleImport hle_msvcp_table[] = {
    M("??0?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@XZ", 0, s_ctor)
    M("??0?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@PBD@Z", 1, s_ctor_cstr)
    M("??0?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@ABV01@@Z", 1, s_ctor_copy)
    M("??1?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@XZ", 0, s_dtor)
    M("??4?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAEAAV01@ABV01@@Z", 1, s_assign_str)
    M("??4?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAEAAV01@PBD@Z", 1, s_assign_cstr)
    M("??Y?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAEAAV01@PBD@Z", 1, s_append_cstr)
    M("?append@?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAEAAV12@PBD@Z", 1, s_append_cstr)
    M("?append@?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAEAAV12@PBDI@Z", 2, s_append_n)
    M("?append@?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAEAAV12@ID@Z", 2, s_append_chars)
    M("?size@?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QBEIXZ", 0, s_size)
    M("?c_str@?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QBEPBDXZ", 0, s_c_str)
    M("??A?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QBEABDU_Size_type_nosscl@01@@Z", 1, s_index)
    M("?find@?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QBEIPBDII@Z", 3, s_find)
    M("?substr@?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QBE?AV12@II@Z", 3, s_substr)
    M("?swap@?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAEXAAV12@@Z", 1, s_swap)
    M("?resize@?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAEXI@Z", 1, s_resize)
    {"msvcp90.dll", "??$?HDU?$char_traits@D@std@@V?$allocator@D@1@@std@@YA?AV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@0@ABV10@D@Z", 0, CONV_CDECL, s_plus_char},
    {"msvcp90.dll", "??$?MDU?$char_traits@D@std@@V?$allocator@D@1@@std@@YA_NABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@0@0@Z", 0, CONV_CDECL, s_less},
    M("??0?$basic_string@_WU?$char_traits@_W@std@@V?$allocator@_W@2@@std@@QAE@XZ", 0, w_ctor)
    M("??0?$basic_string@_WU?$char_traits@_W@std@@V?$allocator@_W@2@@std@@QAE@PB_W@Z", 1, w_ctor_cstr)
    M("??1?$basic_string@_WU?$char_traits@_W@std@@V?$allocator@_W@2@@std@@QAE@XZ", 0, w_dtor)
    M("??4?$basic_string@_WU?$char_traits@_W@std@@V?$allocator@_W@2@@std@@QAEAAV01@PB_W@Z", 1, w_assign_cstr)
    M("??4?$basic_string@_WU?$char_traits@_W@std@@V?$allocator@_W@2@@std@@QAEAAV01@ABV01@@Z", 1, w_assign_str)
    M("?c_str@?$basic_string@_WU?$char_traits@_W@std@@V?$allocator@_W@2@@std@@QBEPB_WXZ", 0, w_c_str)
    {"msvcp90.dll", "?_Throw@std@@YAXABVexception@stdext@@@Z", 0, CONV_CDECL, std_throw},
    {NULL, NULL, 0, 0, NULL},
};
