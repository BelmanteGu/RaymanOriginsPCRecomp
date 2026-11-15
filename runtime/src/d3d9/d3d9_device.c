/* IDirect3D9, IDirect3DDevice9 and IDirect3DSwapChain9.
 *
 * What it does: answers the adapter questions as one GPU with Shader Model 3.0 and a 1920x1080 desktop; CreateDevice
 * builds the device and its back buffer, starts the renderer, and, unless the game passes D3DCREATE_FPU_PRESERVE,
 * switches the calling thread's x87 unit to 24-bit precision exactly as Direct3D 9 does on Windows (the game's math
 * depends on it). Device methods record state in g_dev, create resources (d3d9_objects.c) and pass clears, draws and
 * presents to the renderer. Method indices follow the Windows interfaces; every method pops what its call site pushed.
 * What it does not do: fixed-function lighting/transforms (recorded, not drawn), patches, software vertex processing.
 */
#include <errno.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

#include "d3d9.h"
#include "../game.h"

DevState g_dev;
uint32_t g_device_guest, g_d3d9_guest;
static uint32_t g_swapchain;
#define BAD 0x8876086Cu   /* D3DERR_INVALIDCALL */

static uint32_t ok(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
static uint32_t qi(CPU *c, uint32_t a) { (void)c; if (ARG(2)) W32(ARG(2), ARG(0)); return 0; }
static uint32_t addref(CPU *c, uint32_t a) { (void)c; (void)a; return 2; }
static uint32_t release(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }

/* ---- IDirect3D9 ---- */
static uint32_t GetAdapterCount(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
static uint32_t GetAdapterIdentifier(CPU *c, uint32_t a) {
    (void)c;
    uint32_t p = ARG(3);
    memset(HOST(p), 0, 1100);
    strcpy((char *)HOST(p), "rayman-vulkan.dll");
    strcpy((char *)HOST(p + 512), "RaymanOrigins-PC Vulkan renderer");
    strcpy((char *)HOST(p + 1024), "\\\\.\\DISPLAY1");
    W32(p + 1056, 0x00010000u); W32(p + 1060, 0x00090000u);   /* driver version 9.x */
    W32(p + 1064, 0x10DE); W32(p + 1068, 0x0640);               /* a Shader Model 3.0-class NVIDIA id */
    W32(p + 1096, 1);
    return 0;
}
static uint32_t modes[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1280, 1024}, {1366, 768},
                                    {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160},
                                    {1920, 800}, {2560, 1080}, {3440, 1440}, {3840, 1600}, {5120, 1440},   /* 21:9, 32:9 */
                                    {0, 0}};                         /* the screen's shape (USF_AUTO_RES), filled at first use */
#define NMODES (modes[15][0] ? 16 : 15)
static void auto_mode(void) {
    int usf_auto_resolution(uint32_t *w, uint32_t *h);
    static int done;
    if (!done) { done = 1; usf_auto_resolution(&modes[15][0], &modes[15][1]); }
}
static uint32_t GetAdapterModeCount(CPU *c, uint32_t a) { (void)c; (void)a; auto_mode(); return NMODES; }
static uint32_t EnumAdapterModes(CPU *c, uint32_t a) {
    (void)c;
    auto_mode();
    if (ARG(3) >= NMODES) return BAD;
    W32(ARG(4), modes[ARG(3)][0]); W32(ARG(4) + 4, modes[ARG(3)][1]); W32(ARG(4) + 8, 60); W32(ARG(4) + 12, 22);
    return 0;
}
static uint32_t GetAdapterDisplayMode(CPU *c, uint32_t a) {
    (void)c;
    W32(ARG(2), 1920); W32(ARG(2) + 4, 1080); W32(ARG(2) + 8, 60); W32(ARG(2) + 12, 22);   /* X8R8G8B8 */
    return 0;
}
static uint32_t CheckMultiSample(CPU *c, uint32_t a) { (void)c; if (ARG(6)) W32(ARG(6), 1); return ARG(5) <= 1 ? 0 : 0x8876086Au; }

static void fill_caps(uint32_t p) {
    memset(HOST(p), 0, 304);
    for (uint32_t off = 8; off < 88; off += 4) W32(p + off, 0xFFFFFFFFu);   /* every capability bit of the early fields */
    W32(p, 1); W32(p + 4, 0);
    /* TextureCaps: not every bit, some are restrictions (POW2 0x2, SQUAREONLY 0x20, NONPOW2CONDITIONAL 0x100,
     * ALPHAPALETTE 0x80): perspective, alpha, repeat not scaled, projected, cube/volume maps and their mips. */
    W32(p + 60, 0x1u | 0x4u | 0x40u | 0x400u | 0x800u | 0x2000u | 0x4000u | 0x8000u | 0x10000u | 0x200000u);
    W32(p + 20, 0x8000000Fu);                                  /* presentation intervals: immediate, one..four */
    W32(p + 88, 8192); W32(p + 92, 8192); W32(p + 96, 2048); W32(p + 100, 8192); W32(p + 104, 8192); W32(p + 108, 16);
    float f;
    f = 1e10f; memcpy(HOST(p + 112), &f, 4);
    f = -1e8f; memcpy(HOST(p + 116), &f, 4); memcpy(HOST(p + 120), &f, 4);
    f = 1e8f; memcpy(HOST(p + 124), &f, 4); memcpy(HOST(p + 128), &f, 4);
    W32(p + 136, 0xFFFFFFFFu); W32(p + 140, 0x00080008u); W32(p + 144, 0xFFFFFFFFu);
    W32(p + 148, 8); W32(p + 152, 8); W32(p + 156, 0xFFFFFFFFu); W32(p + 160, 8); W32(p + 164, 8);
    W32(p + 168, 4); W32(p + 172, 255);
    f = 8192.0f; memcpy(HOST(p + 176), &f, 4);
    W32(p + 180, 0x00FFFFFFu); W32(p + 184, 0x00FFFFFFu); W32(p + 188, 16); W32(p + 192, 255);
    W32(p + 196, 0xFFFE0300u); W32(p + 200, 256); W32(p + 204, 0xFFFF0300u);
    f = 65504.0f; memcpy(HOST(p + 208), &f, 4);
    W32(p + 212, 0xFFFFFFFFu); W32(p + 236, 0x3FFu); W32(p + 240, 4); W32(p + 244, 0x03000300u);
    W32(p + 248, 1); W32(p + 252, 24); W32(p + 256, 32); W32(p + 260, 4);
    W32(p + 264, 0x1F); W32(p + 268, 24); W32(p + 272, 32); W32(p + 276, 4); W32(p + 280, 512);
    W32(p + 284, 0x03000300u); W32(p + 288, 65535); W32(p + 292, 65535); W32(p + 296, 32768); W32(p + 300, 32768);
}
static uint32_t D3D9GetDeviceCaps(CPU *c, uint32_t a) { (void)c; fill_caps(ARG(3)); return 0; }
static uint32_t DevGetDeviceCaps(CPU *c, uint32_t a) { (void)c; fill_caps(ARG(1)); return 0; }
static uint32_t GetAdapterMonitor(CPU *c, uint32_t a) { (void)c; (void)a; return 0x00010001u; }

static uint32_t make_backbuffer(uint32_t pp) {
    uint32_t w = R32(pp), h = R32(pp + 4), fmt = R32(pp + 8);
    if (!w || !h) { w = 1280; h = 720; }
    if (!fmt) fmt = 22;
    g_dev.bb_w = w; g_dev.bb_h = h; g_dev.bb_fmt = fmt;
    W32(pp, w); W32(pp + 4, h);
    g_dev.backbuffer = d3d_new_surface(fmt, w, h, 1 /* RENDERTARGET */, 0, 0, 0, 0);
    g_dev.rt[0] = g_dev.backbuffer;
    if (R32(pp + 36)) {
        g_dev.auto_ds = d3d_new_surface(R32(pp + 40), w, h, 2 /* DEPTHSTENCIL */, 0, 0, 0, 0);
        g_dev.ds = g_dev.auto_ds;
    }
    g_dev.vp[0] = 0; g_dev.vp[1] = 0; g_dev.vp[2] = w; g_dev.vp[3] = h;
    float one = 1.0f; memcpy(&g_dev.vp[5], &one, 4);
    return w | h << 16;
}

/* Ultrawide (Hor+): UbiArt fits its cameras and screen rectangles to the 16/9, 9/16 pair at GAME_ASPECT_PAIR (game.h);
 * both become A and 1/A in guest memory, so the view gets wider and the 2D keeps its proportions (the engine places
 * it). USF_ASPECT: auto (the back buffer's shape when wider than 16:9; the default), 16:9 (the game as it was), 21:9,
 * 32:9, or a number. Nothing in the game's files changes. */
float g_aspect = 16.0f / 9.0f;
static void apply_aspect(uint32_t w, uint32_t h) {
    const char *e = getenv("USF_ASPECT");
    float a = h ? (float)w / (float)h : 16.0f / 9.0f;
    if (e && strcmp(e, "auto")) {
        float x = 0, y = 0;
        if (sscanf(e, "%f:%f", &x, &y) == 2 && y > 0) a = x / y; else if (sscanf(e, "%f", &x) == 1 && x > 0) a = x;
    }
    if (a < 16.0f / 9.0f + 0.002f) return;                      /* 16:9 or narrower: the game as it was */
    if (a > 32.0f / 9.0f) a = 32.0f / 9.0f;
    g_aspect = a;
    float inv = 1.0f / a;
    if (R32(GAME_ASPECT_PAIR) == 0x3FE38E39u && R32(GAME_ASPECT_PAIR + 4) == 0x3F100000u) {
        memcpy(HOST(GAME_ASPECT_PAIR), &a, 4);
        memcpy(HOST(GAME_ASPECT_PAIR + 4), &inv, 4);
    } else fprintf(stderr, "usf: ultrawide: the 16/9 pair is not where expected; left as it is\n");
    fprintf(stderr, "usf: ultrawide: aspect %.4f (Hor+)\n", a);
}

static uint32_t CreateDevice(CPU *c, uint32_t a) {
    uint32_t flags = ARG(4), pp = ARG(5);
    if (!(flags & 2u)) {                                       /* no D3DCREATE_FPU_PRESERVE: single precision, as on Windows */
        c->fpu_cw = (uint16_t)(c->fpu_cw & ~0x0300u);
        usf_log("CreateDevice: x87 set to 24-bit precision (no FPU_PRESERVE)\n");
    }
    D3DObj *o;
    g_device_guest = d3d_new(OBJ_DEVICE, &o);
    make_backbuffer(pp);
    for (int s = 0; s < 16; s++) { g_dev.ss[s][1] = 1; g_dev.ss[s][2] = 1; g_dev.ss[s][3] = 1; g_dev.ss[s][5] = 1; g_dev.ss[s][6] = 1; }
    g_dev.rs[7] = 1; g_dev.rs[14] = 1; g_dev.rs[23] = 4; g_dev.rs[22] = 3; g_dev.rs[19] = 2; g_dev.rs[20] = 1;   /* Z on, CCW cull... */
    g_dev.rs[168] = 0xF;                                       /* COLORWRITEENABLE */
    /* the other D3D9 defaults the game relies on (Scaleform's masks never set the stencil write mask) */
    g_dev.rs[58] = g_dev.rs[59] = 0xFFFFFFFFu;                 /* STENCILMASK, STENCILWRITEMASK */
    g_dev.rs[53] = g_dev.rs[54] = g_dev.rs[55] = 1;            /* STENCILFAIL / ZFAIL / PASS = KEEP */
    g_dev.rs[56] = 8;                                          /* STENCILFUNC = ALWAYS */
    g_dev.rs[186] = g_dev.rs[187] = g_dev.rs[188] = 1; g_dev.rs[189] = 8;   /* CCW stencil */
    g_dev.rs[190] = g_dev.rs[191] = g_dev.rs[192] = 0xF;       /* COLORWRITEENABLE1..3 */
    g_dev.rs[19] = 2; g_dev.rs[20] = 1; g_dev.rs[171] = 1;     /* SRCBLEND ONE, DESTBLEND ZERO, BLENDOP ADD */
    g_dev.rs[207] = 2; g_dev.rs[208] = 1; g_dev.rs[209] = 1;   /* SRCBLENDALPHA, DESTBLENDALPHA, BLENDOPALPHA */
    g_dev.rs[25] = 8; g_dev.rs[23] = 4;                        /* ALPHAFUNC ALWAYS, ZFUNC LESSEQUAL */
    fprintf(stderr, "usf: Direct3D 9 device %ux%u format %u windowed %u flags %08x\n", g_dev.bb_w, g_dev.bb_h,
            g_dev.bb_fmt, R32(pp + 32), flags);
    apply_aspect(g_dev.bb_w, g_dev.bb_h);
    r_init(g_dev.bb_w, g_dev.bb_h);
    W32(ARG(6), g_device_guest);
    return 0;
}

HleFn vt_d3d9[17] = {qi, addref, release, ok, GetAdapterCount, GetAdapterIdentifier, GetAdapterModeCount,
                     EnumAdapterModes, GetAdapterDisplayMode, ok, ok, CheckMultiSample, ok, ok, D3D9GetDeviceCaps,
                     GetAdapterMonitor, CreateDevice};

uint32_t x_Direct3DCreate9(CPU *c, uint32_t a) {
    (void)c;
    usf_log("Direct3DCreate9(%u)\n", ARG(0));
    if (!g_d3d9_guest) g_d3d9_guest = d3d_new(OBJ_D3D9, NULL);
    return g_d3d9_guest;
}

/* ---- device: resources ---- */
static uint32_t new_texture(int kind, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, uint32_t usage,
                            uint32_t fmt, uint32_t pool, uint32_t out) {
    D3DObj *o;
    uint32_t g = d3d_new(kind, &o);
    if (!levels) { levels = 1; for (uint32_t m = w > h ? w : h; m > 1; m >>= 1) levels++; }
    if (levels > MAX_LEVELS) levels = MAX_LEVELS;
    o->width = w; o->height = h; o->depth = d ? d : 1; o->levels = levels; o->usage = usage; o->format = fmt; o->pool = pool;
    int faces = kind == OBJ_CUBE ? 6 : 1;
    for (uint32_t l = 0; l < levels; l++) {
        uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1, ld = o->depth >> l ? o->depth >> l : 1;
        o->pitch[l] = d3d_pitch(fmt, lw);
        o->size[l] = d3d_level_size(fmt, lw, lh) * ld;
        for (int f = 0; f < faces; f++) o->mem[f][l] = gcalloc(o->size[l]);
    }
    W32(out, g);
    return 0;
}
uint32_t d3d_create_texture(int kind, uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool) {
    uint32_t out = gmalloc(4);
    new_texture(kind, w, h, 1, levels, usage, fmt, pool, out);
    uint32_t g = R32(out);
    gfree(out);
    return g;
}
static uint32_t CreateTexture(CPU *c, uint32_t a) {
    (void)c;
    if (getenv("USF_TEXLOG")) fprintf(stderr, "CreateTexture %ux%u levels %u usage %x fmt %u pool %u (from %08x)\n", ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), R32(a - 4));
    return new_texture(OBJ_TEXTURE, ARG(1), ARG(2), 1, ARG(3), ARG(4), ARG(5), ARG(6), ARG(7));
}
static uint32_t CreateVolumeTexture(CPU *c, uint32_t a) { (void)c; return new_texture(OBJ_VOLUME, ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), ARG(7), ARG(8)); }
static uint32_t CreateCubeTexture(CPU *c, uint32_t a) { (void)c; return new_texture(OBJ_CUBE, ARG(1), ARG(1), 1, ARG(2), ARG(3), ARG(4), ARG(5), ARG(6)); }

