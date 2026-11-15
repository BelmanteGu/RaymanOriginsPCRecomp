/* kernel32 functions Rayman Origins imports that USFIV-PC did not: version queries in the old forms, the interlocked
 * singly linked list, global memory, local time, message formatting, and the debugging / pipe / process-snapshot
 * calls the game makes in its crash and "already running" paths.
 *
 * What it does: answers them as Windows 7 would (GetVersion, GetVersionExA), keeps the SList operations atomic with
 * one lock, and fails cleanly where there is nothing to give (named pipes, process snapshots).
 * What it does not do: anything with other processes.
 */
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "game.h"
#include "hle.h"

uint32_t k32_GetStartupInfoA(CPU *c, uint32_t a) { (void)c; memset(HOST(ARG(0)), 0, 68); W32(ARG(0), 68); return 0; }
uint32_t k32_GetVersion(CPU *c, uint32_t a) { (void)c; (void)a; return 0x1DB10106u; }   /* 6.1, build 7601 */
uint32_t k32_GetVersionExA(CPU *c, uint32_t a) {
    (void)c;
    uint32_t p = ARG(0);
    W32(p + 4, 6); W32(p + 8, 1); W32(p + 12, 7601); W32(p + 16, 2);
    snprintf((char *)HOST(p + 20), 128, "Service Pack 1");
    if (R32(p) >= 156) { W16(p + 148, 1); W16(p + 150, 0); W16(p + 152, 0x100); W8(p + 154, 1); }
    return 1;
}
uint32_t k32_GlobalMemoryStatus(CPU *c, uint32_t a) {
    (void)c;
    uint32_t p = ARG(0);
    W32(p, 32); W32(p + 4, 30);
    W32(p + 8, 0x7FFFFFFFu); W32(p + 12, 0x60000000u);           /* physical: 2 GB+ (capped, as Windows does) */
    W32(p + 16, 0x7FFFFFFFu); W32(p + 20, 0x60000000u);          /* page file */
    W32(p + 24, 0x7FFE0000u); W32(p + 28, 0x60000000u);          /* virtual */
    return 0;
}
uint32_t k32_GetUserDefaultUILanguage(CPU *c, uint32_t a) { (void)c; (void)a; return 0x0409; }
uint32_t k32_SleepEx(CPU *c, uint32_t a) { (void)c; usleep(ARG(0) ? ARG(0) * 1000u : 0); if (!ARG(0)) sched_yield(); return 0; }
uint32_t k32_FlushConsoleInputBuffer(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
uint32_t k32_TerminateThread(CPU *c, uint32_t a) {
    (void)c;
    fprintf(stderr, "usf: TerminateThread(%x) ignored\n", ARG(0));
    return 1;
}
uint32_t k32_RtlCaptureContext(CPU *c, uint32_t a) {           /* CONTEXT (x86): integer registers only */
    uint32_t p = ARG(0);
    memset(HOST(p), 0, 716);
    W32(p, 0x10007u);
    W32(p + 0x9C, c->edi); W32(p + 0xA0, c->esi); W32(p + 0xA4, c->ebx); W32(p + 0xA8, c->edx);
    W32(p + 0xAC, c->ecx); W32(p + 0xB0, c->eax); W32(p + 0xB4, c->ebp); W32(p + 0xB8, R32(c->esp));
    W32(p + 0xC4, c->esp + 4);
    return 0;
}

/* ---- environment and directories ---- */
uint32_t k32_ExpandEnvironmentStringsA(CPU *c, uint32_t a) {     /* no variables expanded: copied as they are */
    (void)c;
    uint32_t n = (uint32_t)strlen(STR(ARG(0))) + 1;
    if (ARG(2) >= n) memcpy(HOST(ARG(1)), STR(ARG(0)), n);
    return n;
}
uint32_t k32_GetEnvironmentVariableA(CPU *c, uint32_t a) { set_last_error(c, 203); (void)a; return 0; }   /* not found */
uint32_t k32_GetCurrentDirectoryA(CPU *c, uint32_t a) {
    (void)c;
    uint32_t n = (uint32_t)strlen(GAME_WIN_DIR);
    if (ARG(0) > n) { memcpy(HOST(ARG(1)), GAME_WIN_DIR, n + 1); return n; }
    return n + 1;
}
uint32_t k32_SetCurrentDirectoryA(CPU *c, uint32_t a) { (void)c; usf_log("SetCurrentDirectoryA(%s)\n", STR(ARG(0))); return 1; }

/* ---- global memory (moveable handles are plain pointers) ---- */
uint32_t k32_GlobalAlloc(CPU *c, uint32_t a) { (void)c; return (ARG(0) & 0x40u) ? gcalloc(ARG(1)) : gmalloc(ARG(1)); }
uint32_t k32_GlobalFree(CPU *c, uint32_t a) { (void)c; if (ARG(0)) gfree(ARG(0)); return 0; }

/* ---- time ---- */
uint32_t k32_GetLocalTime(CPU *c, uint32_t a) {
    (void)c;
    struct timespec ts;
    struct tm t;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &t);
    uint32_t p = ARG(0);
    W16(p, t.tm_year + 1900); W16(p + 2, t.tm_mon + 1); W16(p + 4, t.tm_wday); W16(p + 6, t.tm_mday);
    W16(p + 8, t.tm_hour); W16(p + 10, t.tm_min); W16(p + 12, t.tm_sec); W16(p + 14, ts.tv_nsec / 1000000);
    return 0;
}

