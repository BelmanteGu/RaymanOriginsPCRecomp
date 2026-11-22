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
void dxb_overlay_draw(void *dev);
void dxb_after_present(void *dev, void *chain);   /* dxvk_present.c */

uint32_t gpu_epoch = 1;
uint64_t copied_buf, copied_tex_in, copied_tex_out;
uint64_t checked_bytes, checked_locks;               /* checked buffer locks: bytes marked past the range */
uint32_t usf_ms(void);   /* bytes copied by locks (statistics) */
uint32_t stream_buf[16], stream_off[16], stream_stride[16], stream_freq[16], cur_ib;   /* bound for draws */

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
Wrap *wraps;
static uint32_t nwraps = 1, freelist;
static void **hkeys;                                    /* DXVK object -> wrapper index (open addressing) */
static uint32_t *hvals;
static uint32_t vtables[DXB_NTYPES];
pthread_mutex_t wlock = PTHREAD_MUTEX_INITIALIZER;
uint32_t device_guest;
uint32_t dxb_device_guest(void) { return device_guest; }

static uint32_t hslot(void *p) { return (uint32_t)(((uintptr_t)p >> 4) * 2654435761u) & (HASH - 1); }
static uint32_t hfind(void *p) {
    for (uint32_t i = hslot(p);; i = (i + 1) & (HASH - 1)) {
        if (!hkeys[i]) return 0;
        if (hkeys[i] == p) return hvals[i];
    }
}
void hput(void *p, uint32_t v) {
    uint32_t i = hslot(p);
    while (hkeys[i] && hkeys[i] != p && hkeys[i] != (void *)1) i = (i + 1) & (HASH - 1);
    hkeys[i] = p; hvals[i] = v;
}
void hdel(void *p) {                              /* tombstone (1) keeps later probes going */
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
    if (w->type == DXB_DEVICE && (slot == 65 || (slot >= 81 && slot <= 84)) && getenv("USF_HIDE_TEX")) {
        /* (analysis) USF_HIDE_TEX=<w>x<h>x<format hex>,...: draws whose stage-0 texture is one of these are skipped
         * (e.g. the menu's UI atlases, to capture the scene behind it) */
        static uint32_t tex0;
        if (slot == 65) { if (ARG(1) == 0) tex0 = ARG(2); }
        else if (tex0 && dxb_nat(tex0) && dxb_of(tex0)->type == DXB_TEXTURE) {
            void *t = dxb_nat(tex0);
            uint32_t d[8] = {0};
            dxb_method(t, 17)(t, 0, (uint64_t)(uintptr_t)d, 0, 0, 0, 0, 0, 0, 0, 0, 0);
            char key[48];
            snprintf(key, sizeof key, "%ux%ux%x", d[6], d[7], d[0]);
            if (strstr(getenv("USF_HIDE_TEX"), key)) return 0;
        }
    }
    if (w->type == DXB_DEVICE) switch (slot) {
    case 100: if (ARG(1) < 16) { stream_buf[ARG(1)] = ARG(2); stream_off[ARG(1)] = ARG(3); stream_stride[ARG(1)] = ARG(4); } break;
    case 102: if (ARG(1) < 16) stream_freq[ARG(1)] = ARG(2); break;
    case 104: cur_ib = ARG(1); break;
    case 81: dxl_flush_draw(0, ARG(1), 0, ARG(2), 0, 0, ARG(3)); break;
    case 82: dxl_flush_draw(1, ARG(1), (int32_t)ARG(2), ARG(3), ARG(4), ARG(5), ARG(6)); break;
    case 85: for (int k = 0; k < 16; k++) { Wrap *v = dxb_of(stream_buf[k]); if (v) dxl_flush_range(v, 0, v->shadow_size); } break;
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
__thread float call_ms[DXB_NTYPES][120];
double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
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
        if (!r && w->ring_on > 0) dxl_ring_free(w);
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
            dxb_overlay_draw(n);                          /* the touch layer over the frame (dxvk_overlay.c) */
            uint32_t r = (uint32_t)CALL(n, 17, ARG(1) ? (uint64_t)(uintptr_t)HOST(ARG(1)) : 0,
                                        ARG(2) ? (uint64_t)(uintptr_t)HOST(ARG(2)) : 0, ARG(3) ? (uint64_t)(uintptr_t)g_window : 0,
                                        ARG(4) ? (uint64_t)(uintptr_t)HOST(ARG(4)) : 0, 0, 0, 0, 0, 0, 0, 0);
            dxb_after_present(n, NULL);
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
            return dxl_lock_rect(w, 19, face, level, ARG(cube ? 3 : 2), ARG(cube ? 4 : 3), ARG(cube ? 5 : 4), desc, face << 8 | level);
        }
        if (slot == 20) return dxl_unlock(w, 20, face << 8 | level, cube ? face : level, cube ? level : 0);
        break;
    }
    case DXB_VOLTEX:
        if (slot == 19) {
            uint32_t desc[7];
            CALL(n, 17, ARG(1), (uint64_t)(uintptr_t)desc, 0, 0, 0, 0, 0, 0, 0, 0, 0);
            return dxl_lock_box(w, 19, ARG(1), ARG(2), ARG(3), ARG(4), desc[6], ARG(1));
        }
        if (slot == 20) return dxl_unlock(w, 20, ARG(1), ARG(1), 0);
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
                if (dxl_lock_readback(w, ARG(1), ARG(2), ARG(3), desc)) return 0;
                return dxl_lock_rect(w, 13, 0, 0, ARG(1), ARG(2), ARG(3), desc, 0);
            }
            if (slot == 14) {
                if (w->rb_open) { w->rb_open = 0; return 0; }
                return dxl_unlock(w, 14, 0, 0, 0);
            }
            if (slot == 15) return BAD;                    /* GetDC */
        } else {
            if (slot == 9) {
                uint32_t desc[7];
                CALL(n, 8, (uint64_t)(uintptr_t)desc, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
                return dxl_lock_box(w, 9, 0, ARG(1), ARG(2), ARG(3), desc[6], 0);
            }
            if (slot == 10) return dxl_unlock(w, 10, 0, 0, 0);
        }
        break;
    case DXB_VB: case DXB_IB:
        if (slot == 11) return dxl_lock_buffer(w, ARG(1), ARG(2), ARG(3), ARG(4));
        if (slot == 12) return dxl_unlock_buffer(w);
        break;
    case DXB_SWAPCHAIN:
        if (slot == 3) {
            uint32_t r = (uint32_t)CALL(n, 3, ARG(1) ? (uint64_t)(uintptr_t)HOST(ARG(1)) : 0,
                                        ARG(2) ? (uint64_t)(uintptr_t)HOST(ARG(2)) : 0, ARG(3) ? (uint64_t)(uintptr_t)g_window : 0,
                                        ARG(4) ? (uint64_t)(uintptr_t)HOST(ARG(4)) : 0, ARG(5), 0, 0, 0, 0, 0, 0);
            dxb_after_present(dxb_nat(device_guest), n);
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
