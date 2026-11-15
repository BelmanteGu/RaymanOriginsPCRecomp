/* Direct3D 9 objects: creation, the IUnknown methods, formats, and the resource interfaces (textures, cube and
 * volume textures, surfaces, vertex/index buffers, shaders, vertex declarations, queries, state blocks).
 *
 * What it does: every object kind has a vtable of thunks built once (method index = position in the Windows
 * interface); Lock / LockRect return pointers into guest memory allocated at creation, Unlock bumps a version so the
 * renderer uploads again; GetLevelDesc / GetDesc fill the D3D structures; queries report "done" immediately.
 * What it does not do: the device and the IDirect3D9 interface (d3d9_device.c, d3d9_main.c) or any GPU work.
 */
#include <stdlib.h>
#include <string.h>

#include "d3d9.h"

#define MAXOBJ 65536
static D3DObj *objs[MAXOBJ];
static uint32_t nobj = 1;
static uint32_t vtables[16];

D3DObj *d3d_obj(uint32_t g) {
    if (!g || g >= 0xFFFF0000u) return NULL;
    uint32_t i = R32(g + 8);
    if (!i || i >= nobj || !objs[i] || objs[i]->guest != g) return NULL;
    return objs[i];
}

/* ---- formats ---- */
#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
int d3d_format_block(uint32_t f) {
    return f == FOURCC('D', 'X', 'T', '1') || f == FOURCC('D', 'X', 'T', '2') || f == FOURCC('D', 'X', 'T', '3') ||
           f == FOURCC('D', 'X', 'T', '4') || f == FOURCC('D', 'X', 'T', '5');
}
uint32_t d3d_format_bpp(uint32_t f) {
    if (f == FOURCC('D', 'X', 'T', '1')) return 4;
    if (d3d_format_block(f)) return 8;
    switch (f) {
    case 20: return 24;                                        /* R8G8B8 */
    case 21: case 22: case 32: case 33: case 34: case 35: case 75: case 77: case 71: case 114: case 112: return 32;
    case 23: case 24: case 25: case 26: case 29: case 30: case 51: case 60: case 80: case 81: case 111: return 16;
    case 28: case 50: case 41: return 8;
    case 36: case 113: case 64: return 64;                     /* A16B16G16R16, A16B16G16R16F, G32R32F */
    case 116: return 128;                                      /* A32B32G32R32F */
    case 101: return 16;                                       /* INDEX16 */
    case 102: return 32;                                       /* INDEX32 */
    default: return 32;                                        /* INTZ, DF24, RAWZ, NULL...: 32-bit */
    }
}
uint32_t d3d_pitch(uint32_t f, uint32_t w) {
    if (d3d_format_block(f)) return ((w + 3) / 4) * (f == FOURCC('D', 'X', 'T', '1') ? 8u : 16u);
    return (w * d3d_format_bpp(f) + 7) / 8;
}
uint32_t d3d_level_size(uint32_t f, uint32_t w, uint32_t h) {
    uint32_t rows = d3d_format_block(f) ? (h + 3) / 4 : h;
    return d3d_pitch(f, w) * (rows ? rows : 1);
}