static uint32_t new_buffer(int kind, uint32_t len, uint32_t usage, uint32_t fmt, uint32_t pool, uint32_t out) {
    D3DObj *o;
    uint32_t g = d3d_new(kind, &o);
    o->width = len; o->usage = usage; o->format = fmt; o->pool = pool;
    o->mem[0][0] = gcalloc(len ? len : 4);
    W32(out, g);
    return 0;
}
static uint32_t CreateVertexBuffer(CPU *c, uint32_t a) { (void)c; return new_buffer(OBJ_VB, ARG(1), ARG(2), ARG(3), ARG(4), ARG(5)); }
static uint32_t CreateIndexBuffer(CPU *c, uint32_t a) { (void)c; return new_buffer(OBJ_IB, ARG(1), ARG(2), ARG(3), ARG(4), ARG(5)); }
static uint32_t CreateRenderTarget(CPU *c, uint32_t a) { (void)c; W32(ARG(7), d3d_new_surface(ARG(3), ARG(1), ARG(2), 1, 0, 0, 0, 0)); return 0; }
static uint32_t CreateDepthStencilSurface(CPU *c, uint32_t a) { (void)c; W32(ARG(7), d3d_new_surface(ARG(3), ARG(1), ARG(2), 2, 0, 0, 0, 0)); return 0; }
static uint32_t CreateOffscreenPlainSurface(CPU *c, uint32_t a) { (void)c; W32(ARG(5), d3d_new_surface(ARG(3), ARG(1), ARG(2), 0, 0, 0, 0, 0)); return 0; }

