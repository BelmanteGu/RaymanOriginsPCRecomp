/* The DXVK bridge (see dxvk_bridge.h): wrappers, the generic call through a method's signature, and the methods
 * that need more than a signature.
 *
 * What it does: loads libdxvk_d3d9.so (USF_DXVK_LIB, else the build in private/deps/dxvk) with DXVK_WSI_DRIVER=SDL3
 * (a window handle is the SDL_Window the runtime made); Direct3DCreate9 returns a wrapper of DXVK's IDirect3D9.
 * Special methods: QueryInterface/AddRef/Release (a wrapper lives as long as DXVK's object has references),
 * CreateDevice/Reset/CreateAdditionalSwapChain/GetPresentParameters (D3DPRESENT_PARAMETERS has a window handle, so
 * its 64-bit layout differs), GetCreationParameters, Present (frame count, statistics, captures), Clear and
 * SetNPatchMode (float arguments), GetContainer, and every Lock/Unlock (the game gets a copy in guest memory).
 * What it does not do: GetDC on surfaces (refused), software vertex processing beyond what DXVK does.
 */
#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "d3d9.h"
#include "dxvk_bridge.h"
#include "usf/x87.h"

#define BAD 0x8876086Cu                                 /* D3DERR_INVALIDCALL */
#define HWND_MAIN 0x00010010u
extern SDL_Window *g_window;
extern char g_repo_dir[1024];
void apply_aspect(uint32_t w, uint32_t h);
void game_pace(void);

static uint32_t gpu_epoch = 1;
static uint64_t copied_buf, copied_tex_in, copied_tex_out;   /* bytes copied by locks (statistics) */
static uint32_t stream_buf[16], stream_off[16], stream_stride[16], stream_freq[16], cur_ib;   /* bound for draws */
static void flush_range(Wrap *w, uint32_t lo, uint32_t hi);
static void flush_draw(int indexed, uint32_t type, int32_t base, uint32_t first, uint32_t num, uint32_t start_index,
                       uint32_t prims);                          /* +1 at UpdateSurface/UpdateTexture/StretchRect/... */

/* ---- loading ---- */
static int state;                                       /* 0 not tried, 1 loaded, -1 not available */
static void *(*nat_create9)(uint32_t);
int dxb_enabled(void) {
    if (state) return state > 0;
    state = -1;
    const char *e = getenv("USF_DXVK");
    if (e && !strcmp(e, "0")) return 0;
    char path[1200];
    if (getenv("USF_DXVK_LIB")) snprintf(path, sizeof path, "%s", getenv("USF_DXVK_LIB"));
#ifdef __ANDROID__
    else snprintf(path, sizeof path, "libdxvk_d3d9.so");   /* in the app's library folder, next to libmain.so */
#else
    else snprintf(path, sizeof path, "%s/private/deps/dxvk/build-native/src/d3d9/libdxvk_d3d9.so.0", g_repo_dir);
#endif
    setenv("DXVK_WSI_DRIVER", "SDL3", 0);
    /* the game asks for immediate presentation and runs as fast as it can (uneven 40-90 fps on a fast GPU). Vsync is
     * no cure on a 144 Hz panel (60 does not divide 144: frames alternate 14 and 21 ms), so DXVK's frame limiter
     * paces the frames to USF_FPS (default 60; 0 = uncapped), presenting without waiting for the vblank (on Wayland
     * the compositor still shows whole frames). USF_NOVSYNC: uncapped (tests); USF_VSYNC=1: FIFO vsync as well. */
    static char cfg[160];
    int fps = 0;                       /* Rayman: game_pace() paces (60/120 live from the settings), not DXVK */
    /* cachedWriteOnlyBuffers: the dynamic buffers in cached system memory, not video memory mapped over PCIe. The
     * bridge copies the game's vertices into them at each draw (3 MB a frame in a race), and those writes ran at
     * about 1 GB/s into video memory (13% of the game thread); the GPU reading 3 MB a frame from RAM costs nothing. */
    snprintf(cfg, sizeof cfg, "d3d9.cachedWriteOnlyBuffers = True; d3d9.presentInterval = %d; d3d9.maxFrameRate = %d",
             getenv("USF_VSYNC") && !getenv("USF_NOVSYNC") ? 1 : 0, fps > 0 ? fps : 0);
    setenv("DXVK_CONFIG", cfg, 0);
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "usf: DXVK not loaded (%s): own renderer\n", dlerror()); return 0; }
    nat_create9 = (void *(*)(uint32_t))dlsym(h, "Direct3DCreate9");
    if (!nat_create9) return 0;
    fprintf(stderr, "usf: Direct3D 9 through DXVK native (%s)\n", path);
    state = 1;
    return 1;
}

/* ---- wrappers ---- */
#define MAXW (1u << 18)
#define HASH (1u << 20)
static Wrap *wraps;
static uint32_t nwraps = 1, freelist;
static void **hkeys;                                    /* DXVK object -> wrapper index (open addressing) */
static uint32_t *hvals;
static uint32_t vtables[DXB_NTYPES];
static pthread_mutex_t wlock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t device_guest;
uint32_t dxb_device_guest(void) { return device_guest; }

static uint32_t hslot(void *p) { return (uint32_t)(((uintptr_t)p >> 4) * 2654435761u) & (HASH - 1); }
static uint32_t hfind(void *p) {
    for (uint32_t i = hslot(p);; i = (i + 1) & (HASH - 1)) {
        if (!hkeys[i]) return 0;
        if (hkeys[i] == p) return hvals[i];
    }
}
static void hput(void *p, uint32_t v) {
    uint32_t i = hslot(p);
    while (hkeys[i] && hkeys[i] != p && hkeys[i] != (void *)1) i = (i + 1) & (HASH - 1);
    hkeys[i] = p; hvals[i] = v;
}
static void hdel(void *p) {                              /* tombstone (1) keeps later probes going */
    for (uint32_t i = hslot(p); hkeys[i]; i = (i + 1) & (HASH - 1))
        if (hkeys[i] == p) { hkeys[i] = (void *)1; return; }
}