/* ---- messages ---- */
uint32_t k32_FormatMessageA(CPU *c, uint32_t a) {               /* (flags, source, id, lang, buffer, size, args) */
    (void)c;
    char msg[64];
    int n = snprintf(msg, sizeof msg, "Error %u.\r\n", ARG(2));
    if (ARG(0) & 0x100u) {                                       /* FORMAT_MESSAGE_ALLOCATE_BUFFER */
        uint32_t p = gmalloc((uint32_t)n + 1);
        memcpy(HOST(p), msg, (size_t)n + 1);
        W32(ARG(4), p);
    } else if (ARG(5) > (uint32_t)n) {
        memcpy(HOST(ARG(4)), msg, (size_t)n + 1);
    } else return 0;
    return (uint32_t)n;
}

/* ---- interlocked singly linked list: header = {next, depth:16, sequence:16} ---- */
static pthread_mutex_t slist_lock = PTHREAD_MUTEX_INITIALIZER;
uint32_t k32_InitializeSListHead(CPU *c, uint32_t a) { (void)c; W32(ARG(0), 0); W32(ARG(0) + 4, 0); return 0; }
uint32_t k32_InterlockedPushEntrySList(CPU *c, uint32_t a) {
    (void)c;
    uint32_t h = ARG(0), e = ARG(1);
    pthread_mutex_lock(&slist_lock);
    uint32_t first = R32(h);
    W32(e, first);
    W32(h, e);
    W16(h + 4, R16(h + 4) + 1);
    W16(h + 6, R16(h + 6) + 1);
    pthread_mutex_unlock(&slist_lock);
    return first;
}
uint32_t k32_InterlockedPopEntrySList(CPU *c, uint32_t a) {
    (void)c;
    uint32_t h = ARG(0);
    pthread_mutex_lock(&slist_lock);
    uint32_t first = R32(h);
    if (first) { W32(h, R32(first)); W16(h + 4, R16(h + 4) - 1); W16(h + 6, R16(h + 6) + 1); }
    pthread_mutex_unlock(&slist_lock);
    return first;
}

/* ---- pipes and process snapshots: not available ---- */
uint32_t k32_WaitNamedPipeW(CPU *c, uint32_t a) { set_last_error(c, 2); (void)a; return 0; }
uint32_t k32_TransactNamedPipe(CPU *c, uint32_t a) { set_last_error(c, 6); (void)a; return 0; }
uint32_t k32_SetNamedPipeHandleState(CPU *c, uint32_t a) { set_last_error(c, 6); (void)a; return 0; }
uint32_t k32_CreateToolhelp32Snapshot(CPU *c, uint32_t a) { set_last_error(c, 5); (void)a; return 0xFFFFFFFFu; }
uint32_t k32_Process32NextW(CPU *c, uint32_t a) { set_last_error(c, 18); (void)a; return 0; }