/* ---- IUnknown / IDirect3DResource9 common methods ---- */
static uint32_t m_QueryInterface(CPU *c, uint32_t a) { (void)c; if (ARG(2)) W32(ARG(2), ARG(0)); return 0; }
static uint32_t m_AddRef(CPU *c, uint32_t a) { (void)c; D3DObj *o = d3d_obj(ARG(0)); return o ? ++o->refs : 1; }
static uint32_t m_Release(CPU *c, uint32_t a) {
    (void)c;
    D3DObj *o = d3d_obj(ARG(0));
    if (!o) return 0;
    if (o->refs) o->refs--;
    return o->refs;                                            /* objects are kept: the renderer may still use them */
}
extern uint32_t g_device_guest;
static uint32_t m_GetDevice(CPU *c, uint32_t a) { (void)c; W32(ARG(1), g_device_guest); return 0; }
static uint32_t m_ok(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
static uint32_t m_GetType(CPU *c, uint32_t a) {
    (void)c;
    D3DObj *o = d3d_obj(ARG(0));
    if (!o) return 0;
    switch (o->kind) { case OBJ_SURFACE: return 1; case OBJ_VOLUME: return 4; case OBJ_TEXTURE: return 3;
                       case OBJ_CUBE: return 5; case OBJ_VB: return 6; case OBJ_IB: return 7; default: return 0; }
}
static uint32_t m_GetLevelCount(CPU *c, uint32_t a) { (void)c; D3DObj *o = d3d_obj(ARG(0)); return o ? o->levels : 1; }

/* ---- textures ---- */
static void fill_surface_desc(uint32_t p, D3DObj *o, uint32_t w, uint32_t h) {
    W32(p, o->format); W32(p + 4, 1); W32(p + 8, o->usage); W32(p + 12, o->pool);
    W32(p + 16, 0); W32(p + 20, 0); W32(p + 24, w); W32(p + 28, h);
}
static uint32_t lw(D3DObj *o, uint32_t l) { uint32_t v = o->width >> l; return v ? v : 1; }
static uint32_t lh(D3DObj *o, uint32_t l) { uint32_t v = o->height >> l; return v ? v : 1; }

static uint32_t tex_GetLevelDesc(CPU *c, uint32_t a) {
    (void)c;
    D3DObj *o = d3d_obj(ARG(0));
    if (!o || ARG(1) >= o->levels) return 0x8876086Cu;        /* D3DERR_INVALIDCALL */
    fill_surface_desc(ARG(2), o, lw(o, ARG(1)), lh(o, ARG(1)));
    return 0;
}
static uint32_t get_level_surface(D3DObj *o, uint32_t face, uint32_t level) {
    return d3d_new_surface(o->format, lw(o, level), lh(o, level), o->usage, o->guest, face, level, o->mem[face][level]);
}
static uint32_t tex_GetSurfaceLevel(CPU *c, uint32_t a) {
    (void)c;
    D3DObj *o = d3d_obj(ARG(0));
    if (!o || ARG(1) >= o->levels) return 0x8876086Cu;
    W32(ARG(2), get_level_surface(o, 0, ARG(1)));
    return 0;
}
static uint32_t lock(D3DObj *o, uint32_t face, uint32_t level, uint32_t out, uint32_t rect) {
    uint32_t base = o->mem[face][level], pitch = o->pitch[level];
    if (rect) {
        uint32_t x = R32(rect), y = R32(rect + 4);
        if (d3d_format_block(o->format)) { x /= 4; y /= 4; base += y * pitch + x * (o->format == FOURCC('D', 'X', 'T', '1') ? 8u : 16u); }
        else base += y * pitch + x * d3d_format_bpp(o->format) / 8;
    }
    W32(out, pitch);
    W32(out + 4, base);
    return 0;
}
static uint32_t tex_LockRect(CPU *c, uint32_t a) {
    (void)c;
    D3DObj *o = d3d_obj(ARG(0));
    if (!o || ARG(1) >= o->levels) return 0x8876086Cu;
    if (o->format == 28 && ARG(3) && getenv("USF_TEXLOG"))
        fprintf(stderr, "lock A8 %08x rect %u,%u-%u,%u flags %x (from %08x)\n", ARG(0), R32(ARG(3)), R32(ARG(3) + 4),
                R32(ARG(3) + 8), R32(ARG(3) + 12), ARG(4), R32(a - 4));
    return lock(o, 0, ARG(1), ARG(2), ARG(3));
}
static uint32_t tex_UnlockRect(CPU *c, uint32_t a) { (void)c; D3DObj *o = d3d_obj(ARG(0)); if (o) o->version++; return 0; }

static uint32_t cube_GetCubeMapSurface(CPU *c, uint32_t a) {
    (void)c;
    D3DObj *o = d3d_obj(ARG(0));
    if (!o || ARG(1) > 5 || ARG(2) >= o->levels) return 0x8876086Cu;
    W32(ARG(3), get_level_surface(o, ARG(1), ARG(2)));
    return 0;
}
static uint32_t cube_LockRect(CPU *c, uint32_t a) {
    (void)c;
    D3DObj *o = d3d_obj(ARG(0));
    if (!o || ARG(1) > 5 || ARG(2) >= o->levels) return 0x8876086Cu;
    return lock(o, ARG(1), ARG(2), ARG(3), ARG(4));
}

/* ---- surfaces ---- */
static uint32_t surf_GetDesc(CPU *c, uint32_t a) { (void)c; D3DObj *o = d3d_obj(ARG(0)); if (o) fill_surface_desc(ARG(1), o, o->width, o->height); return 0; }
static uint32_t surf_LockRect(CPU *c, uint32_t a) {
    (void)c;
    D3DObj *o = d3d_obj(ARG(0));
    if (!o) return 0x8876086Cu;
    return lock(o, 0, 0, ARG(1), ARG(2));
}
static uint32_t surf_UnlockRect(CPU *c, uint32_t a) {
    (void)c;
    D3DObj *o = d3d_obj(ARG(0)), *p = o ? d3d_obj(o->parent) : NULL;
    if (o) o->version++;
    if (p) p->version++;
    return 0;
}
static uint32_t surf_GetContainer(CPU *c, uint32_t a) {
    (void)c;
    D3DObj *o = d3d_obj(ARG(0));
    if (!o || !o->parent) return 0x8876086Cu;
    W32(ARG(2), o->parent);
    return 0;
}

/* ---- buffers ---- */
static uint32_t buf_Lock(CPU *c, uint32_t a) {
    (void)c;
    D3DObj *o = d3d_obj(ARG(0));
    if (!o) return 0x8876086Cu;
    W32(ARG(3), o->mem[0][0] + ARG(1));
    return 0;
}
static uint32_t buf_Unlock(CPU *c, uint32_t a) { (void)c; D3DObj *o = d3d_obj(ARG(0)); if (o) o->version++; return 0; }
static uint32_t buf_GetDesc(CPU *c, uint32_t a) {
    (void)c;
    D3DObj *o = d3d_obj(ARG(0));
    if (!o) return 0x8876086Cu;
    W32(ARG(1), o->format); W32(ARG(1) + 4, o->kind == OBJ_VB ? 6u : 7u); W32(ARG(1) + 8, o->usage);
    W32(ARG(1) + 12, o->pool); W32(ARG(1) + 16, o->width); W32(ARG(1) + 20, o->format);
    return 0;
}

/* ---- shaders, declarations, queries ---- */
static uint32_t code_Get(CPU *c, uint32_t a) {
    (void)c;
    D3DObj *o = d3d_obj(ARG(0));
    if (!o) return 0x8876086Cu;
    if (ARG(1)) memcpy(HOST(ARG(1)), HOST(o->code), o->code_size);
    W32(ARG(2), o->code_size);
    return 0;
}
static uint32_t query_GetType(CPU *c, uint32_t a) { (void)c; D3DObj *o = d3d_obj(ARG(0)); return o ? o->query_type : 0; }
static uint32_t query_GetDataSize(CPU *c, uint32_t a) {
    (void)c;
    D3DObj *o = d3d_obj(ARG(0));
    if (!o) return 0;
    return o->query_type == 8 ? 8u : 4u;                       /* EVENT 8 -> BOOL; OCCLUSION 9 -> DWORD */
}
static uint32_t query_GetData(CPU *c, uint32_t a) {
    (void)c;
    D3DObj *o = d3d_obj(ARG(0));
    if (ARG(1) && ARG(2)) {
        memset(HOST(ARG(1)), 0, ARG(2));
        if (o && o->query_type == 8) W32(ARG(1), 1);           /* event: done */
        else if (ARG(2) >= 4) W32(ARG(1), 1000);               /* occlusion: visible */
    }
    return 0;                                                  /* S_OK: data is ready */
}

#define RES_COMMON [0] = m_QueryInterface, [1] = m_AddRef, [2] = m_Release, [3] = m_GetDevice, [4] = m_ok, \
                   [5] = m_ok, [6] = m_ok, [7] = m_ok, [8] = m_ok, [9] = m_ok, [10] = m_GetType
static HleFn vt_texture[22] = {RES_COMMON, [11] = m_ok, [12] = m_ok, [13] = m_GetLevelCount, [14] = m_ok, [15] = m_ok,
                               [16] = m_ok, [17] = tex_GetLevelDesc, [18] = tex_GetSurfaceLevel, [19] = tex_LockRect,
                               [20] = tex_UnlockRect, [21] = m_ok};
static HleFn vt_cube[22] = {RES_COMMON, [11] = m_ok, [12] = m_ok, [13] = m_GetLevelCount, [14] = m_ok, [15] = m_ok,
                            [16] = m_ok, [17] = tex_GetLevelDesc, [18] = cube_GetCubeMapSurface, [19] = cube_LockRect,
                            [20] = tex_UnlockRect, [21] = m_ok};
static HleFn vt_surface[17] = {RES_COMMON, [11] = surf_GetContainer, [12] = surf_GetDesc, [13] = surf_LockRect,
                               [14] = surf_UnlockRect, [15] = m_ok, [16] = m_ok};
static HleFn vt_buffer[14] = {RES_COMMON, [11] = buf_Lock, [12] = buf_Unlock, [13] = buf_GetDesc};
static HleFn vt_code[5] = {m_QueryInterface, m_AddRef, m_Release, m_GetDevice, code_Get};
static HleFn vt_query[8] = {m_QueryInterface, m_AddRef, m_Release, m_GetDevice, query_GetType, query_GetDataSize,
                            m_ok, query_GetData};
static HleFn vt_stateblock[6] = {m_QueryInterface, m_AddRef, m_Release, m_GetDevice, m_ok, m_ok};
extern HleFn vt_device[119], vt_d3d9[17], vt_swapchain[10];

/* Argument dwords of every method (`this` included), from the Windows SDK d3d9.h */
static const uint8_t na_d3d9[17] = {3, 1, 1, 2, 1, 4, 3, 5, 3, 6, 7, 7, 6, 4, 4, 2, 7};
static const uint8_t na_device[119] = {
    3, 1, 1, 1, 1, 1, 2, 2, 3, 2, 4, 4, 2, 3, 3, 1, 2, 5, 5, 3, 2, 4, 3, 9, 10, 8, 7, 7, 9, 9, 5, 3, 3, 3, 6, 4, 7,
    3, 3, 2, 2, 1, 1, 7, 3, 3, 3, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 1, 2, 2, 2, 3, 3, 4, 4, 4, 4, 2, 3, 3, 2,
    2, 2, 2, 2, 1, 2, 1, 4, 7, 5, 9, 7, 3, 2, 2, 2, 2, 3, 2, 2, 4, 4, 4, 4, 4, 4, 5, 5, 3, 3, 2, 2, 3, 2, 2, 4, 4,
    4, 4, 4, 4, 4, 4, 2, 3};
#define NA_RES 3, 1, 1, 2, 5, 4, 2, 2, 1, 1, 1
static const uint8_t na_texture[22] = {NA_RES, 2, 1, 1, 2, 1, 1, 3, 3, 5, 2, 2};
static const uint8_t na_cube[22] = {NA_RES, 2, 1, 1, 2, 1, 1, 3, 4, 6, 3, 3};
static const uint8_t na_surface[17] = {NA_RES, 3, 2, 4, 1, 2, 2};
static const uint8_t na_buffer[14] = {NA_RES, 5, 1, 2};
static const uint8_t na_code[5] = {3, 1, 1, 2, 3};
static const uint8_t na_query[8] = {3, 1, 1, 2, 1, 1, 2, 4};
static const uint8_t na_stateblock[6] = {3, 1, 1, 2, 1, 1};
static const uint8_t na_swapchain[10] = {3, 1, 1, 6, 2, 4, 2, 2, 2, 2};

static uint32_t vtable_of(int kind) {
    if (vtables[kind]) return vtables[kind];
    static const char *names[] = {"", "IDirect3D9", "IDirect3DDevice9", "IDirect3DTexture9", "IDirect3DCubeTexture9",
                                  "IDirect3DVolumeTexture9", "IDirect3DSurface9", "IDirect3DVertexBuffer9",
                                  "IDirect3DIndexBuffer9", "IDirect3DVertexShader9", "IDirect3DPixelShader9",
                                  "IDirect3DVertexDeclaration9", "IDirect3DQuery9", "IDirect3DStateBlock9",
                                  "IDirect3DSwapChain9"};
    HleFn *t;
    const uint8_t *na;
    int n;
    switch (kind) {
    case OBJ_D3D9: t = vt_d3d9; na = na_d3d9; n = 17; break;
    case OBJ_DEVICE: t = vt_device; na = na_device; n = 119; break;
    case OBJ_TEXTURE: case OBJ_VOLUME: t = vt_texture; na = na_texture; n = 22; break;
    case OBJ_CUBE: t = vt_cube; na = na_cube; n = 22; break;
    case OBJ_SURFACE: t = vt_surface; na = na_surface; n = 17; break;
    case OBJ_VB: case OBJ_IB: t = vt_buffer; na = na_buffer; n = 14; break;
    case OBJ_VS: case OBJ_PS: case OBJ_DECL: t = vt_code; na = na_code; n = 5; break;
    case OBJ_QUERY: t = vt_query; na = na_query; n = 8; break;
    case OBJ_SWAPCHAIN: t = vt_swapchain; na = na_swapchain; n = 10; break;
    default: t = vt_stateblock; na = na_stateblock; n = 6; break;
    }
    uint32_t tmp = obj_new_n(names[kind], n, t, na, 16);             /* obj_new builds the vtable; keep only it */
    vtables[kind] = R32(tmp);
    return vtables[kind];
}

uint32_t d3d_new(int kind, D3DObj **out) {
    if (nobj >= MAXOBJ) hle_fatal("too many Direct3D objects");
    D3DObj *o = calloc(1, sizeof *o);
    o->kind = kind;
    o->refs = 1;
    uint32_t g = gcalloc(32);
    W32(g, vtable_of(kind));
    W32(g + 8, nobj);
    o->guest = g;
    objs[nobj++] = o;
    if (out) *out = o;
    return g;
}

uint32_t d3d_new_surface(uint32_t fmt, uint32_t w, uint32_t h, uint32_t usage, uint32_t parent, uint32_t face,
                         uint32_t level, uint32_t mem) {
    D3DObj *o;
    uint32_t g = d3d_new(OBJ_SURFACE, &o);
    o->format = fmt; o->width = w; o->height = h; o->usage = usage; o->levels = 1;
    o->parent = parent; o->face = face; o->level = level;
    o->pitch[0] = d3d_pitch(fmt, w);
    o->size[0] = d3d_level_size(fmt, w, h);
    o->mem[0][0] = mem ? mem : gcalloc(o->size[0]);
    return g;
}

/* A stable name for a shader: FNV-1a of its bytecode (object numbers change from run to run). Cached in the object. */
uint32_t d3d_shader_hash(uint32_t guest) {
    D3DObj *o = d3d_obj(guest);
    if (!o || !o->code) return 0;
    if (o->query_type) return o->query_type;                 /* (shaders do not use query_type: the cache) */
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < o->code_size; i++) { h ^= R8(o->code + i); h *= 16777619u; }
    o->query_type = h ? h : 1;
    return o->query_type;
}