Wrap *dxb_of(uint32_t g) {
    if (!g || g < 0x10000u) return NULL;
    uint32_t i = R32(g + 8);
    if (!i || i >= nwraps || wraps[i].guest != g) return NULL;
    return &wraps[i];
}
void *dxb_nat(uint32_t g) { Wrap *w = dxb_of(g); return w ? w->nat : NULL; }

static uint32_t vtable_of(int type);
uint32_t dxb_wrap(void *nat, int type) {
    if (!nat) return 0;
    pthread_mutex_lock(&wlock);
    if (!wraps) {
        wraps = calloc(MAXW, sizeof *wraps);
        hkeys = calloc(HASH, sizeof *hkeys);
        hvals = calloc(HASH, sizeof *hvals);
    }
    uint32_t i = hfind(nat);
    if (i) { pthread_mutex_unlock(&wlock); return wraps[i].guest; }
    if (freelist) { i = freelist; freelist = (uint32_t)(uintptr_t)wraps[i].nat; }
    else if (nwraps < MAXW) i = nwraps++;
    else hle_fatal("DXVK bridge: too many Direct3D objects");
    Wrap *w = &wraps[i];
    memset(w, 0, sizeof *w);
    w->nat = nat; w->type = type;
    w->guest = gcalloc(16);
    W32(w->guest, vtable_of(type));
    W32(w->guest + 8, i);
    hput(nat, i);
    pthread_mutex_unlock(&wlock);
    return w->guest;
}
static void unwrap(Wrap *w) {
    pthread_mutex_lock(&wlock);
    uint32_t i = (uint32_t)(w - wraps);
    hdel(w->nat);
    if (w->shadow) gfree(w->shadow);
    if (w->tsh) { for (int k = 0; k < 6 * 16; k++) if (w->tsh[k].guest) gfree(w->tsh[k].guest); free(w->tsh); }
    for (Lock *l = w->locks, *n; l; l = n) { n = l->next; gfree(l->guest); free(l); }
    W32(w->guest + 8, 0);
    gfree(w->guest);
    memset(w, 0, sizeof *w);
    w->nat = (void *)(uintptr_t)freelist;
    freelist = i;
    pthread_mutex_unlock(&wlock);
}

/* the kind of a texture / resource DXVK returned (IDirect3DResource9::GetType) */
static int type_of_resource(void *nat) {
    switch ((uint32_t)dxb_method(nat, 10)(nat, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)) {
    case 1: return DXB_SURFACE; case 2: return DXB_VOLUME; case 3: return DXB_TEXTURE; case 4: return DXB_VOLTEX;
    case 5: return DXB_CUBE; case 6: return DXB_VB; case 7: return DXB_IB; default: return DXB_TEXTURE;
    }
}
static int out_type(char k, void *nat) {
    switch (k) {
    case 'D': return DXB_DEVICE; case '9': return DXB_D3D9; case 'S': return DXB_SURFACE; case 'V': return DXB_VOLUME;
    case 'B': return DXB_VB; case 'I': return DXB_IB; case 'v': return DXB_VS; case 'x': return DXB_PS;
    case 'd': return DXB_DECL; case 'q': return DXB_QUERY; case 'k': return DXB_STATEBLOCK; case 'w': return DXB_SWAPCHAIN;
    default: return type_of_resource(nat);              /* T, U */
    }
}

static const char *const *sigs(int type, int *n) {
    switch (type) {
    case DXB_D3D9: *n = 17; return dxb_sig_d3d9;
    case DXB_DEVICE: *n = 119; return dxb_sig_device;
    case DXB_TEXTURE: *n = 22; return dxb_sig_texture;
    case DXB_CUBE: *n = 22; return dxb_sig_cube;
    case DXB_VOLTEX: *n = 22; return dxb_sig_voltex;
    case DXB_SURFACE: *n = 17; return dxb_sig_surface;
    case DXB_VOLUME: *n = 11; return dxb_sig_volume;
    case DXB_VB: case DXB_IB: *n = 14; return dxb_sig_buffer;
    case DXB_VS: case DXB_PS: case DXB_DECL: *n = 5; return dxb_sig_shader;
    case DXB_QUERY: *n = 8; return dxb_sig_query;
    case DXB_STATEBLOCK: *n = 6; return dxb_sig_stateblock;
    default: *n = 10; return dxb_sig_swapchain;
    }
}

