/* The process the game runs in: guest memory, the image, the PEB, threads with their TEBs, and the entry point.
 *
 * What it does: main() reserves the 4 GiB guest window, loads the game executable at its base, builds the PEB and the main
 * thread (TEB + stack), changes to the game folder and calls the lifted entry point. CreateThread starts a host
 * pthread with its own CPU, TEB and guest stack that calls the game's thread routine. Also the kernel32 functions
 * about the process itself: command line, modules, GetProcAddress, TLS, last error, exit.
 * What it does not do: files, sync objects, memory APIs (hle_k32_*.c) or exceptions (not handled yet).
 */
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>

#include "game.h"
#include "hle.h"

uint8_t *usf_mem;
char g_game_dir[1024];
char g_data_dir[1024];                                        /* USF_DATA, or ~/.local/share/rayman-origins-pc */
int g_headless;                                               /* USF_HEADLESS=1: no window shown, no sound */
static uint32_t g_peb, g_cmdline_a, g_cmdline_w;
static pthread_mutex_t tls_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t tls_used[64];
static uint32_t next_tid = 0x100;

extern uint32_t g_image_base, g_entry;
int load_image(const char *path);
int load_module(const char *path, const char *name, uint32_t *entry);
uint32_t module_base(const char *name);
uint32_t guest_call(CPU *c, uint32_t addr, int conv, int n, ...);
void callsites_init(void);

void usf_log(const char *fmt, ...) {
    if (!usf_log_on) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void hle_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "usf: FATAL: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    _exit(71);
}

/* TEB fields (x86): 0x00 SEH list, 0x04 stack base, 0x08 stack limit, 0x18 self, 0x20 pid, 0x24 tid,
 * 0x2C TLS array pointer, 0x30 PEB, 0x34 last error, 0xE10 TLS slots[64] */
uint32_t teb_of(CPU *c) { return c->fs_base; }
void set_last_error(CPU *c, uint32_t e) { W32(c->fs_base + 0x34u, e); }

static uint32_t new_teb(uint32_t stack_lo, uint32_t stack_hi) {
    uint32_t teb = gpages(0, 0x2000);
    W32(teb + 0x00, 0xFFFFFFFFu);
    W32(teb + 0x04, stack_hi);
    W32(teb + 0x08, stack_lo);
    W32(teb + 0x18, teb);
    W32(teb + 0x20, 0x1000);
    W32(teb + 0x24, __atomic_fetch_add(&next_tid, 4, __ATOMIC_SEQ_CST));
    W32(teb + 0x2C, teb + 0xE10);
    W32(teb + 0x30, g_peb);
    return teb;
}

CPU *cpu_alloc(void) {
    CPU *c = calloc(1, sizeof(CPU));
    c->fpu_cw = 0x027F;                  /* Windows default: 53-bit precision, round to nearest */
    c->mxcsr = 0x1F80;
    return c;
}

static void cpu_on_new_stack(CPU *c, uint32_t size) {
    uint32_t lo = gpages(0, size);
    if (!lo) hle_fatal("no guest memory for a %u-byte stack", size);
    c->fs_base = new_teb(lo, lo + size);
    c->esp = lo + size - 64u;
}

/* a CPU with its own guest stack and TEB, for host threads that call game code (the XAudio2 engine thread) */
CPU *cpu_guest_thread_context(uint32_t stack);
CPU *cpu_guest_thread_context(uint32_t stack) {
    CPU *c = cpu_alloc();
    cpu_on_new_stack(c, stack);
    return c;
}

/* ---- threads ---- */

typedef struct {
    CPU *c;
    uint32_t start, param;
    pthread_t th;
    uint32_t exit_code;
    int done, suspended;
    pthread_mutex_t m;
    pthread_cond_t cv;
} Thread;

static __thread Thread *self_thread;

