/* kernel32: memory (heaps, VirtualAlloc, LocalAlloc), environment, startup info, system and version information,
 * code pages and locale (answered as US English, code page 1252), time, and the small odds and ends.
 *
 * What it does: what the MSVC 2010 C runtime and the game ask during start-up, answered the way a Windows 7
 * machine with an English locale would. Character conversion between code page 1252 / UTF-8 and UTF-16 is done
 * for the Latin-1 range (enough for file names and the CRT's locale tables).
 * What it does not do: real locale data (GetLocaleInfo returns a few fixed fields), or more than one heap
 * (every HeapCreate handle allocates from the same guest heap).
 */
#include <sched.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "hle.h"

#define PROCESS_HEAP 0x00150000u

/* ---- heaps ---- */
uint32_t k32_GetProcessHeap(CPU *c, uint32_t a) { (void)c; (void)a; return PROCESS_HEAP; }
uint32_t k32_HeapCreate(CPU *c, uint32_t a) { (void)c; (void)a; return PROCESS_HEAP + 0x10000u; }
uint32_t k32_HeapSetInformation(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
uint32_t k32_HeapAlloc(CPU *c, uint32_t a) {
    (void)c;
    return (ARG(1) & 8u) ? gcalloc(ARG(2)) : gmalloc(ARG(2));   /* HEAP_ZERO_MEMORY */
}
uint32_t k32_HeapFree(CPU *c, uint32_t a) { (void)c; gfree(ARG(2)); return 1; }
uint32_t k32_HeapSize(CPU *c, uint32_t a) { (void)c; return gsize(ARG(2)); }
uint32_t k32_HeapReAlloc(CPU *c, uint32_t a) {
    (void)c;
    uint32_t old = gsize(ARG(2)), n = grealloc(ARG(2), ARG(3));
    if (n && (ARG(1) & 8u) && ARG(3) > old) memset(HOST(n + old), 0, ARG(3) - old);
    return n;
}
uint32_t k32_LocalAlloc(CPU *c, uint32_t a) { (void)c; return (ARG(0) & 0x40u) ? gcalloc(ARG(1)) : gmalloc(ARG(1)); }
uint32_t x_CoTaskMemAlloc(CPU *c, uint32_t a) { (void)c; return gmalloc(ARG(0)); }
uint32_t x_CoTaskMemFree(CPU *c, uint32_t a) { (void)c; gfree(ARG(0)); return 0; }

uint32_t k32_VirtualAlloc(CPU *c, uint32_t a) {
    uint32_t addr = ARG(0), size = ARG(1), type = ARG(2);
    if (addr && !(type & 0x2000u)) return addr & ~0xFFFu;     /* MEM_COMMIT inside a reservation: already usable */
    uint32_t r = gpages(addr & ~0xFFFFu, size);
    if (!r) set_last_error(c, 8);                              /* ERROR_NOT_ENOUGH_MEMORY */
    usf_log("VirtualAlloc(%08x, %u, %x) -> %08x\n", addr, size, type, r);
    return r;
}
uint32_t k32_VirtualFree(CPU *c, uint32_t a) { (void)c; if (ARG(2) & 0x8000u) gpages_free(ARG(0)); return 1; }
uint32_t k32_IsBadWritePtr(CPU *c, uint32_t a) { (void)c; return ARG(0) < 0x10000u; }

uint32_t k32_GlobalMemoryStatusEx(CPU *c, uint32_t a) {
    (void)c;
    uint32_t p = ARG(0);
    memset(HOST(p + 4), 0, 60);
    W32(p + 4, 30);                                            /* memory load % */
    W64(p + 8, 8ull << 30);                                    /* total physical */
    W64(p + 16, 6ull << 30);                                   /* available physical */
    W64(p + 24, 16ull << 30);
    W64(p + 32, 12ull << 30);
    W64(p + 40, 2ull << 30);                                   /* total virtual (32-bit process) */
    W64(p + 48, 1536ull << 20);
    return 1;
}

/* ---- environment and startup ---- */
uint32_t k32_GetEnvironmentStringsW(CPU *c, uint32_t a) {
    (void)c; (void)a;
    static const char *env[] = {"SystemRoot=C:\\Windows", "USERPROFILE=C:\\Users\\player", "TEMP=C:\\Temp",
                                "APPDATA=C:\\Users\\player\\AppData\\Roaming", NULL};
    uint32_t p = gmalloc(1024), o = p;
    for (int i = 0; env[i]; i++) { utf8_to_w(env[i], o, 200); o += 2u * ((uint32_t)wlen(o) + 1u); }
    W16(o, 0);
    return p;
}
uint32_t k32_FreeEnvironmentStringsW(CPU *c, uint32_t a) { (void)c; gfree(ARG(0)); return 1; }
uint32_t k32_SetEnvironmentVariableA(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }

uint32_t k32_GetStartupInfoW(CPU *c, uint32_t a) {
    (void)c;
    memset(HOST(ARG(0)), 0, 68);
    W32(ARG(0), 68);
    return 0;
}
uint32_t k32_GetStdHandle(CPU *c, uint32_t a) {
    (void)c;
    return ARG(0) == 0xFFFFFFF5u ? 0x14u : ARG(0) == 0xFFFFFFF4u ? 0x18u : 0x10u;   /* stdout, stderr, stdin */
}
uint32_t k32_SetStdHandle(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
uint32_t k32_SetHandleCount(CPU *c, uint32_t a) { (void)c; return ARG(0); }
uint32_t k32_GetConsoleCP(CPU *c, uint32_t a) { (void)c; (void)a; return 437; }
uint32_t k32_GetConsoleMode(CPU *c, uint32_t a) { set_last_error(c, 6); (void)a; return 0; }
uint32_t k32_SetErrorMode(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t k32_IsDebuggerPresent(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t k32_OutputDebugStringA(CPU *c, uint32_t a) { (void)c; fprintf(stderr, "[debug] %s", STR(ARG(0))); return 0; }
uint32_t k32_SetUnhandledExceptionFilter(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t k32_UnhandledExceptionFilter(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
uint32_t k32_EncodePointer(CPU *c, uint32_t a) { (void)c; return ARG(0); }
uint32_t k32_DecodePointer(CPU *c, uint32_t a) { (void)c; return ARG(0); }
uint32_t k32_SetThreadExecutionState(CPU *c, uint32_t a) { (void)c; (void)a; return 0x80000000u; }
uint32_t k32_MulDiv(CPU *c, uint32_t a) {
    (void)c;
    int32_t d = (int32_t)ARG(2);
    if (!d) return 0xFFFFFFFFu;
    int64_t v = (int64_t)(int32_t)ARG(0) * (int32_t)ARG(1);
    return (uint32_t)(int32_t)((v + (v >= 0 ? d / 2 : -d / 2)) / d);
}

uint32_t k32_IsProcessorFeaturePresent(CPU *c, uint32_t a) {
    (void)c;
    uint32_t f = ARG(0);
    return f == 6 || f == 7 || f == 10 || f == 13;   /* XMMI, 3DNOW? no: XMMI(6) XMMI64(10) SSE3(13), PAE(9) */
}

uint32_t k32_GetSystemInfo(CPU *c, uint32_t a) {
    (void)c;
    uint32_t p = ARG(0);
    memset(HOST(p), 0, 36);
    W32(p + 4, 4096);                                          /* page size */
    W32(p + 8, 0x10000);
    W32(p + 12, 0x7FFEFFFF);
    W32(p + 16, 0xF);                                          /* active processor mask */
    W32(p + 20, 4);                                            /* processors */
    W32(p + 24, 586);
    W32(p + 28, 0x10000);                                      /* allocation granularity */
    W16(p + 32, 6);
    return 0;
}

uint32_t k32_GetVersionExW(CPU *c, uint32_t a) {
    (void)c;
    uint32_t p = ARG(0);
    W32(p + 4, 6);                                             /* Windows 7: 6.1.7601 SP1 */
    W32(p + 8, 1);
    W32(p + 12, 7601);
    W32(p + 16, 2);                                            /* VER_PLATFORM_WIN32_NT */
    utf8_to_w("Service Pack 1", p + 20, 128);
    if (R32(p) >= 284) { W16(p + 276, 1); W16(p + 278, 0); W16(p + 280, 0x100); W8(p + 282, 1); }
    return 1;
}

uint32_t k32_GetSystemDirectoryA(CPU *c, uint32_t a) {
    (void)c;
    const char *d = "C:\\Windows\\system32";
    uint32_t n = (uint32_t)strlen(d);
    if (ARG(1) > n) memcpy(HOST(ARG(0)), d, n + 1);
    return ARG(1) > n ? n : n + 1;
}

/* ---- code pages and locale ---- */
uint32_t k32_GetACP(CPU *c, uint32_t a) { (void)c; (void)a; return 1252; }
uint32_t k32_GetOEMCP(CPU *c, uint32_t a) { (void)c; (void)a; return 437; }
uint32_t k32_IsValidCodePage(CPU *c, uint32_t a) { (void)c; return ARG(0) == 1252 || ARG(0) == 437 || ARG(0) == 65001; }
uint32_t k32_GetCPInfo(CPU *c, uint32_t a) {
    (void)c;
    uint32_t p = ARG(1);
    memset(HOST(p), 0, 20);
    W32(p, ARG(0) == 65001 ? 4 : 1);                           /* MaxCharSize */
    W8(p + 4, '?');
    return 1;
}
/* The language: the one chosen in the launcher (port_settings.txt lang=, the launcher's list), English otherwise */
int port_setting(const char *key, int def);
uint32_t port_langid(void);
uint32_t port_langid(void) {
    static const uint16_t ids[12] = {0x0409, 0x040C, 0x0407, 0x0C0A, 0x0410, 0x0416, 0x0415, 0x0419, 0x0413, 0x0405, 0x040E, 0x0411};
    int i = port_setting("lang", 0);
    return ids[i >= 0 && i < 12 ? i : 0];
}
uint32_t k32_GetUserDefaultLCID(CPU *c, uint32_t a) { (void)c; (void)a; return port_langid(); }
uint32_t k32_GetUserDefaultLangID(CPU *c, uint32_t a) { (void)c; (void)a; return port_langid(); }
uint32_t k32_IsValidLocale(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
uint32_t k32_EnumSystemLocalesA(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }

static const char *locale_field(uint32_t type) {
    switch (type & 0xFFFFu) {
    case 0x0B: return "1252";            /* LOCALE_IDEFAULTANSICODEPAGE */
    case 0x0C: return "437";             /* LOCALE_IDEFAULTCODEPAGE */
    case 0x01: {                         /* LOCALE_ILANGUAGE: the launcher's language */
        static char id[8];
        snprintf(id, sizeof id, "%04x", port_langid());
        return id;
    }
    case 0x59: {                         /* LOCALE_SISO639LANGNAME */
        static const char *iso[12] = {"en", "fr", "de", "es", "it", "pt", "pl", "ru", "nl", "cs", "hu", "ja"};
        int i = port_setting("lang", 0);
        return iso[i >= 0 && i < 12 ? i : 0];
    }
    case 0x5A: return "US";              /* LOCALE_SISO3166CTRYNAME */
    case 0x0E: return ".";               /* decimal separator */
    case 0x0F: return ",";               /* thousand separator */
    case 0x1001: return "English";
    case 0x1002: return "United States";
    default: return "";
    }
}
uint32_t k32_GetLocaleInfoA(CPU *c, uint32_t a) {
    (void)c;
    const char *s = locale_field(ARG(1));
    uint32_t n = (uint32_t)strlen(s) + 1;
    if (!ARG(3)) return n;
    if (ARG(3) < n) return 0;
    memcpy(HOST(ARG(2)), s, n);
    return n;
}
uint32_t k32_GetLocaleInfoW(CPU *c, uint32_t a) {
    (void)c;
    const char *s = locale_field(ARG(1));
    uint32_t n = (uint32_t)strlen(s) + 1;
    if (!ARG(3)) return n;
    if (ARG(3) < n) return 0;
    utf8_to_w(s, ARG(2), ARG(3));
    return n;
}

/* MultiByteToWideChar / WideCharToMultiByte: code page 1252 and UTF-8 (65001); Latin-1 range only for 1252 */
uint32_t k32_MultiByteToWideChar(CPU *c, uint32_t a) {
    (void)c;
    uint32_t cp = ARG(0), src = ARG(2), dst = ARG(4), cap = ARG(5);
    int32_t len = (int32_t)ARG(3);
    if (len < 0) len = (int32_t)strlen(STR(src)) + 1;
    uint32_t o = 0;
    for (int32_t i = 0; i < len;) {
        uint32_t ch = R8(src + (uint32_t)i);
        int k = 1;
        if (cp == 65001 && ch >= 0x80) {
            if ((ch & 0xE0) == 0xC0) { ch = (ch & 31u) << 6 | (R8(src + i + 1) & 63u); k = 2; }
            else if ((ch & 0xF0) == 0xE0) { ch = (ch & 15u) << 12 | (R8(src + i + 1) & 63u) << 6 | (R8(src + i + 2) & 63u); k = 3; }
        }
        if (cap) { if (o >= cap) return 0; W16(dst + 2u * o, ch); }
        o++;
        i += k;
    }
    return o;
}

uint32_t k32_WideCharToMultiByte(CPU *c, uint32_t a) {
    (void)c;
    uint32_t cp = ARG(0), src = ARG(2), dst = ARG(4), cap = ARG(5);
    int32_t len = (int32_t)ARG(3);
    if (len < 0) len = (int32_t)wlen(src) + 1;
    uint32_t o = 0;
    for (int32_t i = 0; i < len; i++) {
        uint32_t ch = R16(src + 2u * (uint32_t)i);
        uint8_t b[3];
        int k = 1;
        if (cp == 65001 && ch >= 0x80) {
            if (ch < 0x800) { b[0] = (uint8_t)(0xC0 | ch >> 6); b[1] = (uint8_t)(0x80 | (ch & 63)); k = 2; }
            else { b[0] = (uint8_t)(0xE0 | ch >> 12); b[1] = (uint8_t)(0x80 | ((ch >> 6) & 63)); b[2] = (uint8_t)(0x80 | (ch & 63)); k = 3; }
        } else {
            b[0] = ch < 0x100 ? (uint8_t)ch : '?';
            if (ch >= 0x100 && ARG(7)) W32(ARG(7), 1);
        }
        if (cap) { if (o + (uint32_t)k > cap) return 0; for (int j = 0; j < k; j++) W8(dst + o + j, b[j]); }
        o += (uint32_t)k;
    }
    return o;
}

uint32_t k32_GetStringTypeW(CPU *c, uint32_t a) {
    (void)c;
    uint32_t src = ARG(1), out = ARG(3);
    int32_t len = (int32_t)ARG(2);
    if (len < 0) len = (int32_t)wlen(src) + 1;
    for (int32_t i = 0; i < len; i++) {
        uint32_t ch = R16(src + 2u * (uint32_t)i), t = 0;
        if (ch >= 'A' && ch <= 'Z') t = 0x001 | 0x100;          /* C1_UPPER | C1_ALPHA */
        else if (ch >= 'a' && ch <= 'z') t = 0x002 | 0x100;
        else if (ch >= '0' && ch <= '9') t = 0x004 | 0x080;    /* DIGIT | XDIGIT */
        else if (ch == ' ' || (ch >= 9 && ch <= 13)) t = 0x008 | (ch == ' ' || ch == 9 ? 0x040 : 0);
        else if (ch < 32 || ch == 127) t = 0x020;              /* CNTRL */
        else if (ch < 128) t = 0x010;                          /* PUNCT */
        if ((ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')) t |= 0x080;
        W16(out + 2u * (uint32_t)i, t);
    }
    return 1;
}

uint32_t k32_LCMapStringW(CPU *c, uint32_t a) {
    (void)c;
    uint32_t flags = ARG(1), src = ARG(2), dst = ARG(4), cap = ARG(5);
    int32_t len = (int32_t)ARG(3);
    if (len < 0) len = (int32_t)wlen(src) + 1;
    if (!cap) return (uint32_t)len;
    if ((uint32_t)len > cap) return 0;
    for (int32_t i = 0; i < len; i++) {
        uint32_t ch = R16(src + 2u * (uint32_t)i);
        if ((flags & 0x100u) && ch >= 'A' && ch <= 'Z') ch += 32;      /* LCMAP_LOWERCASE */
        if ((flags & 0x200u) && ch >= 'a' && ch <= 'z') ch -= 32;      /* LCMAP_UPPERCASE */
        W16(dst + 2u * (uint32_t)i, ch);
    }
    return (uint32_t)len;
}

static int wcmp(uint32_t p, int32_t n, uint32_t q, int32_t m, int nocase) {
    if (n < 0) n = (int32_t)wlen(p);
    if (m < 0) m = (int32_t)wlen(q);
    for (int32_t i = 0; i < n && i < m; i++) {
        uint32_t x = R16(p + 2u * (uint32_t)i), y = R16(q + 2u * (uint32_t)i);
        if (nocase) { if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; }
        if (x != y) return x < y ? -1 : 1;
    }
    return n == m ? 0 : n < m ? -1 : 1;
}
uint32_t k32_CompareStringW(CPU *c, uint32_t a) {
    (void)c;
    return (uint32_t)(2 + wcmp(ARG(2), (int32_t)ARG(3), ARG(4), (int32_t)ARG(5), ARG(1) & 1u));
}
uint32_t k32_lstrcmpW(CPU *c, uint32_t a) { (void)c; return (uint32_t)wcmp(ARG(0), -1, ARG(1), -1, 0); }

/* ---- time ---- */
static uint64_t filetime_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 10000000ull + (uint64_t)ts.tv_nsec / 100ull + 116444736000000000ull;
}
uint32_t k32_GetSystemTimeAsFileTime(CPU *c, uint32_t a) { (void)c; W64(ARG(0), filetime_now()); return 0; }
uint32_t k32_GetTickCount(CPU *c, uint32_t a) {
    (void)c; (void)a;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull);
}
uint32_t x_timeGetTime(CPU *c, uint32_t a) { return k32_GetTickCount(c, a); }
uint32_t x_timeBeginPeriod(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t x_timeEndPeriod(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t k32_QueryPerformanceFrequency(CPU *c, uint32_t a) { (void)c; W64(ARG(0), 10000000ull); return 1; }
uint32_t k32_QueryPerformanceCounter(CPU *c, uint32_t a) {
    (void)c;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    W64(ARG(0), (uint64_t)ts.tv_sec * 10000000ull + (uint64_t)ts.tv_nsec / 100ull);
    return 1;
}

static void filetime_to_systemtime(uint64_t ft, uint32_t st) {
    time_t t = (time_t)((ft - 116444736000000000ull) / 10000000ull);
    struct tm tm;
    gmtime_r(&t, &tm);
    W16(st + 0, tm.tm_year + 1900); W16(st + 2, tm.tm_mon + 1); W16(st + 4, tm.tm_wday); W16(st + 6, tm.tm_mday);
    W16(st + 8, tm.tm_hour); W16(st + 10, tm.tm_min); W16(st + 12, tm.tm_sec); W16(st + 14, (ft / 10000ull) % 1000);
}
uint32_t k32_GetSystemTime(CPU *c, uint32_t a) { (void)c; filetime_to_systemtime(filetime_now(), ARG(0)); return 0; }
uint32_t k32_FileTimeToSystemTime(CPU *c, uint32_t a) { (void)c; filetime_to_systemtime(R64(ARG(0)), ARG(1)); return 1; }
uint32_t k32_SystemTimeToFileTime(CPU *c, uint32_t a) {
    (void)c;
    uint32_t st = ARG(0);
    struct tm tm = {0};
    tm.tm_year = R16(st) - 1900; tm.tm_mon = R16(st + 2) - 1; tm.tm_mday = R16(st + 6);
    tm.tm_hour = R16(st + 8); tm.tm_min = R16(st + 10); tm.tm_sec = R16(st + 12);
    uint64_t t = (uint64_t)timegm(&tm);
    W64(ARG(1), t * 10000000ull + R16(st + 14) * 10000ull + 116444736000000000ull);
    return 1;
}
uint32_t k32_FileTimeToLocalFileTime(CPU *c, uint32_t a) { (void)c; W64(ARG(1), R64(ARG(0))); return 1; }
uint32_t k32_LocalFileTimeToFileTime(CPU *c, uint32_t a) { (void)c; W64(ARG(1), R64(ARG(0))); return 1; }
uint32_t k32_CompareFileTime(CPU *c, uint32_t a) {
    (void)c;
    uint64_t x = R64(ARG(0)), y = R64(ARG(1));
    return x < y ? 0xFFFFFFFFu : x > y;
}
uint32_t k32_GetTimeZoneInformation(CPU *c, uint32_t a) { (void)c; memset(HOST(ARG(0)), 0, 172); return 0; }
uint32_t k32_Sleep(CPU *c, uint32_t a) { (void)c; usleep(ARG(0) ? ARG(0) * 1000u : 0); if (!ARG(0)) sched_yield(); return 0; }

/* ---- COM and the device APIs not answered yet: fail cleanly so the game takes its "not available" path ---- */
uint32_t x_CoInitializeEx(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t x_CoInitialize(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t x_CoUninitialize(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t xaudio2_create(uint32_t clsid, uint32_t out);
uint32_t wmi_create(uint32_t clsid, uint32_t out);
uint32_t x_CoCreateInstance(CPU *c, uint32_t a) {
    (void)c;
    if (xaudio2_create(ARG(0), ARG(4)) == 0) return 0;       /* XAudio2 2.7 (the game's SoundAdapter_XAudio) */
    if (wmi_create(ARG(0), ARG(4)) == 0) return 0;           /* WbemLocator: the XInput pad check (hle_wmi.c) */
    const uint8_t *g = HOST(ARG(0));
    usf_log("CoCreateInstance {%08x-%04x-%04x-...}: class not registered\n", R32(ARG(0)), g[4] | g[5] << 8, g[6] | g[7] << 8);
    if (ARG(4)) W32(ARG(4), 0);
    return 0x80040154u;                                        /* REGDB_E_CLASSNOTREG */
}

/* RaiseException: MSVC's "set thread name" (0x406D1388) is for debuggers and is skipped. Anything else is a real
 * exception (C++ throw = 0xE06D7363): not supported yet, so it is reported. */
uint32_t k32_RaiseException(CPU *c, uint32_t a) {
    if (ARG(0) == 0x406D1388u) return 0;
    fprintf(stderr, "usf: RaiseException %08x (flags %x, %u args) from %08x: exceptions are not handled yet\n", ARG(0),
            ARG(1), ARG(2), R32(c->esp));
    return 0;
}

/* ---- memory queries ---- */
uint32_t k32_HeapDestroy(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
uint32_t k32_VirtualQuery(CPU *c, uint32_t a) {
    (void)c;
    uint32_t p = ARG(1);
    memset(HOST(p), 0, 28);
    W32(p, ARG(0) & ~0xFFFu); W32(p + 4, ARG(0) & ~0xFFFFu); W32(p + 8, 4); W32(p + 12, 0x1000);
    W32(p + 16, 0x1000);                                       /* MEM_COMMIT */
    W32(p + 20, 4);                                            /* PAGE_READWRITE */
    W32(p + 24, 0x20000);                                      /* MEM_PRIVATE */
    return 28;
}