static uint32_t copy_code(uint32_t src, int decl, uint32_t *size) {
    uint32_t n = 0;
    if (decl) { while (R16(src + 8 * n) != 0xFF) n++; n = 8 * (n + 1); }
    else {
        uint32_t i = 1;                                        /* token 0 is the version */
        for (;;) {
            uint32_t t = R32(src + 4 * i);
            if (t == 0x0000FFFFu) { i++; break; }
            if ((t & 0xFFFF) == 0xFFFE) { i += 1 + ((t >> 16) & 0x7FFF); continue; }
            i++;
            if (i > 1 << 20) break;
        }
        n = 4 * i;
    }
    uint32_t p = gmalloc(n);
    memcpy(HOST(p), HOST(src), n);
    *size = n;
    return p;
}
static uint32_t new_code(int kind, uint32_t src, uint32_t out) {
    D3DObj *o;
    uint32_t g = d3d_new(kind, &o);
    o->code = copy_code(src, kind == OBJ_DECL, &o->code_size);
    const char *dump = getenv("USF_SHADER_DUMP");             /* write the game's shaders for the translator tests */
    if (dump && kind != OBJ_DECL) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s_%05u.bin", dump, kind == OBJ_VS ? "vs" : "ps", R32(g + 8));
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(HOST(o->code), 1, o->code_size, f); fclose(f); }
    }
    W32(out, g);
    return 0;
}
uint32_t d3d_create_shader(int kind, uint32_t src) {          /* for ID3DXEffect (d3dx9_effect.c) */
    uint32_t out = gmalloc(4), g;
    new_code(kind, src, out);
    g = R32(out);
    gfree(out);
    return g;
}
static uint32_t CreateVertexDeclaration(CPU *c, uint32_t a) { (void)c; return new_code(OBJ_DECL, ARG(1), ARG(2)); }
static uint32_t CreateVertexShader(CPU *c, uint32_t a) { (void)c; return new_code(OBJ_VS, ARG(1), ARG(2)); }
static uint32_t CreatePixelShader(CPU *c, uint32_t a) { (void)c; return new_code(OBJ_PS, ARG(1), ARG(2)); }
static uint32_t CreateQuery(CPU *c, uint32_t a) {
    (void)c;
    if (!ARG(2)) return 0;
    D3DObj *o;
    W32(ARG(2), d3d_new(OBJ_QUERY, &o));
    o->query_type = ARG(1);
    return 0;
}
static uint32_t CreateStateBlock(CPU *c, uint32_t a) { (void)c; W32(ARG(2), d3d_new(OBJ_STATEBLOCK, NULL)); return 0; }
static uint32_t EndStateBlock(CPU *c, uint32_t a) { (void)c; W32(ARG(1), d3d_new(OBJ_STATEBLOCK, NULL)); return 0; }