static void *thread_main(void *p) {
    Thread *t = p;
    self_thread = t;
    pthread_mutex_lock(&t->m);
    while (t->suspended) pthread_cond_wait(&t->cv, &t->m);
    pthread_mutex_unlock(&t->m);
    uint32_t r = guest_call(t->c, t->start, CONV_STDCALL, 1, t->param);
    pthread_mutex_lock(&t->m);
    t->exit_code = r;
    t->done = 1;
    pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->m);
    return NULL;
}

int thread_wait(void *obj, uint32_t ms);   /* used by WaitForSingleObject (hle_k32_sync.c) */
int thread_wait(void *obj, uint32_t ms) {
    Thread *t = obj;
    pthread_mutex_lock(&t->m);
    int ok = 1;
    if (ms == 0xFFFFFFFFu) { while (!t->done) pthread_cond_wait(&t->cv, &t->m); }
    else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += ms / 1000; ts.tv_nsec += (long)(ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        while (!t->done && ok) ok = pthread_cond_timedwait(&t->cv, &t->m, &ts) == 0;
        ok = t->done;
    }
    pthread_mutex_unlock(&t->m);
    return ok;
}

uint32_t k32_CreateThread(CPU *c, uint32_t a) {
    uint32_t stack = ARG(1) ? ARG(1) : 0x100000u;
    Thread *t = calloc(1, sizeof *t);
    t->c = cpu_alloc();
    cpu_on_new_stack(t->c, stack < 0x40000u ? 0x40000u : (stack + 0xFFFFu) & ~0xFFFFu);
    t->start = ARG(2);
    t->param = ARG(3);
    t->suspended = (ARG(4) & 4u) != 0;     /* CREATE_SUSPENDED */
    pthread_mutex_init(&t->m, NULL);
    pthread_cond_init(&t->cv, NULL);
    uint32_t h = handle_new(H_THREAD, t);
    if (ARG(5)) W32(ARG(5), R32(t->c->fs_base + 0x24));
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 8u << 20);
    pthread_create(&t->th, &at, thread_main, t);
    usf_log("CreateThread start %08x param %08x -> handle %x%s\n", t->start, t->param, h, t->suspended ? " (suspended)" : "");
    (void)c;
    return h;
}

uint32_t k32_ResumeThread(CPU *c, uint32_t a) {
    Thread *t = handle_get(ARG(0), H_THREAD);
    (void)c;
    if (!t) return 0xFFFFFFFFu;
    pthread_mutex_lock(&t->m);
    uint32_t prev = (uint32_t)t->suspended;
    t->suspended = 0;
    pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->m);
    return prev;
}

uint32_t k32_GetExitCodeThread(CPU *c, uint32_t a) {
    Thread *t = handle_get(ARG(0), H_THREAD);
    (void)c;
    if (!t) return 0;
    W32(ARG(1), t->done ? t->exit_code : 259u /* STILL_ACTIVE */);
    return 1;
}

uint32_t k32_ExitThread(CPU *c, uint32_t a) {
    (void)c;
    if (self_thread) {
        pthread_mutex_lock(&self_thread->m);
        self_thread->exit_code = ARG(0);
        self_thread->done = 1;
        pthread_cond_broadcast(&self_thread->cv);
        pthread_mutex_unlock(&self_thread->m);
    }
    pthread_exit(NULL);
}

uint32_t k32_GetCurrentThreadId(CPU *c, uint32_t a) { (void)a; return R32(c->fs_base + 0x24); }
uint32_t k32_GetCurrentProcessId(CPU *c, uint32_t a) { (void)c; (void)a; return 0x1000; }
uint32_t k32_GetCurrentThread(CPU *c, uint32_t a) { (void)c; (void)a; return 0xFFFFFFFEu; }
uint32_t k32_GetCurrentProcess(CPU *c, uint32_t a) { (void)c; (void)a; return 0xFFFFFFFFu; }

uint32_t k32_ExitProcess(CPU *c, uint32_t a) {
    (void)c;
    fprintf(stderr, "usf: ExitProcess(%u)\n", ARG(0));
    fflush(NULL);
    _exit((int)ARG(0));
}