/* ---- the call ---- */
static uint32_t special(CPU *c, uint32_t a, Wrap *w, int slot);
static uint32_t dispatch(CPU *c, uint32_t a, int slot) {
    Wrap *w = dxb_of(ARG(0));
    if (!w) return BAD;
    int n;
    const char *s = sigs(w->type, &n)[slot];
    if (s[0] == '*') return special(c, a, w, slot);
    uint64_t x[11] = {0};
    void *outs[2] = {0};
    uint32_t out_at[2] = {0};
    char out_kind[2] = {0};
    int nout = 0;
    for (int i = 0; s[i] && i < 11; i++) {
        uint32_t v = ARG(i + 1);
        switch (s[i]) {
        case 'u': x[i] = v; break;
        case 'i': x[i] = (uint64_t)(int64_t)(int32_t)v; break;
        case 'p': x[i] = v ? (uint64_t)(uintptr_t)HOST(v) : 0; break;
        case 'o': x[i] = (uint64_t)(uintptr_t)dxb_nat(v); break;
        case 'h': x[i] = (uint64_t)(uintptr_t)(v ? g_window : NULL); break;
        case 'n': x[i] = 0; break;
        default:                                         /* an out interface */
            if (nout < 2) {
                out_at[nout] = v; out_kind[nout] = s[i];
                x[i] = v ? (uint64_t)(uintptr_t)&outs[nout] : 0;
                nout++;
            }
            break;
        }
    }
    if (w->type == DXB_DEVICE) switch (slot) {
    case 100: if (ARG(1) < 16) { stream_buf[ARG(1)] = ARG(2); stream_off[ARG(1)] = ARG(3); stream_stride[ARG(1)] = ARG(4); } break;
    case 102: if (ARG(1) < 16) stream_freq[ARG(1)] = ARG(2); break;
    case 104: cur_ib = ARG(1); break;
    case 81: flush_draw(0, ARG(1), 0, ARG(2), 0, 0, ARG(3)); break;
    case 82: flush_draw(1, ARG(1), (int32_t)ARG(2), ARG(3), ARG(4), ARG(5), ARG(6)); break;
    case 85: for (int k = 0; k < 16; k++) { Wrap *v = dxb_of(stream_buf[k]); if (v) flush_range(v, 0, v->shadow_size); } break;
    }
    if (w->type == DXB_DEVICE && getenv("USF_DXB_RTLOG")) {   /* (analysis) render targets the game makes */
        if (slot == 23 && (ARG(4) & 3u))
            fprintf(stderr, "dxb rt texture %ux%u levels %u usage %x fmt %u pool %u\n", ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6));
        if (slot == 25 && (ARG(3) & 3u))
            fprintf(stderr, "dxb rt cube %u levels %u usage %x fmt %u\n", ARG(1), ARG(2), ARG(3), ARG(4));
        if (slot == 18 || slot == 38) fprintf(stderr, "dxb %s (frame %u)\n", slot == 18 ? "GetBackBuffer" : "GetRenderTarget", g_dev.frames);
        if (slot == 37) {
            Wrap *sw = dxb_of(ARG(2));
            uint32_t d8[8] = {0};
            if (sw && sw->nat) dxb_method(sw->nat, 12)(sw->nat, (uint64_t)(uintptr_t)d8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
            static uint32_t last_w, last_h;
            if (d8[6] != last_w || d8[7] != last_h) { fprintf(stderr, "dxb SetRenderTarget(%u) %ux%u (frame %u)\n", ARG(1), d8[6], d8[7], g_dev.frames); last_w = d8[6]; last_h = d8[7]; }
        }
        if (slot == 28 || slot == 29)
            fprintf(stderr, "dxb %s %ux%u fmt %u ms %u q %u\n", slot == 28 ? "rt surface" : "depth surface", ARG(1), ARG(2), ARG(3), ARG(4), ARG(5));
    }
    if (w->type == DXB_DEVICE && slot >= 81 && slot <= 84) g_dev.draws++;
    if (w->type == DXB_DEVICE && slot >= 30 && slot <= 35) gpu_epoch++;   /* the GPU may write a texture level */
    if (w->type == DXB_D3D9 && slot == 5 && getenv("USF_DXB_IDLOG")) {   /* (analysis) the adapter's GUID */
        uint32_t r5 = (uint32_t)dxb_method(w->nat, 5)(w->nat, x[0], x[1], x[2], 0, 0, 0, 0, 0, 0, 0, 0);
        uint32_t g = ARG(3) + 1056 + 8 + 4 * 4;                /* after Driver, Description, DeviceName, version, ids */
        fprintf(stderr, "dxb adapter %u '%s' guid %08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\n", ARG(1),
                STR(ARG(3) + 512), R32(g), R16(g + 4), R16(g + 6), R8(g + 8), R8(g + 9), R8(g + 10), R8(g + 11),
                R8(g + 12), R8(g + 13), R8(g + 14), R8(g + 15));
        return r5;
    }
    uint32_t r = (uint32_t)dxb_method(w->nat, slot)(w->nat, x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7], x[8],
                                                    x[9], x[10]);
    if (w->type == DXB_DEVICE && (int32_t)r < 0 && getenv("USF_DXB_FAILLOG")) {   /* (analysis) failed calls */
        static int nfail;
        if (nfail++ < 40) {
            fprintf(stderr, "dxb device slot %d failed %08x args", slot, r);
            for (int i = 1; i <= 5; i++) fprintf(stderr, " %08x", ARG(i));
            if (slot == 34) for (int k = 2; k <= 4; k += 2) if (ARG(k)) fprintf(stderr, " rect%d %d,%d,%d,%d", k, (int32_t)R32(ARG(k)), (int32_t)R32(ARG(k) + 4), (int32_t)R32(ARG(k) + 8), (int32_t)R32(ARG(k) + 12));
            fprintf(stderr, "\n");
        }
    }
    for (int k = 0; k < nout; k++)
        if (out_at[k]) W32(out_at[k], outs[k] ? dxb_wrap(outs[k], out_type(out_kind[k], outs[k])) : 0);
    return r;
}

/* one thunk target per slot; the wrapper's type picks the signature */
/* USF_STALLS: time of each DXVK call on the calling thread, reported for the frames that come late */
int usf_stalls = -1;
__thread double usf_block_ms[3];                         /* this thread: Wait*, Sleep, critical sections (ms) */
static __thread float call_ms[DXB_NTYPES][120];
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static uint32_t timed(CPU *c, uint32_t a, int n) {
    if (!usf_stalls) return dispatch(c, a, n);
    Wrap *w = dxb_of(ARG(0));                            /* before the call: Release may free it */
    int ty = w ? w->type : 0;
    double t = now_ms();
    uint32_t r = dispatch(c, a, n);
    call_ms[ty][n] += (float)(now_ms() - t);
    return r;
}
#define S1(n) static uint32_t s##n(CPU *c, uint32_t a) { return timed(c, a, n); }
#define S10(n) S1(n##0) S1(n##1) S1(n##2) S1(n##3) S1(n##4) S1(n##5) S1(n##6) S1(n##7) S1(n##8) S1(n##9)
S1(0) S1(1) S1(2) S1(3) S1(4) S1(5) S1(6) S1(7) S1(8) S1(9)
S10(1) S10(2) S10(3) S10(4) S10(5) S10(6) S10(7) S10(8) S10(9) S10(10) S10(11)
#define L10(n) s##n##0, s##n##1, s##n##2, s##n##3, s##n##4, s##n##5, s##n##6, s##n##7, s##n##8, s##n##9
static HleFn slot_fn[120] = {s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, L10(1), L10(2), L10(3), L10(4), L10(5),
                             L10(6), L10(7), L10(8), L10(9), L10(10), L10(11)};

