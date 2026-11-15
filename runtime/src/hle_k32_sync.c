/* kernel32 synchronisation: critical sections, events, mutexes, semaphores, waits, Interlocked*.
 *
 * What it does: a CRITICAL_SECTION in guest memory is tied (by its guest address) to a host recursive mutex in a
 * hash table; events, mutexes and semaphores are one host "waitable" (mutex + condition variable + state) behind a
 * handle. WaitForSingleObject / WaitForMultipleObjects wait on those and on threads, with timeouts; a single global
 * condition variable wakes multi-object waits (simple and enough for the few the game does).
 * What it does not do: alertable waits, APCs, or named objects shared between processes (names are ignored).
 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hle.h"

int thread_wait(void *obj, uint32_t ms);

/* ---- critical sections ---- */
#define CS_BUCKETS 4096
typedef struct CsEnt { uint32_t addr; pthread_mutex_t m; struct CsEnt *next; } CsEnt;
static CsEnt *cs_tab[CS_BUCKETS];
static pthread_mutex_t cs_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t *cs_get(uint32_t addr) {
    uint32_t b = (addr >> 3) % CS_BUCKETS;
    pthread_mutex_lock(&cs_lock);
    CsEnt *e = cs_tab[b];
    while (e && e->addr != addr) e = e->next;
    if (!e) {
        e = calloc(1, sizeof *e);
        e->addr = addr;
        pthread_mutexattr_t at;
        pthread_mutexattr_init(&at);
        pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&e->m, &at);
        e->next = cs_tab[b];
        cs_tab[b] = e;
    }
    pthread_mutex_unlock(&cs_lock);
    return &e->m;
}