uint32_t k32_TerminateProcess(CPU *c, uint32_t a) {
    (void)c;
    fprintf(stderr, "usf: TerminateProcess(%08x, %u)\n", ARG(0), ARG(1));
    fflush(NULL);
    _exit((int)ARG(1));
}

/* ---- TLS and last error ---- */

uint32_t k32_TlsAlloc(CPU *c, uint32_t a) {
    (void)c; (void)a;
    pthread_mutex_lock(&tls_lock);
    for (uint32_t i = 0; i < 64; i++)
        if (!tls_used[i]) { tls_used[i] = 1; pthread_mutex_unlock(&tls_lock); return i; }
    pthread_mutex_unlock(&tls_lock);
    return 0xFFFFFFFFu;
}
uint32_t k32_TlsFree(CPU *c, uint32_t a) { (void)c; if (ARG(0) < 64) tls_used[ARG(0)] = 0; return 1; }
uint32_t k32_TlsGetValue(CPU *c, uint32_t a) {
    set_last_error(c, 0);
    return ARG(0) < 64 ? R32(c->fs_base + 0xE10u + 4u * ARG(0)) : 0;
}
uint32_t k32_TlsSetValue(CPU *c, uint32_t a) {
    if (ARG(0) >= 64) return 0;
    W32(c->fs_base + 0xE10u + 4u * ARG(0), ARG(1));
    return 1;
}
uint32_t k32_GetLastError(CPU *c, uint32_t a) { (void)a; return R32(c->fs_base + 0x34u); }
uint32_t k32_SetLastError(CPU *c, uint32_t a) { set_last_error(c, ARG(0)); return 0; }

/* ---- command line, modules ---- */

uint32_t k32_GetCommandLineA(CPU *c, uint32_t a) { (void)c; (void)a; return g_cmdline_a; }

uint32_t k32_GetModuleHandleA(CPU *c, uint32_t a) {
    (void)c;
    if (!ARG(0)) return g_image_base;
    uint32_t b = module_base(STR(ARG(0)));
    return b ? b : 0x10000000u | (uint32_t)(strlen(STR(ARG(0))) * 0x101u & 0xFFFF);
}

uint32_t k32_GetModuleHandleW(CPU *c, uint32_t a) {
    (void)c;
    if (!ARG(0)) return g_image_base;
    char name[256];
    w2utf8(ARG(0), name, sizeof name);
    usf_log("GetModuleHandleW(%s)\n", name);
    return 0x10000000u | (uint32_t)(strlen(name) * 0x101u & 0xFFFF);   /* a fake, stable module handle */
}

static uint32_t module_name(uint32_t h, char *out, size_t n) {
    if (h == 0 || h == g_image_base) return (uint32_t)snprintf(out, n, "%s\\%s", GAME_WIN_DIR, GAME_EXE);
    return (uint32_t)snprintf(out, n, "C:\\Windows\\System32\\module.dll");
}

uint32_t k32_GetModuleFileNameA(CPU *c, uint32_t a) {
    char buf[260];
    uint32_t len = module_name(ARG(0), buf, sizeof buf);
    if (len >= ARG(2)) len = ARG(2) ? ARG(2) - 1 : 0;
    memcpy(HOST(ARG(1)), buf, len);
    W8(ARG(1) + len, 0);
    set_last_error(c, 0);
    return len;
}

uint32_t k32_GetModuleFileNameW(CPU *c, uint32_t a) {
    char buf[260];
    uint32_t len = module_name(ARG(0), buf, sizeof buf);
    if (len >= ARG(2)) len = ARG(2) ? ARG(2) - 1 : 0;
    buf[len] = 0;
    utf8_to_w(buf, ARG(1), ARG(2));
    set_last_error(c, 0);
    return len;
}

/* LoadLibrary: every DLL "loads"; its functions come from our tables. The handle encodes nothing. */
static char loaded_names[64][64];
static uint32_t nloaded;