static uint32_t vtable_of(int type) {
    if (vtables[type]) return vtables[type];
    static const char *names[DXB_NTYPES] = {"", "IDirect3D9 (DXVK)", "IDirect3DDevice9 (DXVK)", "IDirect3DTexture9 (DXVK)",
        "IDirect3DCubeTexture9 (DXVK)", "IDirect3DVolumeTexture9 (DXVK)", "IDirect3DSurface9 (DXVK)",
        "IDirect3DVolume9 (DXVK)", "IDirect3DVertexBuffer9 (DXVK)", "IDirect3DIndexBuffer9 (DXVK)",
        "IDirect3DVertexShader9 (DXVK)", "IDirect3DPixelShader9 (DXVK)", "IDirect3DVertexDeclaration9 (DXVK)",
        "IDirect3DQuery9 (DXVK)", "IDirect3DStateBlock9 (DXVK)", "IDirect3DSwapChain9 (DXVK)"};
    int n;
    const char *const *s = sigs(type, &n);
    uint8_t na[120];
    for (int i = 0; i < n; i++) na[i] = (uint8_t)(1 + strlen(s[i][0] == '*' ? s[i] + 1 : s[i]));
    vtables[type] = R32(obj_new_n(names[type], n, slot_fn, na, 16));
    return vtables[type];
}

/* ---- D3DPRESENT_PARAMETERS: guest (32-bit, 56 bytes) <-> DXVK (64-bit: the window handle is 8 bytes, at 32) ---- */
static void pp_in(uint32_t g, uint8_t *n) {
    memset(n, 0, 64);
    memcpy(n, HOST(g), 28);                              /* width .. swap effect */
    void *wnd = R32(g + 28) ? (void *)g_window : NULL;
    memcpy(n + 32, &wnd, 8);
    memcpy(n + 40, HOST(g + 32), 24);                    /* windowed .. presentation interval */
}
static void pp_out(const uint8_t *n, uint32_t g) {
    memcpy(HOST(g), n, 28);
    W32(g + 28, HWND_MAIN);
    memcpy(HOST(g + 32), n + 40, 24);
}