/* ---- device: state ---- */
static uint32_t ref_out(uint32_t v, uint32_t out) { W32(out, v); return v ? 0 : 0x8876086Cu; }
static uint32_t GetDirect3D(CPU *c, uint32_t a) { (void)c; W32(ARG(1), g_d3d9_guest); return 0; }
static uint32_t TestCooperativeLevel(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
static uint32_t GetAvailableTextureMem(CPU *c, uint32_t a) { (void)c; (void)a; return 1024u << 20; }
static uint32_t GetDisplayMode(CPU *c, uint32_t a) { (void)c; W32(ARG(2), g_dev.bb_w); W32(ARG(2) + 4, g_dev.bb_h); W32(ARG(2) + 8, 60); W32(ARG(2) + 12, g_dev.bb_fmt); return 0; }
static uint32_t GetCreationParameters(CPU *c, uint32_t a) { (void)c; W32(ARG(1), 0); W32(ARG(1) + 4, 1); W32(ARG(1) + 8, 0x00010010u); W32(ARG(1) + 12, 0x40); return 0; }
static uint32_t GetSwapChain(CPU *c, uint32_t a) { (void)c; if (!g_swapchain) g_swapchain = d3d_new(OBJ_SWAPCHAIN, NULL); W32(ARG(2), g_swapchain); return 0; }
static uint32_t GetNumberOfSwapChains(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
static uint32_t Reset(CPU *c, uint32_t a) { (void)c; make_backbuffer(ARG(1)); r_init(g_dev.bb_w, g_dev.bb_h); return 0; }
/* The game advances one step per presented frame, so frames are paced: 60 per second (RAYMAN_FPS to change, 0 = no
 * limit; another rate also sets the engine's app-loop and logic rates, so RAYMAN_FPS=120 is 120 real logic steps
 * per second, not interpolation), whatever the display's refresh (a 144 Hz screen would otherwise run the game 2.4 times too fast). A frame
 * that comes late resets the schedule instead of being caught up. */
static void pace(void) {
    static double next;
    static int fps = -1;
    if (fps < 0) { const char *e = getenv("RAYMAN_FPS"); fps = e ? atoi(e) : 60; }
    if (fps <= 0) return;
    if (fps != 60) {                                           /* the engine's own rates follow (game.h) */
        float f = (float)fps, dt = 1.0f / f;
        uint32_t v = GAME_RATE_VARS;
        if (RF32(v) != f || RF32(v + 4) != f) { WF32(v, f); WF32(v + 4, f); WF32(v + 8, dt); WF32(v + 12, dt); }
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = (double)ts.tv_sec + ts.tv_nsec / 1e9, period = 1.0 / fps;
    if (next == 0.0 || now > next + period) next = now;           /* first frame, or late: start again from now */
    else if (now < next) {
        double d = next - now;
        struct timespec sl = {(time_t)d, (long)((d - (double)(time_t)d) * 1e9)};
        while (nanosleep(&sl, &sl) && errno == EINTR) {}
    }
    next += period;
}
static uint32_t Present(CPU *c, uint32_t a) { (void)c; (void)a; r_present(); pace(); g_dev.frames++; return 0; }
static uint32_t GetBackBuffer(CPU *c, uint32_t a) { (void)c; W32(ARG(4), g_dev.backbuffer); return 0; }
static uint32_t GetRasterStatus(CPU *c, uint32_t a) { (void)c; memset(HOST(ARG(2)), 0, 8); return 0; }
static uint32_t SetRenderTarget(CPU *c, uint32_t a) { (void)c; if (ARG(1) < 4) g_dev.rt[ARG(1)] = ARG(2); return 0; }
static uint32_t GetRenderTarget(CPU *c, uint32_t a) { (void)c; return ARG(1) < 4 ? ref_out(g_dev.rt[ARG(1)], ARG(2)) : BAD; }
static uint32_t SetDepthStencilSurface(CPU *c, uint32_t a) { (void)c; g_dev.ds = ARG(1); return 0; }
static uint32_t GetDepthStencilSurface(CPU *c, uint32_t a) { (void)c; return ref_out(g_dev.ds, ARG(1)); }
static uint32_t Clear(CPU *c, uint32_t a) { (void)c; float z; memcpy(&z, HOST(a + 20), 4); r_clear(ARG(3), ARG(4), z, ARG(6)); return 0; }
static uint32_t SetViewport(CPU *c, uint32_t a) { (void)c; memcpy(g_dev.vp, HOST(ARG(1)), 24); return 0; }
static uint32_t GetViewport(CPU *c, uint32_t a) { (void)c; memcpy(HOST(ARG(1)), g_dev.vp, 24); return 0; }
static uint32_t SetRenderState(CPU *c, uint32_t a) { (void)c; if (ARG(1) < 256) g_dev.rs[ARG(1)] = ARG(2); return 0; }
static uint32_t GetRenderState(CPU *c, uint32_t a) { (void)c; W32(ARG(2), ARG(1) < 256 ? g_dev.rs[ARG(1)] : 0); return 0; }
static uint32_t SetTexture(CPU *c, uint32_t a) { (void)c; uint32_t s = ARG(1) >= 257 ? ARG(1) - 257 + 12 : ARG(1); if (s < 16) g_dev.tex[s] = ARG(2); return 0; }
static uint32_t GetTexture(CPU *c, uint32_t a) { (void)c; W32(ARG(2), ARG(1) < 16 ? g_dev.tex[ARG(1)] : 0); return 0; }
static uint32_t SetSamplerState(CPU *c, uint32_t a) { (void)c; uint32_t s = ARG(1) >= 257 ? ARG(1) - 257 + 12 : ARG(1); if (s < 16 && ARG(2) < 14) g_dev.ss[s][ARG(2)] = ARG(3); return 0; }
static uint32_t GetSamplerState(CPU *c, uint32_t a) { (void)c; W32(ARG(3), ARG(1) < 16 && ARG(2) < 14 ? g_dev.ss[ARG(1)][ARG(2)] : 0); return 0; }
static uint32_t ValidateDevice(CPU *c, uint32_t a) { (void)c; W32(ARG(1), 1); return 0; }
static uint32_t SetScissorRect(CPU *c, uint32_t a) { (void)c; memcpy(g_dev.scissor, HOST(ARG(1)), 16); return 0; }
static uint32_t GetScissorRect(CPU *c, uint32_t a) { (void)c; memcpy(HOST(ARG(1)), g_dev.scissor, 16); return 0; }
static uint32_t SetVertexDeclaration(CPU *c, uint32_t a) { (void)c; g_dev.decl = ARG(1); return 0; }
static uint32_t GetVertexDeclaration(CPU *c, uint32_t a) { (void)c; W32(ARG(1), g_dev.decl); return 0; }
static uint32_t SetFVF(CPU *c, uint32_t a) { (void)c; g_dev.fvf = ARG(1); g_dev.decl = 0; return 0; }
static uint32_t GetFVF(CPU *c, uint32_t a) { (void)c; W32(ARG(1), g_dev.fvf); return 0; }
static uint32_t SetVertexShader(CPU *c, uint32_t a) { (void)c; g_dev.vs = ARG(1); return 0; }
static uint32_t GetVertexShader(CPU *c, uint32_t a) { (void)c; W32(ARG(1), g_dev.vs); return 0; }
static uint32_t SetPixelShader(CPU *c, uint32_t a) { (void)c; g_dev.ps = ARG(1); return 0; }
static uint32_t GetPixelShader(CPU *c, uint32_t a) { (void)c; W32(ARG(1), g_dev.ps); return 0; }
static uint32_t SetVSConstF(CPU *c, uint32_t a) { (void)c; if (ARG(1) + ARG(3) <= 256) memcpy(g_dev.vsc[ARG(1)], HOST(ARG(2)), 16 * ARG(3)); return 0; }
static uint32_t GetVSConstF(CPU *c, uint32_t a) { (void)c; if (ARG(1) + ARG(3) <= 256) memcpy(HOST(ARG(2)), g_dev.vsc[ARG(1)], 16 * ARG(3)); return 0; }
static uint32_t SetPSConstF(CPU *c, uint32_t a) { (void)c; if (ARG(1) + ARG(3) <= 224) memcpy(g_dev.psc[ARG(1)], HOST(ARG(2)), 16 * ARG(3)); return 0; }
static uint32_t GetPSConstF(CPU *c, uint32_t a) { (void)c; if (ARG(1) + ARG(3) <= 224) memcpy(HOST(ARG(2)), g_dev.psc[ARG(1)], 16 * ARG(3)); return 0; }
static uint32_t SetVSConstI(CPU *c, uint32_t a) { (void)c; if (ARG(1) + ARG(3) <= 16) memcpy(g_dev.vsi[ARG(1)], HOST(ARG(2)), 16 * ARG(3)); return 0; }
static uint32_t SetPSConstI(CPU *c, uint32_t a) { (void)c; if (ARG(1) + ARG(3) <= 16) memcpy(g_dev.psi[ARG(1)], HOST(ARG(2)), 16 * ARG(3)); return 0; }
static uint32_t SetVSConstB(CPU *c, uint32_t a) { (void)c; if (ARG(1) + ARG(3) <= 16) memcpy(&g_dev.vsb[ARG(1)], HOST(ARG(2)), 4 * ARG(3)); return 0; }
static uint32_t SetPSConstB(CPU *c, uint32_t a) { (void)c; if (ARG(1) + ARG(3) <= 16) memcpy(&g_dev.psb[ARG(1)], HOST(ARG(2)), 4 * ARG(3)); return 0; }
static uint32_t SetStreamSource(CPU *c, uint32_t a) {
    (void)c;
    if (ARG(1) < 16) { g_dev.stream[ARG(1)] = ARG(2); g_dev.stream_off[ARG(1)] = ARG(3); g_dev.stream_stride[ARG(1)] = ARG(4); }
    return 0;
}
static uint32_t GetStreamSource(CPU *c, uint32_t a) {
    (void)c;
    uint32_t s = ARG(1) & 15;
    W32(ARG(2), g_dev.stream[s]); W32(ARG(3), g_dev.stream_off[s]); W32(ARG(4), g_dev.stream_stride[s]);
    return 0;
}
static uint32_t SetStreamSourceFreq(CPU *c, uint32_t a) { (void)c; if (ARG(1) < 16) g_dev.stream_freq[ARG(1)] = ARG(2); return 0; }
static uint32_t SetIndices(CPU *c, uint32_t a) { (void)c; g_dev.ib = ARG(1); return 0; }
static uint32_t GetIndices(CPU *c, uint32_t a) { (void)c; W32(ARG(1), g_dev.ib); return 0; }

/* ---- device: draws ---- */
static uint32_t DrawPrimitive(CPU *c, uint32_t a) { (void)c; r_draw(0, ARG(1), 0, 0, 0, ARG(2), ARG(3), 0, 0, 0, 0); return 0; }
static uint32_t DrawIndexedPrimitive(CPU *c, uint32_t a) { (void)c; r_draw(1, ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), 0, 0, 0, 0); return 0; }
static uint32_t DrawPrimitiveUP(CPU *c, uint32_t a) {
    (void)c;
    if (getenv("USF_DRAWLOG") && g_dev.frames == (uint32_t)atoi(getenv("USF_DRAWLOG"))) {   /* the vertices, as floats */
        uint32_t n = ARG(1) == 6 ? ARG(2) + 2 : ARG(2) * 3, st = ARG(4);
        fprintf(stderr, "UP prim %u count %u stride %u fvf %x decl %08x:", ARG(1), ARG(2), st, g_dev.fvf, g_dev.decl);
        for (uint32_t v = 0; v < n && v < 4; v++) {
            fprintf(stderr, " [");
            for (uint32_t k = 0; k < st / 4; k++) { float f; uint32_t u = R32(ARG(3) + v * st + 4 * k); memcpy(&f, &u, 4); fprintf(stderr, " %g", f); }
            fprintf(stderr, " ]");
        }
        fprintf(stderr, "\n");
    }
    r_draw(0, ARG(1), 0, 0, 0, 0, ARG(2), ARG(3), ARG(4), 0, 0);
    return 0;
}
static uint32_t DrawIndexedPrimitiveUP(CPU *c, uint32_t a) {
    (void)c;
    r_draw(1, ARG(1), 0, ARG(2), ARG(3), 0, ARG(4), ARG(7), ARG(8), ARG(5), ARG(6));
    return 0;
}

/* surface copies: done on the CPU copies in guest memory when formats and sizes match */
void vk_copy_surface(uint32_t src, uint32_t dst);
static uint32_t StretchRect(CPU *c, uint32_t a) {
    (void)c;
    D3DObj *s = d3d_obj(ARG(1)), *d = d3d_obj(ARG(3));
    if (getenv("USF_DRAWLOG") && g_dev.frames == (uint32_t)atoi(getenv("USF_DRAWLOG")))
        fprintf(stderr, "StretchRect %08x(kind %d usage %x parent %08x %ux%u) rect %08x -> %08x(kind %d usage %x parent %08x %ux%u) rect %08x filter %u\n",
                ARG(1), s ? s->kind : -1, s ? s->usage : 0, s ? s->parent : 0, s ? s->width : 0, s ? s->height : 0, ARG(2),
                ARG(3), d ? d->kind : -1, d ? d->usage : 0, d ? d->parent : 0, d ? d->width : 0, d ? d->height : 0, ARG(4), ARG(5));
    if (s && d && ((s->usage & 3u) || ARG(1) == g_dev.backbuffer || (d->usage & 3u) || ARG(3) == g_dev.backbuffer)) {
        vk_copy_surface(ARG(1), ARG(3));                    /* render targets live on the GPU */
        return 0;
    }
    if (s && d && s->format == d->format && s->width == d->width && s->height == d->height && !ARG(2) && !ARG(4)) {
        memcpy(HOST(d->mem[0][0]), HOST(s->mem[0][0]), d->size[0]);
        d->version++;
    }
    return 0;
}
static uint32_t GetRenderTargetData(CPU *c, uint32_t a) {
    (void)c;
    D3DObj *s = d3d_obj(ARG(1)), *d = d3d_obj(ARG(2));
    if (s && d && s->size[0] == d->size[0]) memcpy(HOST(d->mem[0][0]), HOST(s->mem[0][0]), d->size[0]);
    return 0;
}
#define FOURCC_DXT1 0x31545844u
/* UpdateSurface(src, srcRect, dst, dstPoint): a rectangle of a system-memory surface into a texture level. */
static uint32_t UpdateSurface(CPU *c, uint32_t a) {
    (void)c;
    D3DObj *s = d3d_obj(ARG(1)), *d = d3d_obj(ARG(3));
    if (!s || !d || s->format != d->format) return BAD;
    static int once;
    if (!once++) fprintf(stderr, "usf: UpdateSurface used (format %u)\n", s->format);
    uint32_t x0 = 0, y0 = 0, x1 = s->width, y1 = s->height, dx = 0, dy = 0;
    if (ARG(2)) { x0 = R32(ARG(2)); y0 = R32(ARG(2) + 4); x1 = R32(ARG(2) + 8); y1 = R32(ARG(2) + 12); }
    if (ARG(4)) { dx = R32(ARG(4)); dy = R32(ARG(4) + 4); }
    uint32_t blk = d3d_format_block(s->format) ? 4u : 1u;
    uint32_t unit = blk == 4 ? (s->format == FOURCC_DXT1 ? 8u : 16u) : d3d_format_bpp(s->format) / 8;
    uint32_t rows = (y1 - y0 + blk - 1) / blk, bytes = (x1 - x0 + blk - 1) / blk * unit;
    for (uint32_t r = 0; r < rows; r++) {
        uint32_t so = s->mem[0][0] + (y0 / blk + r) * s->pitch[0] + x0 / blk * unit;
        uint32_t dof = d->mem[0][0] + (dy / blk + r) * d->pitch[0] + dx / blk * unit;
        memmove(HOST(dof), HOST(so), bytes);
    }
    D3DObj *p = d3d_obj(d->parent);
    d->version++;
    if (p) p->version++;
    return 0;
}
/* ColorFill(surface, rect, color): system-memory and texture surfaces filled in memory (32-bit formats). */
static uint32_t ColorFill(CPU *c, uint32_t a) {
    (void)c;
    D3DObj *d = d3d_obj(ARG(1));
    static int once;
    if (!once++) fprintf(stderr, "usf: ColorFill used (format %u usage %x)\n", d ? d->format : 0, d ? d->usage : 0);
    if (!d || (d->usage & 3u) || d3d_format_bpp(d->format) != 32) return 0;   /* render targets: not yet */
    uint32_t x0 = 0, y0 = 0, x1 = d->width, y1 = d->height;
    if (ARG(2)) { x0 = R32(ARG(2)); y0 = R32(ARG(2) + 4); x1 = R32(ARG(2) + 8); y1 = R32(ARG(2) + 12); }
    for (uint32_t y = y0; y < y1; y++)
        for (uint32_t x = x0; x < x1; x++) W32(d->mem[0][0] + y * d->pitch[0] + x * 4, ARG(3));
    D3DObj *p = d3d_obj(d->parent);
    d->version++;
    if (p) p->version++;
    return 0;
}
static uint32_t UpdateTexture(CPU *c, uint32_t a) {
    (void)c;
    D3DObj *s = d3d_obj(ARG(1)), *d = d3d_obj(ARG(2));
    if (!s || !d) return BAD;
    int faces = d->kind == OBJ_CUBE ? 6 : 1;
    for (int f = 0; f < faces; f++)
        for (uint32_t l = 0; l < d->levels && l < s->levels; l++)
            if (s->size[l] == d->size[l]) memcpy(HOST(d->mem[f][l]), HOST(s->mem[f][l]), d->size[l]);
    d->version++;
    return 0;
}

HleFn vt_device[119] = {
    qi, addref, release, TestCooperativeLevel, GetAvailableTextureMem, ok, GetDirect3D, DevGetDeviceCaps, GetDisplayMode,
    GetCreationParameters, ok, ok, ok, ok, GetSwapChain, GetNumberOfSwapChains, Reset, Present, GetBackBuffer,
    GetRasterStatus, ok, ok, ok, CreateTexture, CreateVolumeTexture, CreateCubeTexture, CreateVertexBuffer,
    CreateIndexBuffer, CreateRenderTarget, CreateDepthStencilSurface, UpdateSurface, UpdateTexture, GetRenderTargetData, ok,
    StretchRect, ColorFill, CreateOffscreenPlainSurface, SetRenderTarget, GetRenderTarget, SetDepthStencilSurface,
    GetDepthStencilSurface, ok, ok, Clear, ok, ok, ok, SetViewport, GetViewport, ok, ok, ok, ok, ok, ok, ok, ok,
    SetRenderState, GetRenderState, CreateStateBlock, ok, EndStateBlock, ok, ok, GetTexture, SetTexture, ok, ok,
    GetSamplerState, SetSamplerState, ValidateDevice, ok, ok, ok, ok, SetScissorRect, GetScissorRect, ok, ok, ok, ok,
    DrawPrimitive, DrawIndexedPrimitive, DrawPrimitiveUP, DrawIndexedPrimitiveUP, ok, CreateVertexDeclaration,
    SetVertexDeclaration, GetVertexDeclaration, SetFVF, GetFVF, CreateVertexShader, SetVertexShader, GetVertexShader,
    SetVSConstF, GetVSConstF, SetVSConstI, ok, SetVSConstB, ok, SetStreamSource, GetStreamSource, SetStreamSourceFreq,
    ok, SetIndices, GetIndices, CreatePixelShader, SetPixelShader, GetPixelShader, SetPSConstF, GetPSConstF,
    SetPSConstI, ok, SetPSConstB, ok, ok, ok, ok, CreateQuery};

/* IDirect3DSwapChain9 */
static uint32_t sc_Present(CPU *c, uint32_t a) { (void)c; (void)a; r_present(); g_dev.frames++; return 0; }
static uint32_t sc_GetBackBuffer(CPU *c, uint32_t a) { (void)c; W32(ARG(3), g_dev.backbuffer); return 0; }
static uint32_t sc_GetDisplayMode(CPU *c, uint32_t a) { (void)c; W32(ARG(1), g_dev.bb_w); W32(ARG(1) + 4, g_dev.bb_h); W32(ARG(1) + 8, 60); W32(ARG(1) + 12, g_dev.bb_fmt); return 0; }
static uint32_t sc_GetDevice(CPU *c, uint32_t a) { (void)c; W32(ARG(1), g_device_guest); return 0; }
static uint32_t sc_GetPresentParameters(CPU *c, uint32_t a) {
    (void)c;
    memset(HOST(ARG(1)), 0, 56);
    W32(ARG(1), g_dev.bb_w); W32(ARG(1) + 4, g_dev.bb_h); W32(ARG(1) + 8, g_dev.bb_fmt); W32(ARG(1) + 12, 1); W32(ARG(1) + 32, 1);
    return 0;
}
HleFn vt_swapchain[10] = {qi, addref, release, sc_Present, ok, sc_GetBackBuffer, GetRasterStatus, sc_GetDisplayMode,
                          sc_GetDevice, sc_GetPresentParameters};
