/* The DXVK bridge's locks (see dxvk_bridge.h): the game locks a guest copy that Unlock puts into DXVK's memory;
 * texture levels keep their guest copy between locks; small render targets read back by the CPU go through
 * system-memory copies; large dynamic buffers rotate through a ring of DXVK buffers on DISCARD; dynamic vertex and
 * index data is copied at draw time, only the range each draw reads.
 */
#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#include "d3d9.h"
#include "dxvk_bridge.h"

#define BAD 0x8876086Cu
/* ---- locks: the game writes into a guest copy; Unlock puts it into DXVK's memory ---- */
#define LOCK_READONLY 0x10u
#define LOCK_NOOVERWRITE 0x1000u
#define LOCK_DISCARD 0x2000u
int dxl_block_format(uint32_t f) { return f == 0x31545844u || f == 0x32545844u || f == 0x33545844u || f == 0x34545844u || f == 0x35545844u || f == 0x31495441u || f == 0x32495441u; }
void dxl_lock_push(Wrap *w, uint32_t key, uint32_t guest, uint32_t size, void *host) {
    Lock *l = malloc(sizeof *l);
    *l = (Lock){key, guest, size, host, w->locks};
    w->locks = l;
}
Lock *dxl_lock_pop(Wrap *w, uint32_t key) {
    for (Lock **pl = &w->locks; *pl; pl = &(*pl)->next)
        if (((*pl)->key & 0x0FFFFFFFu) == key) { Lock *l = *pl; *pl = l->next; return l; }
    return NULL;
}
/* a rectangle lock (texture level, cube face level, surface): desc = D3DSURFACE_DESC of what is locked */
/* A rectangle lock (texture level, cube face level, surface); desc = D3DSURFACE_DESC of what is locked.
 * Each level keeps a guest copy for the texture's life. DXVK's level is locked whole; the game gets the copy (at its
 * rectangle) and Unlock writes the rectangle's rows back. The copy is refreshed from DXVK only when the GPU may have
 * written the level since (render targets and depth surfaces always; any texture after a device-side copy into one,
 * gpu_epoch): a movie frame or a dynamic texture the CPU fills each frame is copied once per frame, not twice. */
uint32_t dxl_lock_rect(Wrap *w, int slot, uint64_t face, uint64_t level, uint32_t g_lr, uint32_t g_rect,
                          uint32_t flags, const uint32_t *desc, uint32_t key) {
    struct { int32_t pitch; int32_t pad; uint8_t *bits; } lr = {0};
    int32_t r;
    double t_lock = usf_stalls > 0 ? now_ms() : 0;
    if (w->type == DXB_SURFACE) r = (int32_t)dxb_method(w->nat, slot)(w->nat, (uint64_t)(uintptr_t)&lr, 0, flags, 0, 0, 0, 0, 0, 0, 0, 0);
    else if (w->type == DXB_CUBE) r = (int32_t)dxb_method(w->nat, slot)(w->nat, face, level, (uint64_t)(uintptr_t)&lr, 0, flags, 0, 0, 0, 0, 0, 0);
    else r = (int32_t)dxb_method(w->nat, slot)(w->nat, level, (uint64_t)(uintptr_t)&lr, 0, flags, 0, 0, 0, 0, 0, 0, 0);
    if (usf_stalls > 0 && now_ms() - t_lock > 1.0)
        fprintf(stderr, "usf: slow lock %.1f ms: type %d fmt %08x usage %x pool %u %ux%u flags %x level %u\n",
                now_ms() - t_lock, w->type, desc[0], desc[2], desc[3], desc[6], desc[7], flags, (unsigned)level);
    if (r < 0) return (uint32_t)r;
    int blk = dxl_block_format(desc[0]);
    uint32_t pitch = (uint32_t)lr.pitch, rows = blk ? (desc[7] + 3) / 4 : desc[7], full = pitch * rows;
    if (!w->tsh) w->tsh = calloc(6 * 16, sizeof *w->tsh);
    TexShadow *ts = &w->tsh[(face % 6) * 16 + (level & 15)];
    if (!ts->guest || ts->size != full) {
        if (ts->guest) gfree(ts->guest);
        ts->guest = gmalloc(full ? full : 16);
        ts->size = full;
        ts->epoch = 0;
    }
    int gpu_written = (desc[2] & 3u) != 0;                 /* D3DUSAGE_RENDERTARGET / DEPTHSTENCIL */
    if (!(flags & LOCK_DISCARD) && (ts->epoch != gpu_epoch || gpu_written)) { memcpy(HOST(ts->guest), lr.bits, full); copied_tex_in += full; }
    ts->epoch = gpu_epoch;
    uint32_t y0 = 0, y1 = rows, xoff = 0;
    if (g_rect) {
        uint32_t left = R32(g_rect), top = R32(g_rect + 4), bottom = R32(g_rect + 12);
        y0 = blk ? top / 4 : top;
        y1 = blk ? (bottom + 3) / 4 : bottom;
        if (y1 > rows) y1 = rows;
        xoff = blk ? (left / 4) * (desc[0] == 0x31545844u ? 8u : 16u) : left * d3d_format_bpp(desc[0]) / 8;
    }
    dxl_lock_push(w, key | 0x20000000u | (flags & LOCK_READONLY ? 0x80000000u : 0), ts->guest + y0 * pitch,
              (y1 > y0 ? y1 - y0 : 0) * pitch, lr.bits + (size_t)y0 * pitch);
    W32(g_lr, pitch);
    W32(g_lr + 4, ts->guest + y0 * pitch + xoff);
    return 0;
}
/* A small render target the CPU reads every frame (the game's 1x1 R32F average luminance, for the exposure): a
 * direct lock waits until the GPU has finished everything before it (2-20 ms a frame, and the CPU and GPU no longer
 * overlap). Instead each lock asks for a copy into one of four system-memory surfaces (GetRenderTargetData, queued
 * like a draw) and reads the copy made three locks ago, long done: the exposure follows the scene 3 frames late.
 * Returns 1 if done; 0 if not possible (the caller locks normally). USF_SYNC_READBACK=1 turns it off. */