/* ---- Present: frame count, statistics (the same line as the own renderer), captures ---- */
uint32_t usf_ms(void);
uint32_t usf_big_discards, usf_discards;
static uint64_t checked_bytes, checked_locks;            /* checked buffer locks: bytes marked past the range */                 /* USF_STALLS: DISCARD locks (of buffers of 4 MB or more) */
static void shot_if_wanted(void *dev, void *chain);
static int cmp_float(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }
static double ft_p99(float *v, uint32_t n) {             /* 99th percentile frame time (sorts v) */
    if (!n) return 0;
    qsort(v, n, sizeof *v, cmp_float);
    return v[n * 99 / 100 < n ? n * 99 / 100 : n - 1];
}
static uint32_t ft_late(const float *v, uint32_t n) {    /* frames 25% over the target (18 ms at 60 fps) */
    int fps = getenv("USF_FPS") ? atoi(getenv("USF_FPS")) : 60;
    double lim = fps > 0 && !getenv("USF_NOVSYNC") ? 1250.0 / fps : 20.8;
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) k += v[i] > lim;
    return k;
}
static void after_present(void *dev, void *chain) {   /* chain: an additional swap chain presented, or NULL */
    g_dev.frames++;
    static struct timespec t0, c0;
    static uint32_t f0, d0;
    static double worst;
    static struct timespec last;
    static float fts[4096];                              /* frame times of the current 2 s window */
    static uint32_t nft;
    static FILE *ftlog;                                  /* USF_FTLOG: every frame time, one per line (ms) */
    struct timespec t, ct;
    clock_gettime(CLOCK_MONOTONIC, &t);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ct);
    if (last.tv_sec) {
        double ft = (double)(t.tv_sec - last.tv_sec) * 1e3 + (double)(t.tv_nsec - last.tv_nsec) / 1e6;
        if (ft > worst) worst = ft;
        if (nft < 4096) fts[nft++] = (float)ft;
        if (!ftlog && getenv("USF_FTLOG")) ftlog = fopen(getenv("USF_FTLOG"), "w");
        if (ftlog) fprintf(ftlog, "%u %.3f\n", usf_ms(), ft);
        static struct timespec lc;
        if (usf_stalls < 0) usf_stalls = getenv("USF_STALLS") != NULL;
        if (usf_stalls && ft > 20.8) {
            double cpu = (double)(ct.tv_sec - lc.tv_sec) * 1e3 + (double)(ct.tv_nsec - lc.tv_nsec) / 1e6;
            char top[256] = "";
            for (int k = 0; k < 3; k++) {
                int bt = 0, bs = 0;
                for (int ty = 0; ty < DXB_NTYPES; ty++)
                    for (int sl = 0; sl < 120; sl++) if (call_ms[ty][sl] > call_ms[bt][bs]) { bt = ty; bs = sl; }
                if (call_ms[bt][bs] < 0.5f) break;
                snprintf(top + strlen(top), sizeof top - strlen(top), " t%d.%d=%.1f", bt, bs, call_ms[bt][bs]);
                call_ms[bt][bs] = -call_ms[bt][bs];
            }
            fprintf(stderr, "usf: late frame at %u ms: %.1f ms, cpu %.1f, wait %.1f, sleep %.1f, cs %.1f, dxvk%s\n",
                    usf_ms(), ft, cpu, usf_block_ms[0], usf_block_ms[1], usf_block_ms[2], top);
        }
        lc = ct;
    }
    if (usf_stalls > 0) { memset(call_ms, 0, sizeof call_ms); memset(usf_block_ms, 0, sizeof usf_block_ms); }
    last = t;
    if (!t0.tv_sec) { t0 = t; c0 = ct; f0 = g_dev.frames; d0 = g_dev.draws; }
    double el = (double)(t.tv_sec - t0.tv_sec) * 1e3 + (double)(t.tv_nsec - t0.tv_nsec) / 1e6;
    if (el >= 2000.0) {
        uint32_t nf = g_dev.frames - f0 ? g_dev.frames - f0 : 1;
        double cpu = (double)(ct.tv_sec - c0.tv_sec) * 1e3 + (double)(ct.tv_nsec - c0.tv_nsec) / 1e6;
        fprintf(stderr, "usf: %.1f fps, %u draws/frame, ring 0 KB | cpu %.1f ms/f, draws 0.00 ms/f, gpu wait 0.0 ms/f, "
                "worst frame %.1f ms, p99 %.1f ms, late %u | dxvk, copies/frame: buffers %llu KB, textures in %llu KB out %llu KB\n",
                nf * 1000.0 / el, (g_dev.draws - d0) / nf, cpu / nf, worst, ft_p99(fts, nft), ft_late(fts, nft), (unsigned long long)(copied_buf / nf >> 10),
                (unsigned long long)(copied_tex_in / nf >> 10), (unsigned long long)(copied_tex_out / nf >> 10));
        if (usf_stalls > 0) {
            extern uint64_t usf_mem_bytes[3], usf_mem_calls[3];
            fprintf(stderr, "usf: discards/frame %.1f, of big buffers %.1f; game memcpy %.0f KB in %.0f calls, memmove %.0f KB "
                    "in %.0f, memset %.0f KB in %.0f (per frame, all threads)\n", usf_discards / (double)nf,
                    usf_big_discards / (double)nf, usf_mem_bytes[0] / 1024.0 / nf, usf_mem_calls[0] / (double)nf,
                    usf_mem_bytes[1] / 1024.0 / nf, usf_mem_calls[1] / (double)nf, usf_mem_bytes[2] / 1024.0 / nf,
                    usf_mem_calls[2] / (double)nf);
            fprintf(stderr, "usf: checked locks/frame %.1f, marked %.0f KB/frame\n", checked_locks / (double)nf, checked_bytes / 1024.0 / nf);
            checked_bytes = checked_locks = 0;
            memset(usf_mem_bytes, 0, sizeof usf_mem_bytes); memset(usf_mem_calls, 0, sizeof usf_mem_calls);
        }
        usf_discards = usf_big_discards = 0;
        copied_buf = copied_tex_in = copied_tex_out = 0;
        t0 = t; c0 = ct; f0 = g_dev.frames; d0 = g_dev.draws; worst = 0; nft = 0;
    }
    shot_if_wanted(dev, chain);
    void usf_perf_frame(void);
    usf_perf_frame();
}

