/* Guest memory allocation: the game's heap and its VirtualAlloc pages, both inside the 4 GiB guest window.
 *
 * What it does: gmalloc/gfree/grealloc are a boundary-tag allocator with segregated free lists (16-byte aligned
 * blocks, coalescing on free) over the arena 0x01000000..0x40000000; HeapAlloc, LocalAlloc, CoTaskMemAlloc and
 * friends use it. gpages hands out 64 KiB-granular regions from 0x40000000..0x7E000000 (VirtualAlloc, thread stacks,
 * TEBs) with a first-fit list of free ranges. One mutex each; the game allocates from several threads.
 * What it does not do: return memory to the host OS (the reservation is MAP_NORESERVE: untouched pages cost nothing)
 * or honour VirtualAlloc's protection flags (all guest memory is read/write).
 */
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>

#include "hle.h"

#define ARENA_LO 0x01000000u
#define ARENA_HI 0x40000000u
#define PAGES_LO 0x40000000u
#define PAGES_HI 0x7E000000u
#define NBINS 64

/* Block layout: [size|used] header (4 bytes) ... payload ... [size] footer (4 bytes); size includes both, multiple
 * of 16. A free block also holds next/prev free-list links in its first payload dwords. */
static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t bins[NBINS];
static uint32_t top;                    /* first never-used address */

static int bin_of(uint32_t size) {
    int b = 0;
    uint32_t s = size >> 4;
    while (s > 1 && b < NBINS - 1) { s >>= 1; b++; }
    return b;
}

#define HDR(b) R32(b)
#define FTR(b, sz) R32((b) + (sz) - 4u)
#define NEXT(b) R32((b) + 4u)
#define PREV(b) R32((b) + 8u)

static void unlink_free(uint32_t b) {
    uint32_t n = NEXT(b), p = PREV(b);
    if (p) W32(p + 4u, n); else bins[bin_of(HDR(b) & ~15u)] = n;
    if (n) W32(n + 8u, p);
}

static void push_free(uint32_t b, uint32_t sz) {
    W32(b, sz);
    W32(b + sz - 4u, sz);
    int k = bin_of(sz);
    W32(b + 4u, bins[k]);
    W32(b + 8u, 0);
    if (bins[k]) W32(bins[k] + 8u, b);
    bins[k] = b;
}

void heap_init(void) {
    top = ARENA_LO;
    memset(bins, 0, sizeof bins);
}

static uint32_t alloc_locked(uint32_t size) {
    uint32_t need = (size + 8u + 15u) & ~15u;
    if (need < 32u) need = 32u;
    for (int k = bin_of(need); k < NBINS; k++) {
        for (uint32_t b = bins[k]; b; b = NEXT(b)) {
            uint32_t sz = HDR(b) & ~15u;
            if (sz < need) continue;
            unlink_free(b);
            if (sz - need >= 48u) {
                push_free(b + need, sz - need);
                sz = need;
            }
            W32(b, sz | 1u);
            W32(b + sz - 4u, sz | 1u);
            return b + 4u;
        }
    }
    if (top + need > ARENA_HI) return 0;
    uint32_t b = top;
    top += need;
    W32(b, need | 1u);
    W32(b + need - 4u, need | 1u);
    return b + 4u;
}

uint32_t gmalloc(uint32_t size) {
    pthread_mutex_lock(&heap_lock);
    uint32_t p = alloc_locked(size);
    pthread_mutex_unlock(&heap_lock);
    if (!p) usf_log("gmalloc(%u): out of guest heap\n", size);
    return p;
}

uint32_t gcalloc(uint32_t size) {
    uint32_t p = gmalloc(size);
    if (p) memset(HOST(p), 0, size);
    return p;
}

uint32_t gsize(uint32_t p) {
    return p ? (R32(p - 4u) & ~15u) - 8u : 0;
}

void gfree(uint32_t p) {
    if (!p) return;
    pthread_mutex_lock(&heap_lock);
    uint32_t b = p - 4u, sz = HDR(b) & ~15u;
    if (!(HDR(b) & 1u)) { pthread_mutex_unlock(&heap_lock); usf_log("gfree(%08x): not allocated\n", p); return; }
    if (b > ARENA_LO && !(R32(b - 4u) & 1u)) {              /* merge with the free block before */
        uint32_t psz = R32(b - 4u) & ~15u;
        b -= psz;
        unlink_free(b);
        sz += psz;
    }
    if (b + sz < top && !(HDR(b + sz) & 1u)) {              /* merge with the free block after */
        uint32_t nsz = HDR(b + sz) & ~15u;
        unlink_free(b + sz);
        sz += nsz;
    }
    if (b + sz == top) top = b;                             /* give the tail back */
    else push_free(b, sz);
    pthread_mutex_unlock(&heap_lock);
}

uint32_t grealloc(uint32_t p, uint32_t size) {
    if (!p) return gmalloc(size);
    if (!size) { gfree(p); return 0; }
    uint32_t old = gsize(p);
    if (old >= size) return p;
    uint32_t n = gmalloc(size);
    if (n) { memmove(HOST(n), HOST(p), old); gfree(p); }
    return n;
}

/* ---- 64 KiB-granular pages ---- */

typedef struct { uint32_t lo, hi; } Range;
static Range used[4096];
static int nused;
static pthread_mutex_t page_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t gpages(uint32_t want, uint32_t size) {
    size = (size + 0xFFFFu) & ~0xFFFFu;
    pthread_mutex_lock(&page_lock);
    uint32_t at = 0;
    if (want) {                                              /* a fixed address: accept if free */
        int ok = want >= PAGES_LO && want + size <= PAGES_HI;
        for (int i = 0; ok && i < nused; i++) if (want < used[i].hi && want + size > used[i].lo) ok = 0;
        at = ok ? want : 0;
    } else {
        uint32_t cand = PAGES_LO;
        for (int moved = 1; moved;) {                        /* first fit over the (unsorted) used list */
            moved = 0;
            for (int i = 0; i < nused; i++)
                if (cand < used[i].hi && cand + size > used[i].lo) { cand = used[i].hi; moved = 1; }
        }
        at = cand + size <= PAGES_HI ? cand : 0;
    }
    if (at && nused < 4096) {
        used[nused].lo = at;
        used[nused].hi = at + size;
        nused++;
        memset(HOST(at), 0, size);
    } else {
        at = 0;
    }
    pthread_mutex_unlock(&page_lock);
    return at;
}

void gpages_free(uint32_t addr) {
    pthread_mutex_lock(&page_lock);
    for (int i = 0; i < nused; i++)
        if (used[i].lo == addr) {
            madvise(HOST(addr), used[i].hi - used[i].lo, MADV_DONTNEED);
            used[i] = used[--nused];
            break;
        }
    pthread_mutex_unlock(&page_lock);
}