static uint32_t load_library(const char *name) {
    for (uint32_t i = 0; i < nloaded; i++)
        if (!strcasecmp(loaded_names[i], name)) return 0x60000000u + 0x10000u * i;
    if (nloaded >= 64) return 0;
    snprintf(loaded_names[nloaded], 64, "%s", name);
    usf_log("LoadLibrary(%s) -> %08x\n", name, 0x60000000u + 0x10000u * nloaded);
    return 0x60000000u + 0x10000u * nloaded++;
}

uint32_t k32_LoadLibraryA(CPU *c, uint32_t a) { (void)c; return load_library(STR(ARG(0))); }
uint32_t k32_LoadLibraryW(CPU *c, uint32_t a) {
    char name[256];
    (void)c;
    w2utf8(ARG(0), name, sizeof name);
    return load_library(name);
}
uint32_t k32_FreeLibrary(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }

/* the C runtime's operator new / delete (msvcr90.dll on Windows; Crypto++ asks for them by name) */
static uint32_t op_new(CPU *c, uint32_t a) { (void)c; return gmalloc(ARG(0) ? ARG(0) : 1); }
static uint32_t op_delete(CPU *c, uint32_t a) { (void)c; gfree(ARG(0)); return 0; }

uint32_t k32_GetProcAddress(CPU *c, uint32_t a) {
    uint32_t h = ARG(0), p = ARG(1);
    char name[128];
    if (p < 0x10000u) snprintf(name, sizeof name, "#%u", p);
    else snprintf(name, sizeof name, "%s", STR(p));
    if (!strcmp(name, "??2@YAPAXI@Z")) return usf_thunk_fn("operator new", op_new, 0, CONV_CDECL);
    if (!strcmp(name, "??3@YAXPAX@Z")) return usf_thunk_fn("operator delete", op_delete, 0, CONV_CDECL);
    const char *dll = "*";
    if (h >= 0x60000000u && (h - 0x60000000u) / 0x10000u < nloaded) dll = loaded_names[(h - 0x60000000u) / 0x10000u];
    const HleImport *imp = hle_find(dll, name);
    if (!imp) imp = hle_find("*", name);
    if (!imp || !imp->fn) {
        usf_log("GetProcAddress(%s, %s) -> not available\n", dll, name);
        set_last_error(c, 127);              /* ERROR_PROC_NOT_FOUND */
        return 0;
    }
    return usf_thunk_fn(name, imp->fn, imp->nargs, imp->conv);
}

/* ---- startup ---- */

static void build_peb(void) {
    g_peb = gpages(0, 0x10000);
    W32(g_peb + 0x08, g_image_base);
    W32(g_peb + 0x18, 0x00150000u);          /* ProcessHeap: the handle GetProcessHeap returns */
    g_cmdline_a = g_peb + 0x1000;
    char cl[300];
    snprintf(cl, sizeof cl, "\"%s\\%s\"", GAME_WIN_DIR, GAME_EXE);
    memcpy(HOST(g_cmdline_a), cl, strlen(cl) + 1);
    g_cmdline_w = g_peb + 0x2000;
    utf8_to_w(cl, g_cmdline_w, 256);
}

#ifdef __ANDROID__
#include <SDL3/SDL_main.h>      /* main becomes SDL_main, called by SDLActivity: argv = game folder, data folder */
#include <android/log.h>
static int log_pipe[2];
static void *log_thread(void *arg) {     /* stderr -> logcat (tag "rayman"), line by line */
    (void)arg;
    char line[1024];
    FILE *in = fdopen(log_pipe[0], "r");
    while (in && fgets(line, sizeof line, in)) __android_log_write(ANDROID_LOG_INFO, "rayman", line);
    return NULL;
}
static void android_log(void) {
    pthread_t t;
    setvbuf(stderr, NULL, _IOLBF, 0);
    if (pipe(log_pipe) == 0) { dup2(log_pipe[1], 2); pthread_create(&t, NULL, log_thread, NULL); }
}
#endif