/* USF_SHOT / USF_SHOT_MS / USF_SHOT_DIR as with the own renderer: the back buffer as PPM */
static int want_shot(void) {
    const char *lm = getenv("USF_SHOT_MS");
    if (lm) {
        static uint32_t done;
        uint32_t now = usf_ms(), k = 0;
        for (const char *p = lm; *p; k++) {
            if (k == done && now >= (uint32_t)strtoul(p, NULL, 10)) { done++; return 1; }
            p = strchr(p, ',');
            if (!p) break;
            p++;
        }
        return 0;
    }
    const char *l = getenv("USF_SHOT");
    for (const char *p = l; p && *p;) {
        if ((uint32_t)strtoul(p, NULL, 10) == g_dev.frames) return 1;
        p = strchr(p, ',');
        if (p) p++;
    }
    return 0;
}
static void shot_if_wanted(void *dev, void *chain) {
    if (!want_shot()) return;
    void *bb = NULL, *sys = NULL;
    /* the back buffer of what was presented: the device's implicit swap chain, or the additional one */
    if (chain ? (int32_t)dxb_method(chain, 5)(chain, 0, 0, (uint64_t)(uintptr_t)&bb, 0, 0, 0, 0, 0, 0, 0, 0) < 0
              : (int32_t)dxb_method(dev, 18)(dev, 0, 0, 0, (uint64_t)(uintptr_t)&bb, 0, 0, 0, 0, 0, 0, 0) < 0 || !bb)
        return;
    if (!bb) return;
    uint32_t desc[8];
    dxb_method(bb, 12)(bb, (uint64_t)(uintptr_t)desc, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    uint32_t w = desc[6], h = desc[7];
    if ((int32_t)dxb_method(dev, 36)(dev, w, h, desc[0], 2, (uint64_t)(uintptr_t)&sys, 0, 0, 0, 0, 0, 0) >= 0 && sys &&
        (int32_t)dxb_method(dev, 32)(dev, (uint64_t)(uintptr_t)bb, (uint64_t)(uintptr_t)sys, 0, 0, 0, 0, 0, 0, 0, 0, 0) >= 0) {
        struct { int32_t pitch; int32_t pad; uint8_t *bits; } lr;
        if ((int32_t)dxb_method(sys, 13)(sys, (uint64_t)(uintptr_t)&lr, 0, 0x10, 0, 0, 0, 0, 0, 0, 0, 0) >= 0) {
            char fn[1200];
            const char *dir = getenv("USF_SHOT_DIR");
            snprintf(fn, sizeof fn, "%s/frame_%05u.ppm", dir ? dir : ".", g_dev.frames);
            FILE *f = fopen(fn, "wb");
            if (f) {
                fprintf(f, "P6\n%u %u\n255\n", w, h);
                uint8_t *row = malloc(3u * w);
                for (uint32_t y = 0; y < h; y++) {
                    const uint8_t *s = lr.bits + (size_t)y * (uint32_t)lr.pitch;
                    for (uint32_t x = 0; x < w; x++) { row[3 * x] = s[4 * x + 2]; row[3 * x + 1] = s[4 * x + 1]; row[3 * x + 2] = s[4 * x]; }
                    fwrite(row, 3, w, f);
                }
                free(row);
                fclose(f);
            }
            dxb_method(sys, 14)(sys, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        }
    }
    if (sys) dxb_method(sys, 2)(sys, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    dxb_method(bb, 2)(bb, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

/* ---- locks: the game writes into a guest copy; Unlock puts it into DXVK's memory ---- */
#define LOCK_READONLY 0x10u
#define LOCK_NOOVERWRITE 0x1000u
#define LOCK_DISCARD 0x2000u
static int block_format(uint32_t f) { return f == 0x31545844u || f == 0x32545844u || f == 0x33545844u || f == 0x34545844u || f == 0x35545844u || f == 0x31495441u || f == 0x32495441u; }
static void lock_push(Wrap *w, uint32_t key, uint32_t guest, uint32_t size, void *host) {
    Lock *l = malloc(sizeof *l);
    *l = (Lock){key, guest, size, host, w->locks};
    w->locks = l;
}
static Lock *lock_pop(Wrap *w, uint32_t key) {
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
static uint32_t lock_rect(Wrap *w, int slot, uint64_t face, uint64_t level, uint32_t g_lr, uint32_t g_rect,
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
    int blk = block_format(desc[0]);
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
    lock_push(w, key | 0x20000000u | (flags & LOCK_READONLY ? 0x80000000u : 0), ts->guest + y0 * pitch,
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
static int lock_readback(Wrap *w, uint32_t g_lr, uint32_t g_rect, uint32_t flags, const uint32_t *desc) {
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
static uint32_t unlock(Wrap *w, int slot, uint32_t key, uint64_t a1, uint64_t a2) {
    Lock *l = lock_pop(w, key);
    if (l) {
        if (!(l->key & 0x80000000u)) { memcpy(l->host, HOST(l->guest), l->size); copied_tex_out += l->size; }
        if (!(l->key & 0x20000000u)) gfree(l->guest);       /* (texture levels keep their copy) */
        free(l);
    }
    return (uint32_t)dxb_method(w->nat, slot)(w->nat, a1, a2, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}
static uint32_t lock_box(Wrap *w, int slot, uint64_t level, uint32_t g_lb, uint32_t g_box, uint32_t flags,
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
    lock_push(w, key | (flags & LOCK_READONLY ? 0x80000000u : 0), g, size, lb.bits);
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
static uint32_t ring_discard(Wrap *w, const uint32_t *desc, uint32_t flags) {
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
static void ring_free(Wrap *w) {
    pthread_mutex_lock(&wlock);
    for (int k = 1; k < 4; k++) hdel(w->ring[k]);
    pthread_mutex_unlock(&wlock);
    for (int k = 1; k < 4; k++) dxb_method(w->ring[k], 2)(w->ring[k], 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    w->nat = w->ring[0];
    w->ring_on = 0;
}
/* buffers: one guest copy of the whole buffer; Lock(offset, size, ppbData, flags) */
__attribute__((noinline)) static uint32_t lock_buffer(Wrap *w, uint32_t off, uint32_t size, uint32_t g_pp, uint32_t flags) {
    uint32_t desc[6];
    dxb_method(w->nat, 13)(w->nat, (uint64_t)(uintptr_t)desc, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    uint32_t total = desc[4];
    if (!w->shadow || w->shadow_size != total) { if (w->shadow) gfree(w->shadow); w->shadow = gcalloc(total + 16); w->shadow_size = total; }
    if (!size || off + size > total) size = off < total ? total - off : 0;
    void *p = NULL;
    double t_lock = usf_stalls > 0 ? now_ms() : 0;
    if (usf_stalls > 0 && (flags & LOCK_DISCARD)) { extern uint32_t usf_big_discards, usf_discards;
static uint64_t checked_bytes, checked_locks;            /* checked buffer locks: bytes marked past the range */ usf_discards++; if (total >= (4u << 20)) usf_big_discards++; }
    w->gen++;                                            /* (index ranges remembered for it are stale) */
    uint32_t nflags = ring_discard(w, desc, flags);
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
        lock_push(w, 0x10000000u, w->shadow + off, 0, p);   /* (Unlock: nothing to copy) */
        W32(g_pp, w->shadow + off);
        return 0;
    }
    lock_push(w, (flags & LOCK_READONLY ? 0x80000000u : 0) | (mark ? 0x40000000u : 0), w->shadow + off, size, p);
    W32(g_pp, w->shadow + off);
    return 0;
}

/* copy the pending bytes of a buffer that a draw reads: [lo, hi) of the buffer */
static void flush_range(Wrap *w, uint32_t lo, uint32_t hi) {
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
static uint32_t index_size(Wrap *ib) {                     /* 2 or 4 (D3DFMT_INDEX32), asked once */
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
static void index_range(Wrap *ib, uint32_t start, uint32_t n, uint32_t isz, uint32_t *lo, uint32_t *hi) {
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
static uint32_t vcount(uint32_t type, uint32_t prims) {
    switch (type) { case 1: return prims; case 2: return prims * 2; case 3: return prims + 1; case 4: return prims * 3;
                    default: return prims + 2; }
}
/* before DrawPrimitive (indexed = 0: start vertex, count) / DrawIndexedPrimitive (base, min, num, start index) */
static void flush_draw(int indexed, uint32_t type, int32_t base, uint32_t first, uint32_t num, uint32_t start_index,
                       uint32_t prims) {
    /* Indexed draws: the vertex range comes from the indices themselves. The game's MinVertexIndex / NumVertices are
     * not always right (Direct3D 9 does not check them), and copying by them left vertices out: stretched triangles. */
    int have_minmax = 0;
    uint32_t imin = 0, imax = 0;
    for (int sidx = 0; sidx < 16; sidx++) {
        Wrap *v = dxb_of(stream_buf[sidx]);
        if (!v || !v->slice || v->dlo >= v->dhi) continue;
        if (stream_freq[sidx] != 1 && stream_freq[sidx] != 0) { flush_range(v, 0, v->shadow_size); continue; }
        if (indexed && !have_minmax) {
            have_minmax = -1;
            Wrap *ib = dxb_of(cur_ib);
            if (ib && ib->shadow) {
                uint32_t isz = index_size(ib), n = vcount(type, prims);
                if ((uint64_t)(start_index + n) * isz <= ib->shadow_size) {
                    if (n) { index_range(ib, start_index, n, isz, &imin, &imax); have_minmax = 1; }
                }
            }
        }
        if (indexed && have_minmax == 1) { first = imin; num = imax - imin + 1; }
        int64_t v0 = indexed ? (int64_t)base + first : first, nv = indexed ? num : vcount(type, prims);
        int64_t lo = stream_off[sidx] + v0 * stream_stride[sidx], hi = lo + nv * stream_stride[sidx];
        if (lo < 0) lo = 0;
        if (hi > lo) flush_range(v, (uint32_t)lo, (uint32_t)hi);
    }
    if (indexed) {
        Wrap *ib = dxb_of(cur_ib);
        if (ib && ib->slice && ib->dlo < ib->dhi) {
            uint32_t isz = index_size(ib), lo = start_index * isz;
            flush_range(ib, lo, lo + vcount(type, prims) * isz);
        }
    }
}
__attribute__((noinline)) static uint32_t unlock_buffer(Wrap *w) {
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

/* ---- the methods a signature cannot describe ---- */
#define CALL(nat, slot, ...) dxb_method(nat, slot)(nat, __VA_ARGS__)
__attribute__((noinline)) static uint32_t special(CPU *c, uint32_t a, Wrap *w, int slot) {
    void *n = w->nat;
    if (slot == 0) {                                    /* QueryInterface(riid, ppv) */
        void *o = NULL;
        uint32_t r = (uint32_t)CALL(n, 0, (uint64_t)(uintptr_t)HOST(ARG(1)), (uint64_t)(uintptr_t)&o, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        if (ARG(2)) W32(ARG(2), o ? dxb_wrap(o, o == n ? w->type : type_of_resource(o)) : 0);
        return r;
    }
    void *own = w->ring_on > 0 ? w->ring[0] : n;           /* the reference count is the original buffer's */
    if (slot == 1) return (uint32_t)CALL(own, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    if (slot == 2) {
        uint32_t r = (uint32_t)CALL(own, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        if (!r && w->ring_on > 0) ring_free(w);
        if (!r) unwrap(w);
        return r;
    }
    switch (w->type) {
    case DXB_D3D9:
        if (slot == 15) return 0x00010002u;              /* GetAdapterMonitor: an HMONITOR the game only passes on */
        if (slot == 16) {                                 /* CreateDevice */
            uint8_t pp[64];
            pp_in(ARG(5), pp);
            void *dev = NULL;
            uint32_t r = (uint32_t)CALL(n, 16, ARG(1), ARG(2), (uint64_t)(uintptr_t)g_window, ARG(4),
                                        (uint64_t)(uintptr_t)pp, (uint64_t)(uintptr_t)&dev, 0, 0, 0, 0, 0);
            pp_out(pp, ARG(5));
            if (!(ARG(4) & 2u)) c->fpu_cw = (uint16_t)(c->fpu_cw & ~0x0300u);   /* no FPU_PRESERVE: 24-bit x87 */
            device_guest = dev ? dxb_wrap(dev, DXB_DEVICE) : 0;
            W32(ARG(6), device_guest);
            fprintf(stderr, "usf: DXVK device %ux%u -> %08x\n", R32(ARG(5)), R32(ARG(5) + 4), r);
            if (!r) apply_aspect(R32(ARG(5)), R32(ARG(5) + 4));   /* Hor+ for the window's shape (d3d9_device.c) */
            return r;
        }
        break;
    case DXB_DEVICE:
        switch (slot) {
        case 9: {                                         /* GetCreationParameters: HWND inside */
            struct { uint32_t adapter, type; void *wnd; uint32_t flags; } cp;
            uint32_t r = (uint32_t)CALL(n, 9, (uint64_t)(uintptr_t)&cp, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
            W32(ARG(1), cp.adapter); W32(ARG(1) + 4, cp.type); W32(ARG(1) + 8, HWND_MAIN); W32(ARG(1) + 12, cp.flags);
            return r;
        }
        case 13: case 16: {                               /* CreateAdditionalSwapChain(pp, ppSwap) / Reset(pp) */
            uint8_t pp[64];
            pp_in(ARG(1), pp);
            void *sc = NULL;
            uint32_t r = (uint32_t)CALL(n, slot, (uint64_t)(uintptr_t)pp, slot == 13 ? (uint64_t)(uintptr_t)&sc : 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
            pp_out(pp, ARG(1));
            if (slot == 13) W32(ARG(2), sc ? dxb_wrap(sc, DXB_SWAPCHAIN) : 0);
            if (slot == 16 && !r) apply_aspect(R32(ARG(1)), R32(ARG(1) + 4));   /* a new size from the game's Options */
            return r;
        }
        case 17: {                                        /* Present(src, dst, window, dirty) */
            uint32_t r = (uint32_t)CALL(n, 17, ARG(1) ? (uint64_t)(uintptr_t)HOST(ARG(1)) : 0,
                                        ARG(2) ? (uint64_t)(uintptr_t)HOST(ARG(2)) : 0, ARG(3) ? (uint64_t)(uintptr_t)g_window : 0,
                                        ARG(4) ? (uint64_t)(uintptr_t)HOST(ARG(4)) : 0, 0, 0, 0, 0, 0, 0, 0);
            after_present(n, NULL);
            game_pace();                                  /* 60 / 120 fps and the engine's rates (d3d9_device.c) */
            return r;
        }
        case 43: {                                        /* Clear(count, rects, flags, color, float z, stencil) */
            float z;
            uint32_t zb = ARG(5);
            memcpy(&z, &zb, 4);
            return (uint32_t)((int32_t (*)(void *, uint32_t, const void *, uint32_t, uint32_t, float, uint32_t))dxb_method(n, 43))(
                n, ARG(1), ARG(2) ? HOST(ARG(2)) : NULL, ARG(3), ARG(4), z, ARG(6));
        }
        case 79: {
            float f;
            uint32_t fb = ARG(1);
            memcpy(&f, &fb, 4);
            return (uint32_t)((int32_t (*)(void *, float))dxb_method(n, 79))(n, f);
        }
        case 80: { float f = ((float (*)(void *))dxb_method(n, 80))(n); FPUSH(f); return 0; }
        }
        break;
    case DXB_TEXTURE: case DXB_CUBE: {
        int cube = w->type == DXB_CUBE;
        uint32_t face = cube ? ARG(1) : 0, level = cube ? ARG(2) : ARG(1);
        if (slot == 19) {
            uint32_t desc[8];
            CALL(n, 17, level, (uint64_t)(uintptr_t)desc, 0, 0, 0, 0, 0, 0, 0, 0, 0);
            return lock_rect(w, 19, face, level, ARG(cube ? 3 : 2), ARG(cube ? 4 : 3), ARG(cube ? 5 : 4), desc, face << 8 | level);
        }
        if (slot == 20) return unlock(w, 20, face << 8 | level, cube ? face : level, cube ? level : 0);
        break;
    }
    case DXB_VOLTEX:
        if (slot == 19) {
            uint32_t desc[7];
            CALL(n, 17, ARG(1), (uint64_t)(uintptr_t)desc, 0, 0, 0, 0, 0, 0, 0, 0, 0);
            return lock_box(w, 19, ARG(1), ARG(2), ARG(3), ARG(4), desc[6], ARG(1));
        }
        if (slot == 20) return unlock(w, 20, ARG(1), ARG(1), 0);
        break;
    case DXB_SURFACE: case DXB_VOLUME:
        if (slot == (w->type == DXB_SURFACE ? 11 : 7)) { /* GetContainer(riid, pp) */
            void *o = NULL;
            uint32_t r = (uint32_t)CALL(n, slot, (uint64_t)(uintptr_t)HOST(ARG(1)), (uint64_t)(uintptr_t)&o, 0, 0, 0, 0, 0, 0, 0, 0, 0);
            if (ARG(2)) W32(ARG(2), !o ? 0 : R32(ARG(1)) == 0xd0223b96u ? dxb_wrap(o, DXB_DEVICE) : dxb_wrap(o, type_of_resource(o)));
            return r;
        }
        if (w->type == DXB_SURFACE) {
            if (slot == 13) {
                uint32_t desc[8];
                CALL(n, 12, (uint64_t)(uintptr_t)desc, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
                if (lock_readback(w, ARG(1), ARG(2), ARG(3), desc)) return 0;
                return lock_rect(w, 13, 0, 0, ARG(1), ARG(2), ARG(3), desc, 0);
            }
            if (slot == 14) {
                if (w->rb_open) { w->rb_open = 0; return 0; }
                return unlock(w, 14, 0, 0, 0);
            }
            if (slot == 15) return BAD;                    /* GetDC */
        } else {
            if (slot == 9) {
                uint32_t desc[7];
                CALL(n, 8, (uint64_t)(uintptr_t)desc, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
                return lock_box(w, 9, 0, ARG(1), ARG(2), ARG(3), desc[6], 0);
            }
            if (slot == 10) return unlock(w, 10, 0, 0, 0);
        }
        break;
    case DXB_VB: case DXB_IB:
        if (slot == 11) return lock_buffer(w, ARG(1), ARG(2), ARG(3), ARG(4));
        if (slot == 12) return unlock_buffer(w);
        break;
    case DXB_SWAPCHAIN:
        if (slot == 3) {
            uint32_t r = (uint32_t)CALL(n, 3, ARG(1) ? (uint64_t)(uintptr_t)HOST(ARG(1)) : 0,
                                        ARG(2) ? (uint64_t)(uintptr_t)HOST(ARG(2)) : 0, ARG(3) ? (uint64_t)(uintptr_t)g_window : 0,
                                        ARG(4) ? (uint64_t)(uintptr_t)HOST(ARG(4)) : 0, ARG(5), 0, 0, 0, 0, 0, 0);
            after_present(dxb_nat(device_guest), n);
            return r;
        }
        if (slot == 9) {
            uint8_t pp[64];
            uint32_t r = (uint32_t)CALL(n, 9, (uint64_t)(uintptr_t)pp, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
            pp_out(pp, ARG(1));
            return r;
        }
        break;
    }
    fprintf(stderr, "usf: DXVK bridge: no special case for type %d method %d\n", w->type, slot);
    return BAD;
}

/* Direct3DCreate9 with DXVK: the wrapper of DXVK's IDirect3D9 */
uint32_t dxb_create9(uint32_t sdk);
uint32_t dxb_create9(uint32_t sdk) {
    void *d = nat_create9 ? nat_create9(sdk) : NULL;
    return d ? dxb_wrap(d, DXB_D3D9) : 0;
}
