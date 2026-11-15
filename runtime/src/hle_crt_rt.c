/* msvcr90.dll: program start and exit, CRT locks, threads, floating-point control, the x87 math helpers (_CIpow...),
 * std::exception, the stops for errors, and the table that binds every msvcr90 import to its implementation.
 *
 * What it does: the start-up calls mainCRTStartup makes (__getmainargs, _initterm, _onexit...) the way the DLL
 * answers them; exit runs the registered atexit functions. _CIxxx take their arguments on the x87 stack (st0, and
 * st1 for two-argument ones) and leave the result in st0, like the DLL's assembly. _controlfp_s maps the CRT's
 * abstract control word onto the x87 control word and MXCSR (precision and rounding are honoured by the lifted code).
 * std::exception gets a guest vtable (deleting destructor, what) so the game's derived classes work.
 * What it does not do: C++ exception dispatch or longjmp across lifted code. A throw stops the process with the
 * thrown type's name; setjmp returns 0 and a longjmp is reported.
 */
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "game.h"
#include "hle_crt.h"
#include "usf/x87.h"

#define D(name) uint32_t x_##name(CPU *c, uint32_t a)

/* ---- start-up ---- */
static uint32_t data_block;                                    /* the CRT's exported variables, in guest memory */
enum { V_FMODE = 0, V_COMMODE = 4, V_ADJUST_FDIV = 8, V_SYS_NERR = 12, V_ACMDLN = 16, V_NPOS = 20, V_RAISE = 24,
       V_ARGV = 32, V_ENV = 48, V_CMDLINE = 64 };
static uint32_t crt_data(void) {
    if (!data_block) {
        data_block = gcalloc(512);
        W32(data_block + V_SYS_NERR, 43);
        W32(data_block + V_NPOS, 0xFFFFFFFFu);
        snprintf((char *)HOST(data_block + V_CMDLINE), 256, "\"%s\\%s\"", GAME_WIN_DIR, GAME_EXE);
        W32(data_block + V_ACMDLN, data_block + V_CMDLINE);
        snprintf((char *)HOST(data_block + 320), 180, "%s\\%s", GAME_WIN_DIR, GAME_EXE);
        W32(data_block + V_ARGV, data_block + 320);            /* argv = { exe, NULL }, envp = { NULL } */
    }
    return data_block;
}
static uint32_t d_acmdln(CPU *c, uint32_t a) { (void)c; (void)a; return crt_data() + V_ACMDLN; }
static uint32_t d_sys_nerr(CPU *c, uint32_t a) { (void)c; (void)a; return crt_data() + V_SYS_NERR; }
static uint32_t d_adjust_fdiv(CPU *c, uint32_t a) { (void)c; (void)a; return crt_data() + V_ADJUST_FDIV; }
static uint32_t d_npos(CPU *c, uint32_t a) { (void)c; (void)a; return crt_data() + V_NPOS; }
static uint32_t d_raise(CPU *c, uint32_t a) { (void)c; (void)a; return crt_data() + V_RAISE; }

D(__p__fmode) { (void)c; (void)a; return crt_data() + V_FMODE; }
D(__p__commode) { (void)c; (void)a; return crt_data() + V_COMMODE; }
D(__set_app_type) { (void)c; (void)a; return 0; }
D(__setusermatherr) { (void)c; (void)a; return 0; }
D(_configthreadlocale) { (void)c; (void)a; return 2; }
D(_encode_pointer) { (void)c; return ARG(0); }
D(_decode_pointer) { (void)c; return ARG(0); }
D(_crt_debugger_hook) { (void)c; (void)a; return 0; }
D(_set_purecall_handler) { (void)c; (void)a; return 0; }
D(_set_invalid_parameter_handler) { (void)c; (void)a; return 0; }
D(signal) { (void)c; (void)a; return 0; }
D(__getmainargs) {                                             /* (&argc, &argv, &envp, wildcard, &startinfo) */
    (void)c;
    uint32_t d = crt_data();
    W32(ARG(0), 1);
    W32(ARG(1), d + V_ARGV);
    W32(ARG(2), d + V_ENV);
    return 0;
}
D(_initterm) {                                                 /* calls every function pointer in [begin, end) */
    for (uint32_t p = ARG(0); p < ARG(1); p += 4)
        if (R32(p)) guest_call(c, R32(p), CONV_CDECL, 0);
    return 0;
}
D(_initterm_e) {
    for (uint32_t p = ARG(0); p < ARG(1); p += 4)
        if (R32(p)) { uint32_t r = guest_call(c, R32(p), CONV_CDECL, 0); if (r) return r; }
    return 0;
}