#define RB_N 4
int dxl_lock_readback(Wrap *w, uint32_t g_lr, uint32_t g_rect, uint32_t flags, const uint32_t *desc) {
    static int off = -1;
    if (off < 0) off = getenv("USF_SYNC_READBACK") != NULL;
    if (off || w->rb_off || (flags & LOCK_DISCARD) || !(desc[2] & 1u) || desc[3] != 0 || desc[6] * desc[7] > 64 * 64)
        return 0;
    void *dev = dxb_nat(device_guest);
    if (!w->rb[0]) {
        for (int i = 0; i < RB_N; i++)
            if ((int32_t)dxb_method(dev, 36)(dev, desc[6], desc[7], desc[0], 2 /* D3DPOOL_SYSTEMMEM */,
                                             (uint64_t)(uintptr_t)&w->rb[i], 0, 0, 0, 0, 0, 0) < 0 || !w->rb[i]) {
                w->rb_off = 1;
                return 0;
            }
    }
    uint32_t cur = w->rb_n % RB_N;
    if ((int32_t)dxb_method(dev, 32)(dev, (uint64_t)(uintptr_t)w->nat, (uint64_t)(uintptr_t)w->rb[cur], 0, 0, 0, 0, 0, 0, 0, 0, 0) < 0)
        return 0;
    void *src = w->rb[w->rb_n >= RB_N - 1 ? (w->rb_n - (RB_N - 1)) % RB_N : cur];
    w->rb_n++;
    struct { int32_t pitch; int32_t pad; uint8_t *bits; } lr = {0};
    if ((int32_t)dxb_method(src, 13)(src, (uint64_t)(uintptr_t)&lr, 0, LOCK_READONLY, 0, 0, 0, 0, 0, 0, 0, 0) < 0)
        return 0;
    uint32_t pitch = (uint32_t)lr.pitch, full = pitch * desc[7];
    if (!w->shadow || w->shadow_size != full) { if (w->shadow) gfree(w->shadow); w->shadow = gmalloc(full + 16); w->shadow_size = full; }
    memcpy(HOST(w->shadow), lr.bits, full);
    dxb_method(src, 14)(src, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    uint32_t at = w->shadow;
    if (g_rect) at += R32(g_rect + 4) * pitch + R32(g_rect) * d3d_format_bpp(desc[0]) / 8;
    W32(g_lr, pitch);
    W32(g_lr + 4, at);
    w->rb_open = 1;
    return 1;
}
uint32_t dxl_unlock(Wrap *w, int slot, uint32_t key, uint64_t a1, uint64_t a2) {
    Lock *l = dxl_lock_pop(w, key);
    if (l) {
        if (!(l->key & 0x80000000u)) { memcpy(l->host, HOST(l->guest), l->size); copied_tex_out += l->size; }
        if (w->type == DXB_TEXTURE && !(l->key & 0xFFFFu) && !(l->key & 0x80000000u)) {   /* level 0: content hash */
            uint32_t base = w->tsh ? w->tsh[0].guest : l->guest, sz = w->tsh ? w->tsh[0].size : l->size;
            uint32_t h = 2166136261u, n = sz < 4096 ? sz : 4096;                  /* (analysis: USF_HIDE_TEX*) */
            const uint8_t *b = HOST(base);
            for (uint32_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
            w->code_hash = h;
        }
        if (w->type == DXB_SURFACE && w->parent && !w->plevel && !(l->key & 0x80000000u)) {   /* a texture's level 0 */
            uint32_t base = w->tsh ? w->tsh[0].guest : l->guest, sz = w->tsh ? w->tsh[0].size : l->size;
            uint32_t h = 2166136261u, n = sz < 4096 ? sz : 4096;
            const uint8_t *b = HOST(base);
            for (uint32_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
            Wrap *t = dxb_of(w->parent);
            if (t) t->code_hash = h;
        }
        if (!(l->key & 0x20000000u)) gfree(l->guest);       /* (texture levels keep their copy) */
        free(l);
    }
    return (uint32_t)dxb_method(w->nat, slot)(w->nat, a1, a2, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}
uint32_t dxl_lock_box(Wrap *w, int slot, uint64_t level, uint32_t g_lb, uint32_t g_box, uint32_t flags,
                         uint32_t depth, uint32_t key) {
    struct { int32_t row, slice; uint8_t *bits; } lb = {0};
    uint64_t box = g_box ? (uint64_t)(uintptr_t)HOST(g_box) : 0;
    int32_t r = w->type == DXB_VOLUME
        ? (int32_t)dxb_method(w->nat, slot)(w->nat, (uint64_t)(uintptr_t)&lb, box, flags, 0, 0, 0, 0, 0, 0, 0, 0)
        : (int32_t)dxb_method(w->nat, slot)(w->nat, level, (uint64_t)(uintptr_t)&lb, box, flags, 0, 0, 0, 0, 0, 0, 0);
    if (r < 0) return (uint32_t)r;
    uint32_t d = g_box ? R32(g_box + 20) - R32(g_box + 16) : depth, size = (uint32_t)lb.slice * d;
    uint32_t g = gmalloc(size ? size : 16);
    if (!(flags & LOCK_DISCARD)) memcpy(HOST(g), lb.bits, size);
    dxl_lock_push(w, key | (flags & LOCK_READONLY ? 0x80000000u : 0), g, size, lb.bits);
    W32(g_lb, (uint32_t)lb.row); W32(g_lb + 4, (uint32_t)lb.slice); W32(g_lb + 8, g);
    return 0;
}
/* A large dynamic buffer the game discards each frame (three 24 MB vertex buffers in a race): DXVK gives every
 * DISCARD a new 24 MB slice, too big for its allocation cache, so each costs a fresh allocation (4 ms on an
 * integrated GPU; 13 ms a frame for the three). Instead the buffer gets four DXVK buffers in a ring: DISCARD moves
 * to the next one, last used 5 or more frames ago (DXVK lets the GPU fall at most 3 behind, so it is idle), which
 * is then written without waiting (NOOVERWRITE), and the streams or indices it is bound to are re-bound. Discarded
 * more often than that, DXVK's own DISCARD is used. AddRef / Release go to the first (the one the game made).
 * Returns the flags for DXVK's Lock. USF_NO_RING=1 turns it off. */
#define RING_MIN (4u << 20)
uint32_t dxl_ring_discard(Wrap *w, const uint32_t *desc, uint32_t flags) {
    static int off = -1;
    if (off < 0) off = getenv("USF_NO_RING") != NULL;
    if (off || w->ring_on < 0 || (flags & (LOCK_DISCARD | LOCK_NOOVERWRITE | LOCK_READONLY)) != LOCK_DISCARD ||
        desc[4] < RING_MIN || !(desc[2] & 0x200u) || desc[3] != 0)
        return flags;
    void *dev = dxb_nat(device_guest);
    if (!w->ring_on) {
        w->ring[0] = w->nat;
        for (int k = 1; k < 4; k++) {
            void *b = NULL;
            int32_t r = (int32_t)dxb_method(dev, w->type == DXB_VB ? 26 : 27)(dev, desc[4], desc[2],
                w->type == DXB_VB ? desc[5] : desc[0], desc[3], (uint64_t)(uintptr_t)&b, 0, 0, 0, 0, 0, 0);
            if (r < 0 || !b) {
                for (int j = 1; j < k; j++) dxb_method(w->ring[j], 2)(w->ring[j], 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
                w->ring_on = -1;
                return flags;
            }
            w->ring[k] = b;
        }
        pthread_mutex_lock(&wlock);
        for (int k = 1; k < 4; k++) hput(w->ring[k], (uint32_t)(w - wraps));   /* GetStreamSource finds the wrapper */
        pthread_mutex_unlock(&wlock);
        w->ring_on = 1;
        w->ring_i = 0;
    }
    int nx = (w->ring_i + 1) & 3;
    if (g_dev.frames - w->ring_last[nx] < 5 && w->ring_last[nx]) return flags;
    w->ring_last[w->ring_i] = g_dev.frames ? g_dev.frames : 1;
    w->ring_i = nx;
    w->nat = w->ring[nx];
    for (int k = 0; k < 16; k++)
        if (stream_buf[k] == w->guest)
            dxb_method(dev, 100)(dev, (uint64_t)k, (uint64_t)(uintptr_t)w->nat, stream_off[k], stream_stride[k], 0, 0, 0, 0, 0, 0, 0);
    if (w->type == DXB_IB && cur_ib == w->guest) dxb_method(dev, 104)(dev, (uint64_t)(uintptr_t)w->nat, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    return (flags & ~LOCK_DISCARD) | LOCK_NOOVERWRITE;
}
/* the game's Release of a ringed buffer reached 0: the other ring buffers go too */
void dxl_ring_free(Wrap *w) {
    pthread_mutex_lock(&wlock);
    for (int k = 1; k < 4; k++) hdel(w->ring[k]);
    pthread_mutex_unlock(&wlock);
    for (int k = 1; k < 4; k++) dxb_method(w->ring[k], 2)(w->ring[k], 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    w->nat = w->ring[0];
    w->ring_on = 0;
}
/* buffers: one guest copy of the whole buffer; Lock(offset, size, ppbData, flags) */
__attribute__((noinline)) uint32_t dxl_lock_buffer(Wrap *w, uint32_t off, uint32_t size, uint32_t g_pp, uint32_t flags) {
    uint32_t desc[6];
    dxb_method(w->nat, 13)(w->nat, (uint64_t)(uintptr_t)desc, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    uint32_t total = desc[4];
    if (!w->shadow || w->shadow_size != total) { if (w->shadow) gfree(w->shadow); w->shadow = gcalloc(total + 16); w->shadow_size = total; }
    if (!size || off + size > total) size = off < total ? total - off : 0;
    void *p = NULL;
    double t_lock = usf_stalls > 0 ? now_ms() : 0;
    if (usf_stalls > 0 && (flags & LOCK_DISCARD)) { extern uint32_t usf_big_discards, usf_discards;
        usf_discards++; if (total >= (4u << 20)) usf_big_discards++; }
    w->gen++;                                            /* (index ranges remembered for it are stale) */
    uint32_t nflags = dxl_ring_discard(w, desc, flags);
    int32_t r = (int32_t)dxb_method(w->nat, 11)(w->nat, off, size, (uint64_t)(uintptr_t)&p, nflags, 0, 0, 0, 0, 0, 0, 0);
    if (usf_stalls > 0 && now_ms() - t_lock > 1.0)
        fprintf(stderr, "usf: slow buffer lock %.1f ms: type %d size %u usage %x pool %u lock %u+%u flags %x (wrap %p)\n",
                now_ms() - t_lock, w->type, total, desc[2], desc[3], off, size, flags, (void *)w);
    if (r < 0 || !p) { W32(g_pp, 0); return r < 0 ? (uint32_t)r : BAD; }
    if (!(flags & (LOCK_DISCARD | LOCK_NOOVERWRITE)) && !(desc[2] & 8u)) memcpy(HOST(w->shadow + off), p, size);
    /* The game sometimes writes past the range it locked (a 2 MB vertex buffer locked 32 KB at a time takes ~1 MB;
     * on Windows the pointer is into the whole buffer, so it works). A buffer locked rarely (under 64 times a second)
     * is checked: the bytes past the range get a marker, and Unlock writes back up to the last byte the game changed.
     * Buffers locked all the time (the dynamic rings) were measured never to do it and copy just their range. */
    uint32_t now = usf_ms();
    if (now - w->rate_t0 >= 1000) { w->checked = w->rate_n < 64; w->rate_n = 0; w->rate_t0 = now; }
    w->rate_n++;
    uint32_t mark = 0;
    if (w->checked && off + size < total) {
        mark = total - (off + size);
        memset(HOST(w->shadow + off + size), 0xA5, mark);
        checked_bytes += mark; checked_locks++;
    }
    /* Lazy write-back (dynamic buffers locked with DISCARD / NOOVERWRITE, not checked): Unlock copies nothing; the
     * written range is remembered and each draw copies just the vertices / indices it reads (dxb_flush_draw). A ring
     * the game discards ~100 times a frame to write a few vertices cost 50 MB of copies a frame when copied whole. */
    if (!mark && (desc[2] & 0x200u) && (flags & (LOCK_DISCARD | LOCK_NOOVERWRITE)) && !(flags & LOCK_READONLY)) {
        /* pending: from the lock to the end of the buffer (the game may write past the range it locked); each draw
         * copies only what it reads, so this costs nothing more */
        uint8_t *base = (uint8_t *)p - off;
        if ((flags & LOCK_DISCARD) || base != w->slice || w->dlo >= w->dhi) { w->slice = base; w->dlo = off; }
        else if (off < w->dlo) w->dlo = off;
        w->dhi = total;
        dxl_lock_push(w, 0x10000000u, w->shadow + off, 0, p);   /* (Unlock: nothing to copy) */
        W32(g_pp, w->shadow + off);
        return 0;
    }
    dxl_lock_push(w, (flags & LOCK_READONLY ? 0x80000000u : 0) | (mark ? 0x40000000u : 0), w->shadow + off, size, p);
    W32(g_pp, w->shadow + off);
    return 0;
}

/* copy the pending bytes of a buffer that a draw reads: [lo, hi) of the buffer */
void dxl_flush_range(Wrap *w, uint32_t lo, uint32_t hi) {
    if (!w || !w->slice || w->dlo >= w->dhi) return;
    if (lo < w->dlo) lo = w->dlo;
    if (hi > w->dhi) hi = w->dhi;
    if (lo >= hi) return;
    /* from the start of the pending bytes: a draw reading the middle would otherwise leave the interval whole, and
     * every later draw over it copy the same bytes again (bytes the game wrote for draws still to come) */
    lo = w->dlo;
    memcpy(w->slice + lo, HOST(w->shadow + lo), hi - lo);
    copied_buf += hi - lo;
    w->dlo = hi;
}
uint32_t dxl_index_size(Wrap *ib) {                     /* 2 or 4 (D3DFMT_INDEX32), asked once */
    if (!ib->isz) {
        uint32_t desc[6];
        dxb_method(ib->nat, 13)(ib->nat, (uint64_t)(uintptr_t)desc, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        ib->isz = desc[0] == 102 ? 4 : 2;
    }
    return ib->isz;
}
/* The smallest and largest index a draw reads. Remembered per (index buffer, first index, count) while the buffer
 * is not locked again (ib->gen): the static index buffers of the scenery are scanned once, not at every draw. */
#define IR_N 8192
static struct { Wrap *ib; uint32_t gen, start, n, lo, hi; } ir_cache[IR_N];
void dxl_index_range(Wrap *ib, uint32_t start, uint32_t n, uint32_t isz, uint32_t *lo, uint32_t *hi) {
    uint32_t h = (uint32_t)(((uintptr_t)ib >> 4) * 2654435761u ^ start * 40503u ^ n) & (IR_N - 1);
    if (ir_cache[h].ib == ib && ir_cache[h].gen == ib->gen && ir_cache[h].start == start && ir_cache[h].n == n) {
        *lo = ir_cache[h].lo; *hi = ir_cache[h].hi;
        return;
    }
    uint32_t mn = 0xFFFFFFFFu, mx = 0;
    const uint8_t *ip = HOST(ib->shadow + start * isz);
    if (isz == 2) {
        const uint16_t *q = (const uint16_t *)ip;
        for (uint32_t k = 0; k < n; k++) { uint32_t x = q[k]; mn = x < mn ? x : mn; mx = x > mx ? x : mx; }
    } else {
        const uint32_t *q = (const uint32_t *)ip;
        for (uint32_t k = 0; k < n; k++) { uint32_t x = q[k]; mn = x < mn ? x : mn; mx = x > mx ? x : mx; }
    }
    ir_cache[h].ib = ib; ir_cache[h].gen = ib->gen; ir_cache[h].start = start; ir_cache[h].n = n;
    ir_cache[h].lo = *lo = mn; ir_cache[h].hi = *hi = mx;
}
uint32_t dxl_vcount(uint32_t type, uint32_t prims) {
    switch (type) { case 1: return prims; case 2: return prims * 2; case 3: return prims + 1; case 4: return prims * 3;
                    default: return prims + 2; }
}
/* before DrawPrimitive (indexed = 0: start vertex, count) / DrawIndexedPrimitive (base, min, num, start index) */
void dxl_flush_draw(int indexed, uint32_t type, int32_t base, uint32_t first, uint32_t num, uint32_t start_index,
                       uint32_t prims) {
    /* Indexed draws: the vertex range comes from the indices themselves. The game's MinVertexIndex / NumVertices are
     * not always right (Direct3D 9 does not check them), and copying by them left vertices out: stretched triangles. */
    int have_minmax = 0;
    uint32_t imin = 0, imax = 0;
    for (int sidx = 0; sidx < 16; sidx++) {
        Wrap *v = dxb_of(stream_buf[sidx]);
        if (!v || !v->slice || v->dlo >= v->dhi) continue;
        if (stream_freq[sidx] != 1 && stream_freq[sidx] != 0) { dxl_flush_range(v, 0, v->shadow_size); continue; }
        if (indexed && !have_minmax) {
            have_minmax = -1;
            Wrap *ib = dxb_of(cur_ib);
            if (ib && ib->shadow) {
                uint32_t isz = dxl_index_size(ib), n = dxl_vcount(type, prims);
                if ((uint64_t)(start_index + n) * isz <= ib->shadow_size) {
                    if (n) { dxl_index_range(ib, start_index, n, isz, &imin, &imax); have_minmax = 1; }
                }
            }
        }
        if (indexed && have_minmax == 1) { first = imin; num = imax - imin + 1; }
        int64_t v0 = indexed ? (int64_t)base + first : first, nv = indexed ? num : dxl_vcount(type, prims);
        int64_t lo = stream_off[sidx] + v0 * stream_stride[sidx], hi = lo + nv * stream_stride[sidx];
        if (lo < 0) lo = 0;
        if (hi > lo) dxl_flush_range(v, (uint32_t)lo, (uint32_t)hi);
    }
    if (indexed) {
        Wrap *ib = dxb_of(cur_ib);
        if (ib && ib->slice && ib->dlo < ib->dhi) {
            uint32_t isz = dxl_index_size(ib), lo = start_index * isz;
            dxl_flush_range(ib, lo, lo + dxl_vcount(type, prims) * isz);
        }
    }
}
__attribute__((noinline)) uint32_t dxl_unlock_buffer(Wrap *w) {
    Lock *l = w->locks;
    if (l && (l->key & 0x10000000u)) { w->locks = l->next; free(l); l = NULL; }
    if (l) {
        w->locks = l->next;
        uint32_t n = l->size;
        if (l->key & 0x40000000u) {                             /* checked: up to the last byte written past it */
            const uint8_t *g = HOST(l->guest), *top = HOST(w->shadow + w->shadow_size);
            const uint8_t *e = top;
            static const uint8_t a5[64] = {[0 ... 63] = 0xA5};
            while (e - 64 >= g + n && !memcmp(e - 64, a5, 64)) e -= 64;   /* 64 bytes at a time (SIMD) */
            while (e > g + n && e[-1] == 0xA5) e--;
            n = (uint32_t)(e - g);
        }
        if (!(l->key & 0x80000000u)) { memcpy(l->host, HOST(l->guest), n); copied_buf += n; }
        free(l);
    }
    return (uint32_t)dxb_method(w->nat, 12)(w->nat, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}
