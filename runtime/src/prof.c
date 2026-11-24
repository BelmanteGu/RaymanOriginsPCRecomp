/* USF_PROF=<seconds>: a sampling profile of the whole process (analysis; PC and Android).
 *
 * What it does: ITIMER_PROF sends SIGPROF about every millisecond of CPU the process uses; the handler keeps the
 * interrupted program counter and whether it was the game's main thread. After <seconds> (from prof_start) a thread
 * stops the timer and prints the 40 functions with the most samples (dladdr names: the lifted f_XXXXXXXX, the
 * runtime's, DXVK's, libc's), the main thread's and all threads', to stderr (logcat on Android).
 * USF_PROF_WAIT=<seconds> waits that long before sampling (to skip the start-up).
 * What it does not do: call stacks (only the function the time is spent in).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

#define MAXS (1 << 20)
static uintptr_t *pcs;
static uint8_t *is_main;
static volatile int n_s, on;
static pid_t main_tid;

static pid_t my_tid(void) { return (pid_t)gettid(); }

static void on_prof(int sig, siginfo_t *si, void *ctx) {
    (void)sig; (void)si;
    if (!on) return;
    ucontext_t *uc = ctx;
#if defined(__aarch64__)
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.pc;
#elif defined(__x86_64__)
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#else
    uintptr_t pc = 0;
#endif
    int i = __atomic_fetch_add(&n_s, 1, __ATOMIC_RELAXED);
    if (i < MAXS) { pcs[i] = pc; is_main[i] = my_tid() == main_tid; }
}

typedef struct { const void *sym; const char *name; int all, main; } Row;
static int by_main(const void *a, const void *b) { const Row *x = a, *y = b; return y->main - x->main; }
static int by_all(const void *a, const void *b) { const Row *x = a, *y = b; return y->all - x->all; }

static void report(void) {
    int n = n_s < MAXS ? n_s : MAXS, nm = 0;
    Row *rows = calloc(65536, sizeof *rows);
    int nr = 0;
    for (int i = 0; i < n; i++) {
        Dl_info di;
        const void *sym = NULL;
        const char *name = "?";
        if (dladdr((void *)pcs[i], &di)) { sym = di.dli_saddr ? di.dli_saddr : di.dli_fbase; name = di.dli_sname ? di.dli_sname : di.dli_fname; }
        int k = 0;
        while (k < nr && rows[k].sym != sym) k++;
        if (k == nr && nr < 65535) { rows[nr].sym = sym; rows[nr].name = name; nr++; }
        rows[k].all++;
        if (is_main[i]) { rows[k].main++; nm++; }
    }
    fprintf(stderr, "usf: prof: %d samples (main thread %d)\n", n, nm);
    qsort(rows, (size_t)nr, sizeof *rows, by_main);
    fprintf(stderr, "usf: prof: main thread\n");
    for (int k = 0; k < nr && k < 40 && rows[k].main; k++)
        fprintf(stderr, "usf: prof  %5.1f%%  %s\n", 100.0 * rows[k].main / (nm ? nm : 1), rows[k].name);
    qsort(rows, (size_t)nr, sizeof *rows, by_all);
    fprintf(stderr, "usf: prof: all threads\n");
    for (int k = 0; k < nr && k < 25; k++)
        fprintf(stderr, "usf: prof  %5.1f%%  %s\n", 100.0 * rows[k].all / (n ? n : 1), rows[k].name);
    free(rows);
}

static void *runner(void *arg) {
    (void)arg;
    const char *w = getenv("USF_PROF_WAIT");
    if (w) sleep((unsigned)atoi(w));
    struct itimerval it = {{0, 1000}, {0, 1000}};
    on = 1;
    setitimer(ITIMER_PROF, &it, NULL);
    sleep((unsigned)atoi(getenv("USF_PROF")));
    struct itimerval off = {{0, 0}, {0, 0}};
    setitimer(ITIMER_PROF, &off, NULL);
    on = 0;
    report();
    return NULL;
}

/* from the game's main thread, before it starts */
void prof_start(void) {
    if (!getenv("USF_PROF") || atoi(getenv("USF_PROF")) <= 0) return;
    pcs = calloc(MAXS, sizeof *pcs);
    is_main = calloc(MAXS, 1);
    main_tid = my_tid();
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    pthread_t t;
    pthread_create(&t, NULL, runner, NULL);
    pthread_detach(t);
}