/* ---- exit ---- */
static uint32_t atexit_fn[256];
static int natexit;
static pthread_mutex_t exit_lock = PTHREAD_MUTEX_INITIALIZER;
D(_onexit) {
    (void)c;
    pthread_mutex_lock(&exit_lock);
    if (natexit < 256) atexit_fn[natexit++] = ARG(0);
    pthread_mutex_unlock(&exit_lock);
    return ARG(0);
}
D(__dllonexit) { return x__onexit(c, a); }
static void run_atexit(CPU *c) {
    for (;;) {
        pthread_mutex_lock(&exit_lock);
        uint32_t fn = natexit ? atexit_fn[--natexit] : 0;
        pthread_mutex_unlock(&exit_lock);
        if (!fn) return;
        guest_call(c, fn, CONV_CDECL, 0);
    }
}
D(_cexit) { (void)a; run_atexit(c); return 0; }
D(exit) {
    fprintf(stderr, "usf: exit(%d)\n", (int)ARG(0));
    if (!getenv("USF_FAST_EXIT")) run_atexit(c);
    fflush(NULL);
    _exit((int)ARG(0));
}
D(_exit) { (void)c; fprintf(stderr, "usf: _exit(%d)\n", (int)ARG(0)); fflush(NULL); _exit((int)ARG(0)); }

/* ---- errors that stop the program ---- */
static void stop(CPU *c, const char *what) {
    fprintf(stderr, "usf: STOP: %s (called from %08x)\n", what, R32(c->esp));
    fflush(NULL);
    _exit(72);
}
D(abort) { (void)a; stop(c, "abort()"); return 0; }
D(_purecall) { (void)a; stop(c, "pure virtual function call"); return 0; }
D(_amsg_exit) { (void)a; stop(c, "_amsg_exit (CRT runtime error)"); return 0; }
D(_invoke_watson) { (void)a; stop(c, "_invoke_watson (invalid parameter)"); return 0; }
D(_invalid_parameter_noinfo) { (void)a; fprintf(stderr, "usf: CRT invalid parameter (from %08x)\n", R32(c->esp)); return 0; }
static uint32_t terminate_(CPU *c, uint32_t a) { (void)a; stop(c, "std::terminate"); return 0; }

/* _CxxThrowException(object, throw info) (stdcall): reports the type of what was thrown */
static uint32_t cxx_throw(CPU *c, uint32_t a) {
    char msg[512] = "C++ exception";
    uint32_t ti = ARG(1);
    if (ti && R32(ti + 12)) {
        uint32_t arr = R32(ti + 12), ct = R32(arr + 4), td = ct ? R32(ct + 4) : 0;
        if (td) snprintf(msg, sizeof msg, "C++ exception of type %s (not handled yet)", STR(td + 8));
    }
    stop(c, msg);
    return 0;
}
D(__CxxFrameHandler3) { (void)c; (void)a; return 1; }          /* ExceptionContinueSearch: never dispatched here */
D(_except_handler4_common) { (void)c; (void)a; return 1; }
D(_XcptFilter) { (void)c; (void)a; return 0; }
D(_setjmp3) { (void)c; (void)a; return 0; }
D(longjmp) { (void)a; stop(c, "longjmp (not supported yet)"); return 0; }
static uint32_t longjmp_unwind(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }

/* ---- CRT locks ---- */
static pthread_mutex_t locks[64];
static pthread_once_t locks_once = PTHREAD_ONCE_INIT;
static void locks_init(void) {
    pthread_mutexattr_t at;
    pthread_mutexattr_init(&at);
    pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
    for (int i = 0; i < 64; i++) pthread_mutex_init(&locks[i], &at);
}
D(_lock) { (void)c; pthread_once(&locks_once, locks_init); pthread_mutex_lock(&locks[ARG(0) & 63]); return 0; }
D(_unlock) { (void)c; pthread_once(&locks_once, locks_init); pthread_mutex_unlock(&locks[ARG(0) & 63]); return 0; }

/* ---- threads: _beginthreadex has CreateThread's arguments in the same order ---- */
uint32_t k32_CreateThread(CPU *c, uint32_t a);
uint32_t k32_ExitThread(CPU *c, uint32_t a);
D(_beginthreadex) { return k32_CreateThread(c, a); }
D(_endthreadex) { return k32_ExitThread(c, a); }

/* ---- floating-point control ---- */
static uint32_t abstract_cw(const CPU *c) {
    uint32_t cw = c->fpu_cw, r = 0;
    if (cw & 0x01) r |= 0x10;                                  /* invalid */
    if (cw & 0x02) r |= 0x80000;                               /* denormal */
    if (cw & 0x04) r |= 0x08;                                  /* zero divide */
    if (cw & 0x08) r |= 0x04;                                  /* overflow */
    if (cw & 0x10) r |= 0x02;                                  /* underflow */
    if (cw & 0x20) r |= 0x01;                                  /* inexact */
    r |= ((cw >> 10) & 3u) << 8;                               /* rounding */
    uint32_t pc = (cw >> 8) & 3u;
    r |= pc == 0 ? 0x20000u : pc == 2 ? 0x10000u : 0u;         /* precision */
    return r;
}
D(_controlfp_s) {                                              /* (&current, new, mask) */
    uint32_t mask = ARG(2) & 0x000B031Fu, nv = (abstract_cw(c) & ~mask) | (ARG(1) & mask);
    uint32_t cw = 0x40;
    if (nv & 0x10) cw |= 0x01;
    if (nv & 0x80000) cw |= 0x02;
    if (nv & 0x08) cw |= 0x04;
    if (nv & 0x04) cw |= 0x08;
    if (nv & 0x02) cw |= 0x10;
    if (nv & 0x01) cw |= 0x20;
    cw |= ((nv >> 8) & 3u) << 10;
    uint32_t pc = (nv >> 16) & 3u;
    cw |= (pc == 2 ? 0u : pc == 1 ? 2u : 3u) << 8;
    c->fpu_cw = (uint16_t)cw;
    c->mxcsr = (c->mxcsr & ~0x6000u) | (((nv >> 8) & 3u) == 1 ? 0x2000u : ((nv >> 8) & 3u) == 2 ? 0x4000u :
                                         ((nv >> 8) & 3u) == 3 ? 0x6000u : 0u);
    if (ARG(0)) W32(ARG(0), nv);
    usf_log("_controlfp_s(%x, %x) -> x87 cw %04x\n", ARG(1), ARG(2), cw);
    return 0;
}
D(_clearfp) { (void)a; uint32_t r = c->fpu_sw & 0x3F; c->fpu_sw &= ~0x3Fu; return r; }

/* ---- x87 math helpers: argument(s) on the FPU stack, result in st0 ---- */
#define CI1(name, expr) D(_CI##name) { (void)a; double x = ST(0); ST(0) = X87R(expr); return 0; }
#define CI2(name, expr) D(_CI##name) { (void)a; double y = ST(0), x = ST(1); FPOP(); ST(0) = X87R(expr); return 0; }
CI1(acos, acos(x))
CI1(cos, cos(x))
CI1(sin, sin(x))
CI1(tan, tan(x))
CI1(sqrt, sqrt(x))
CI1(log, log(x))
CI1(log10, log10(x))
CI1(exp, exp(x))
CI1(atan, atan(x))
CI2(pow, pow(x, y))
CI2(fmod, fmod(x, y))
CI2(atan2, atan2(x, y))