/* Closing the game: Rayman Origins saves at its checkpoints, so nothing is lost at exit, but its shutdown crashes in
 * the destructor of an online-layer (Quazal) object (0x8F8660, a std::map walked after it was freed). Until that is
 * understood, a fault after the player closed the window ends the process normally, with a note. Before that, a
 * fault is reported as the crash it is. */
#include <signal.h>
extern volatile int g_quitting;
static void on_fault(int sig) {
    if (g_quitting) {
        static const char m[] = "usf: fault during the game's shutdown (known, see process.c): exiting normally\n";
        if (write(2, m, sizeof m - 1) < 0) {}
        _exit(0);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

int main(int argc, char **argv) {
    signal(SIGSEGV, on_fault);
    signal(SIGBUS, on_fault);
#ifdef __ANDROID__
    android_log();
#endif
    g_headless = getenv("USF_HEADLESS") != NULL;
    if (g_headless) setenv("SDL_AUDIO_DRIVER", "dummy", 1);   /* sound is mixed (and can be dumped) but not played */
    const char *data = getenv("USF_DATA");
    if (!data && argc > 2) data = argv[2];
    if (data) snprintf(g_data_dir, sizeof g_data_dir, "%s", data);
    else snprintf(g_data_dir, sizeof g_data_dir, "%s/.local/share/" GAME_DATA_NAME, getenv("HOME"));
    mkdir(g_data_dir, 0755);
#ifdef __ANDROID__
    {   /* development: <data>/env.txt lines KEY=VALUE become environment variables (USF_SHOT, USF_LOG...) */
        char ep[1100], line[512];
        snprintf(ep, sizeof ep, "%s/env.txt", g_data_dir);
        FILE *ef = fopen(ep, "r");
        while (ef && fgets(line, sizeof line, ef)) {
            line[strcspn(line, "\r\n")] = 0;
            char *eq = strchr(line, '=');
            if (eq && line[0] != '#') { *eq = 0; setenv(line, eq + 1, 1); }
        }
        if (ef) fclose(ef);
    }
#endif
    const char *game = getenv(GAME_ENV);
    snprintf(g_game_dir, sizeof g_game_dir, "%s",
             game ? game : (argc > 1 ? argv[1] : ""));
    if (!g_game_dir[0])
        snprintf(g_game_dir, sizeof g_game_dir, "%s" GAME_DEFAULT_DIR, getenv("HOME"));
    usf_log_on = getenv("USF_LOG") != NULL;
    { void touch_init(void); touch_init(); }   /* the on-screen controls and the port settings (touch.c) */
    usf_mem = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (usf_mem == MAP_FAILED) { perror("usf: reserve 4 GiB"); return 1; }
    mprotect(usf_mem, 0x10000, PROT_NONE);  /* null pointer accesses fault */
    heap_init();
    callsites_init();
    if (!getenv("USF_EXTRA_STARTS")) {                       /* absolute path: the process changes directory */
        static char es[1200];
        if (getcwd(es, 1000)) { strcat(es, "/private/analysis/extra_starts.txt"); setenv("USF_EXTRA_STARTS", es, 0); }
    }                        /* before chdir: the table path is relative to the repository */
    char exe[1200];
    snprintf(exe, sizeof exe, "%s/%s", g_game_dir, GAME_EXE);
    if (load_image(exe)) { fprintf(stderr, "usf: cannot load %s\n", exe); return 1; }
    if (chdir(g_game_dir)) { perror("usf: chdir to the game folder"); return 1; }
    build_peb();
    CPU *c = cpu_alloc();
    cpu_on_new_stack(c, 0x400000u);
    fprintf(stderr, "usf: starting %s at %08x (game folder %s)\n", GAME_EXE, g_entry, g_game_dir);
    c->esp -= 4u;
    W32(c->esp, THUNK_BASE - 16u);
    usf_icall(c, g_entry);
    fprintf(stderr, "usf: entry point returned eax=%08x\n", c->eax);
    return (int)c->eax;
}