uint32_t k32_InitializeCriticalSection(CPU *c, uint32_t a) {
    (void)c;
    memset(HOST(ARG(0)), 0, 24);
    W32(ARG(0) + 4, 0xFFFFFFFFu);        /* LockCount = -1, as Windows leaves it */
    cs_get(ARG(0));
    return 0;
}
uint32_t k32_InitializeCriticalSectionAndSpinCount(CPU *c, uint32_t a) { k32_InitializeCriticalSection(c, a); return 1; }
uint32_t k32_DeleteCriticalSection(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t k32_EnterCriticalSection(CPU *c, uint32_t a) { (void)c; pthread_mutex_lock(cs_get(ARG(0))); return 0; }
uint32_t k32_LeaveCriticalSection(CPU *c, uint32_t a) { (void)c; pthread_mutex_unlock(cs_get(ARG(0))); return 0; }
uint32_t k32_TryEnterCriticalSection(CPU *c, uint32_t a) { (void)c; return pthread_mutex_trylock(cs_get(ARG(0))) == 0; }

/* ---- waitable objects ---- */
typedef struct {
    int kind;                  /* H_EVENT, H_MUTEX, H_SEMAPHORE */
    int manual, signaled;      /* events */
    int count, max;            /* semaphores */
    pthread_t owner;           /* mutexes */
    int owned, recursion;
    uint32_t pulses;           /* PulseEvent count: a waiter that saw it change was released */
} Waitable;

static pthread_mutex_t wlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wcond = PTHREAD_COND_INITIALIZER;

static uint32_t new_waitable(int kind, int manual, int signaled, int count, int max) {
    Waitable *w = calloc(1, sizeof *w);
    w->kind = kind; w->manual = manual; w->signaled = signaled; w->count = count; w->max = max;
    return handle_new(kind, w);
}

uint32_t k32_CreateEventA(CPU *c, uint32_t a) { (void)c; return new_waitable(H_EVENT, ARG(1) != 0, ARG(2) != 0, 0, 0); }
uint32_t k32_CreateEventW(CPU *c, uint32_t a) { return k32_CreateEventA(c, a); }
uint32_t k32_CreateSemaphoreA(CPU *c, uint32_t a) {
    (void)c;
    return new_waitable(H_SEMAPHORE, 0, 0, (int)ARG(1), (int)ARG(2));
}
uint32_t k32_CreateSemaphoreW(CPU *c, uint32_t a) { return k32_CreateSemaphoreA(c, a); }
uint32_t k32_CreateMutexA(CPU *c, uint32_t a) {
    (void)c;
    uint32_t h = new_waitable(H_MUTEX, 0, 0, 0, 0);
    if (ARG(1)) { Waitable *w = handle_get(h, H_MUTEX); w->owned = 1; w->recursion = 1; w->owner = pthread_self(); }
    return h;
}
uint32_t k32_CreateMutexW(CPU *c, uint32_t a) { return k32_CreateMutexA(c, a); }

static Waitable *wget(uint32_t h) {
    Waitable *w = handle_get(h, H_EVENT);
    if (!w) w = handle_get(h, H_SEMAPHORE);
    if (!w) w = handle_get(h, H_MUTEX);
    return w;
}

static uint32_t set_event(uint32_t h, int state, int pulse) {
    Waitable *w = handle_get(h, H_EVENT);
    if (!w) return 0;
    pthread_mutex_lock(&wlock);
    w->signaled = state;
    if (pulse) { w->pulses++; w->signaled = 0; }   /* releases the threads waiting now (see wait_many) */
    pthread_cond_broadcast(&wcond);
    pthread_mutex_unlock(&wlock);
    return 1;
}
uint32_t k32_SetEvent(CPU *c, uint32_t a) { (void)c; return set_event(ARG(0), 1, 0); }
uint32_t k32_ResetEvent(CPU *c, uint32_t a) { (void)c; return set_event(ARG(0), 0, 0); }
uint32_t k32_PulseEvent(CPU *c, uint32_t a) { (void)c; return set_event(ARG(0), 1, 1); }

uint32_t k32_ReleaseSemaphore(CPU *c, uint32_t a) {
    (void)c;
    Waitable *w = handle_get(ARG(0), H_SEMAPHORE);
    if (!w) return 0;
    pthread_mutex_lock(&wlock);
    if (ARG(2)) W32(ARG(2), (uint32_t)w->count);
    w->count += (int)ARG(1);
    if (w->max && w->count > w->max) w->count = w->max;
    pthread_cond_broadcast(&wcond);
    pthread_mutex_unlock(&wlock);
    return 1;
}

uint32_t k32_ReleaseMutex(CPU *c, uint32_t a) {
    (void)c;
    Waitable *w = handle_get(ARG(0), H_MUTEX);
    if (!w) return 0;
    pthread_mutex_lock(&wlock);
    if (w->owned && pthread_equal(w->owner, pthread_self()) && --w->recursion == 0) w->owned = 0;
    pthread_cond_broadcast(&wcond);
    pthread_mutex_unlock(&wlock);
    return 1;
}

/* With wlock held: is it signaled for this thread? If take, consume it (auto-reset event, semaphore, mutex). */
static int ready(Waitable *w, int take) {
    switch (w->kind) {
    case H_EVENT:
        if (!w->signaled) return 0;
        if (take && !w->manual) w->signaled = 0;
        return 1;
    case H_SEMAPHORE:
        if (w->count <= 0) return 0;
        if (take) w->count--;
        return 1;
    case H_MUTEX:
        if (w->owned && !pthread_equal(w->owner, pthread_self())) return 0;
        if (take) { w->owned = 1; w->owner = pthread_self(); w->recursion++; }
        return 1;
    }
    return 0;
}

static void deadline(struct timespec *ts, uint32_t ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

/* Returns WAIT_OBJECT_0 + i, or WAIT_TIMEOUT (0x102). Threads are waited for one at a time. */
static uint32_t wait_many(uint32_t n, uint32_t handles_addr, int all, uint32_t ms) {
    for (uint32_t i = 0; i < n; i++) {
        uint32_t h = R32(handles_addr + 4u * i);
        void *t = handle_get(h, H_THREAD);
        if (t) {
            if (thread_wait(t, n == 1 ? ms : 0) && !all) return i;
            if (n == 1) return 0x102u;
        }
    }
    struct timespec ts;
    if (ms != 0xFFFFFFFFu) deadline(&ts, ms);
    pthread_mutex_lock(&wlock);
    uint32_t snap[64];
    for (uint32_t i = 0; i < n && i < 64; i++) { Waitable *w = wget(R32(handles_addr + 4u * i)); snap[i] = w ? w->pulses : 0; }
    for (;;) {
        int got = -1, all_ok = 1;
        for (uint32_t i = 0; i < n; i++) {
            Waitable *w = wget(R32(handles_addr + 4u * i));
            if (!w) continue;
            if (ready(w, 0) || (i < 64 && w->kind == H_EVENT && w->pulses != snap[i])) { if (got < 0) got = (int)i; }
            else all_ok = 0;
        }
        if (!all && got >= 0) {
            ready(wget(R32(handles_addr + 4u * (uint32_t)got)), 1);
            pthread_mutex_unlock(&wlock);
            return (uint32_t)got;
        }
        if (all && all_ok) {
            for (uint32_t i = 0; i < n; i++) { Waitable *w = wget(R32(handles_addr + 4u * i)); if (w) ready(w, 1); }
            pthread_mutex_unlock(&wlock);
            return 0;
        }
        int r = ms == 0xFFFFFFFFu ? pthread_cond_wait(&wcond, &wlock) : pthread_cond_timedwait(&wcond, &wlock, &ts);
        if (r == ETIMEDOUT) { pthread_mutex_unlock(&wlock); return 0x102u; }
    }
}

uint32_t k32_WaitForSingleObject(CPU *c, uint32_t a) {
    (void)c;
    uint32_t tmp = gmalloc(4);
    W32(tmp, ARG(0));
    uint32_t r = wait_many(1, tmp, 0, ARG(1));
    gfree(tmp);
    return r;
}
uint32_t k32_WaitForMultipleObjects(CPU *c, uint32_t a) { (void)c; return wait_many(ARG(0), ARG(1), ARG(2) != 0, ARG(3)); }

uint32_t u32_MsgWaitForMultipleObjects(CPU *c, uint32_t a) {
    (void)c;
    uint32_t ms = ARG(3) > 10u ? 10u : ARG(3);                 /* wake regularly so the message loop runs */
    if (!ARG(0)) { struct timespec t = {0, (long)ms * 1000000L}; nanosleep(&t, NULL); return ARG(0); }
    uint32_t r = wait_many(ARG(0), ARG(1), ARG(2) != 0, ms);
    return r == 0x102u ? ARG(0) : r;                          /* "input available" lets the game pump messages */
}

int file_close_handle(uint32_t h);
uint32_t k32_CloseHandle(CPU *c, uint32_t a) {
    (void)c;
    if (file_close_handle(ARG(0))) return 1;
    void *w = wget(ARG(0));
    handle_close(ARG(0));
    (void)w;                                                   /* objects are leaked on purpose: a waiter may hold them */
    return 1;
}

/* ---- thread odds and ends ---- */
uint32_t k32_SetThreadPriority(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
uint32_t k32_GetThreadPriority(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t k32_SetThreadAffinityMask(CPU *c, uint32_t a) { (void)c; (void)a; return 0xF; }
uint32_t k32_SetThreadIdealProcessor(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t k32_SuspendThread(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }

/* ---- Interlocked (exported functions; the inline `lock` forms are lifted directly) ---- */
#define P32(x) ((uint32_t *)HOST(x))
uint32_t k32_InterlockedIncrement(CPU *c, uint32_t a) { (void)c; return __atomic_add_fetch(P32(ARG(0)), 1u, __ATOMIC_SEQ_CST); }
uint32_t k32_InterlockedDecrement(CPU *c, uint32_t a) { (void)c; return __atomic_sub_fetch(P32(ARG(0)), 1u, __ATOMIC_SEQ_CST); }
uint32_t k32_InterlockedExchange(CPU *c, uint32_t a) { (void)c; return __atomic_exchange_n(P32(ARG(0)), ARG(1), __ATOMIC_SEQ_CST); }
uint32_t k32_InterlockedExchangeAdd(CPU *c, uint32_t a) { (void)c; return __atomic_fetch_add(P32(ARG(0)), ARG(1), __ATOMIC_SEQ_CST); }
uint32_t k32_InterlockedCompareExchange(CPU *c, uint32_t a) {
    (void)c;
    uint32_t exp = ARG(2);
    __atomic_compare_exchange_n(P32(ARG(0)), &exp, ARG(1), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return exp;
}

/* ---- winmm multimedia timers: a host thread calling the game's callback ---- */
typedef struct { uint32_t delay, cb, user, periodic, id; volatile int stop; CPU *c; pthread_t th; } MmTimer;
static MmTimer *timers[64];
uint32_t guest_call(CPU *c, uint32_t addr, int conv, int n, ...);
CPU *cpu_alloc(void);

static void *timer_main(void *p) {
    MmTimer *t = p;
    do {
        struct timespec d = {t->delay / 1000, (long)(t->delay % 1000) * 1000000L};
        nanosleep(&d, NULL);
        if (t->stop) break;
        guest_call(t->c, t->cb, CONV_STDCALL, 5, t->id, 0u, t->user, 0u, 0u);
    } while (t->periodic && !t->stop);
    return NULL;
}

uint32_t x_timeSetEvent(CPU *c, uint32_t a) {
    for (uint32_t i = 1; i < 64; i++)
        if (!timers[i]) {
            MmTimer *t = calloc(1, sizeof *t);
            t->delay = ARG(0) ? ARG(0) : 1; t->cb = ARG(2); t->user = ARG(3); t->periodic = ARG(4) & 1u; t->id = i;
            t->c = cpu_alloc();
            uint32_t stk = gpages(0, 0x40000);
            t->c->fs_base = c->fs_base;                       /* shares the creating thread's TEB (callbacks are short) */
            t->c->esp = stk + 0x40000 - 64;
            timers[i] = t;
            pthread_create(&t->th, NULL, timer_main, t);
            return i;
        }
    return 0;
}
uint32_t x_timeKillEvent(CPU *c, uint32_t a) {
    (void)c;
    if (ARG(0) < 64 && timers[ARG(0)]) { timers[ARG(0)]->stop = 1; timers[ARG(0)] = NULL; }
    return 0;
}

/* SetEvent by handle for the runtime's own threads (the audio mixer's notifications) */
int k32_set_event_handle(uint32_t h) { return (int)set_event(h, 1, 0); }