/* ---- std::exception (12 bytes: vtable, what string, "free it" flag) ---- */
static uint32_t exc_vtable;
static uint32_t exc_what(CPU *c, uint32_t a) {
    (void)a;
    static uint32_t unknown;
    if (!unknown) { unknown = gmalloc(32); strcpy((char *)HOST(unknown), "Unknown exception"); }
    return R32(c->ecx + 4) ? R32(c->ecx + 4) : unknown;
}
static void exc_free(uint32_t self) { if (R32(self + 8) && R32(self + 4)) gfree(R32(self + 4)); W32(self + 4, 0); W32(self + 8, 0); }
static uint32_t exc_deleting_dtor(CPU *c, uint32_t a) {
    uint32_t self = c->ecx;
    exc_free(self);
    if (ARG(0) & 1) gfree(self);
    return self;
}
static uint32_t vtable(void) {
    if (!exc_vtable) {
        exc_vtable = gcalloc(8);
        W32(exc_vtable, usf_thunk_fn("std::exception::`vector deleting destructor'", exc_deleting_dtor, 1, CONV_STDCALL));
        W32(exc_vtable + 4, usf_thunk_fn("std::exception::what", exc_what, 0, CONV_STDCALL));
    }
    return exc_vtable;
}
static void exc_set(uint32_t self, uint32_t str) {
    W32(self, vtable());
    W32(self + 4, 0);
    W32(self + 8, 0);
    if (str) {
        uint32_t n = (uint32_t)strlen(STR(str)) + 1, p = gmalloc(n);
        memcpy(HOST(p), STR(str), n);
        W32(self + 4, p);
        W32(self + 8, 1);
    }
}
static uint32_t exc_ctor(CPU *c, uint32_t a) { (void)a; exc_set(c->ecx, 0); return c->ecx; }
static uint32_t exc_ctor_str(CPU *c, uint32_t a) { exc_set(c->ecx, R32(ARG(0))); return c->ecx; }   /* const char *const & */
static uint32_t exc_ctor_copy(CPU *c, uint32_t a) { exc_set(c->ecx, R32(ARG(0) + 4)); return c->ecx; }
static uint32_t exc_dtor(CPU *c, uint32_t a) { (void)a; W32(c->ecx, vtable()); exc_free(c->ecx); return 0; }
static uint32_t type_info_dtor(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }

/* ---- the table: every msvcr90.dll import ---- */
#define CRT_CDECL(X) \
    X(isupper) X(_fileno) X(_setmode) X(ferror) X(strcmp) X(vfprintf) X(_stat64i32) X(isalpha) X(isalnum) X(_stat64) \
    X(fputc) X(memchr) X(_strtoi64) X(isdigit) X(isxdigit) X(strtoul) X(strerror) X(strncpy) X(wcscpy_s) X(strcat_s) \
    X(_beginthreadex) X(_endthreadex) X(getenv) X(_clearfp) X(_getch) X(fgets) X(_mbsicmp) X(_mbscmp) X(_mbsstr) \
    X(_mbsnbcpy) X(_mbschr) X(feof) X(_CIacos) X(_CIcos) X(_CIsin) X(_CIsqrt) X(__CxxFrameHandler3) X(swprintf_s) \
    X(fflush) X(fprintf) X(__iob_func) X(memset) X(_purecall) X(exit) X(_CIfmod) X(_set_purecall_handler) \
    X(_set_invalid_parameter_handler) X(_invalid_parameter_noinfo) X(_snwprintf_s) X(memmove_s) X(memcpy) \
    X(_CIlog10) X(wcstombs_s) X(_localtime64) X(_time64) X(_aligned_malloc) X(_aligned_free) X(strftime) \
    X(wcsncpy_s) X(strrchr) X(wcsrchr) X(tolower) X(fclose) X(fopen) X(free) X(malloc) X(_findclose) \
    X(_findnext64i32) X(strncat) X(_findfirst64i32) X(_strdup) X(_itoa) X(fread) X(fwrite) X(fseek) X(ftell) \
    X(realloc) X(_setjmp3) X(sscanf) X(strncmp) X(floor) X(calloc) X(_snprintf) X(_gmtime64) X(abort) X(_CIpow) \
    X(memmove) X(div) X(_CItan) X(longjmp) X(strtod) X(bsearch) X(qsort) X(_CIexp) X(rand) X(_CIlog) X(_CIatan2) \
    X(wcsstr) X(swscanf) X(ceil) X(sprintf_s) X(_isnan) X(_CIatan) X(sscanf_s) X(atof) X(atol) X(atoi) X(strchr) \
    X(_vsnprintf_s) X(strtol) X(vswprintf_s) X(_vsnprintf) X(_msize) X(towlower) X(isspace) X(_waccess_s) \
    X(_wcserror) X(_errno) X(_wremove) X(_wstat64i32) X(_strnicmp) X(strstr) X(_except_handler4_common) \
    X(_amsg_exit) X(__getmainargs) X(_cexit) X(_exit) X(_XcptFilter) X(_ismbblead) X(_initterm) X(_initterm_e) \
    X(_configthreadlocale) X(__setusermatherr) X(__p__commode) X(__p__fmode) X(_encode_pointer) X(__set_app_type) \
    X(_unlock) X(__dllonexit) X(_lock) X(_onexit) X(_decode_pointer) X(_stricmp) X(_crt_debugger_hook) \
    X(_invoke_watson) X(_controlfp_s) X(fputs) X(signal) X(sprintf)

#define DECL(n) uint32_t x_##n(CPU *c, uint32_t a);
CRT_CDECL(DECL)
#define ENTRY(n) {"msvcr90.dll", #n, 0, CONV_CDECL, x_##n},

const HleImport hle_crt_table[] = {
    CRT_CDECL(ENTRY)
    {"msvcr90.dll", "_adjust_fdiv", 0, CONV_DATA, d_adjust_fdiv},
    {"msvcr90.dll", "_acmdln", 0, CONV_DATA, d_acmdln},
    {"msvcr90.dll", "__sys_nerr", 0, CONV_DATA, d_sys_nerr},
    {"msvcr90.dll", "_CxxThrowException", 2, CONV_STDCALL, cxx_throw},
    {"msvcr90.dll", "__CxxLongjmpUnwind", 1, CONV_STDCALL, longjmp_unwind},
    {"msvcr90.dll", "?terminate@@YAXXZ", 0, CONV_CDECL, terminate_},
    {"msvcr90.dll", "??0exception@std@@QAE@XZ", 0, CONV_STDCALL, exc_ctor},
    {"msvcr90.dll", "??0exception@std@@QAE@ABQBD@Z", 1, CONV_STDCALL, exc_ctor_str},
    {"msvcr90.dll", "??0exception@std@@QAE@ABV01@@Z", 1, CONV_STDCALL, exc_ctor_copy},
    {"msvcr90.dll", "??1exception@std@@UAE@XZ", 0, CONV_STDCALL, exc_dtor},
    {"msvcr90.dll", "?what@exception@std@@UBEPBDXZ", 0, CONV_STDCALL, exc_what},
    {"msvcr90.dll", "?_type_info_dtor_internal_method@type_info@@QAEXXZ", 0, CONV_STDCALL, type_info_dtor},
    /* msvcp90.dll variables */
    {"msvcp90.dll", "?npos@?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@2IB", 0, CONV_DATA, d_npos},
    {"msvcp90.dll", "?_Raise_handler@std@@3P6AXABVexception@stdext@@@ZA", 0, CONV_DATA, d_raise},
    {NULL, NULL, 0, 0, NULL},
};
