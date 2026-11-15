/* The D3DX 9 functions the game imports: textures from DDS files in memory and surface loads (the shader assembler
 * is in d3dx9_asm.c).
 *
 * What it does: parses a DDS header (DXT1/3/5, the FourCC formats that are D3DFMT numbers, 8/16/32-bit RGB(A),
 * luminance, alpha), creates the texture or cube texture with the file's size, format and mip count, and copies every
 * level into guest memory. D3DXLoadSurfaceFromMemory copies a rectangle, converting between the uncompressed
 * 8/16/24/32-bit formats (Scaleform sends its decoded images as A8B8G8R8 into A8R8G8B8 textures).
 * What it does not do: other image formats (PNG/JPG/TGA/BMP fail with D3DXERR_INVALIDDATA), compressed-format conversion or
 * resizing (the file's own format and size are used).
 */
#include <stdlib.h>
#include <string.h>

#include "d3d9.h"

#define D3DXERR_INVALIDDATA 0x88760B59u
#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

static uint32_t dds_format(uint32_t h) {          /* h: guest address of the DDS header (after "DDS ") */
    uint32_t pf = h + 72, flags = R32(pf + 4), fourcc = R32(pf + 8), bits = R32(pf + 12);
    uint32_t rm = R32(pf + 16), gm = R32(pf + 20), bm = R32(pf + 24), am = R32(pf + 28);
    if (flags & 4u) return fourcc;                                  /* DDPF_FOURCC: DXTn, or a D3DFMT number */
    if (bits == 32) {
        if (rm == 0x00FF0000u) return am ? 21u : 22u;               /* A8R8G8B8 / X8R8G8B8 */
        if (rm == 0x000000FFu) return am ? 32u : 33u;               /* A8B8G8R8 / X8B8G8R8 */
        if (rm == 0x0000FFFFu && gm == 0xFFFF0000u) return 34u;     /* G16R16 */
        return 21u;
    }
    if (bits == 24) return 20u;
    if (bits == 16) {
        if (rm == 0xF800u) return 23u;                              /* R5G6B5 */
        if (rm == 0x7C00u) return am ? 25u : 24u;                   /* A1R5G5B5 / X1R5G5B5 */
        if (rm == 0x0F00u) return 26u;                              /* A4R4G4B4 */
        if (rm == 0x00FFu && am == 0xFF00u) return 51u;             /* A8L8 */
        return 23u;
    }
    if (bits == 8) return (flags & 2u) && !rm ? 28u : 50u;          /* A8 / L8 */
    (void)bm;
    return 0;
}

static uint32_t from_dds(uint32_t src, uint32_t size, uint32_t want_levels, uint32_t usage, uint32_t pool,
                         uint32_t info, uint32_t out, int cube) {
    if (size < 128 || R32(src) != FOURCC('D', 'D', 'S', ' ')) return D3DXERR_INVALIDDATA;
    uint32_t h = src + 4;
    uint32_t height = R32(h + 8), width = R32(h + 12), mips = R32(h + 24), caps2 = R32(h + 108);
    uint32_t fmt = dds_format(h);
    if (!fmt || !width || !height) return D3DXERR_INVALIDDATA;
    if (!mips) mips = 1;
    if (getenv("USF_TEXLOG")) fprintf(stderr, "DDS %ux%u mips %u fmt %u flags %x pitch %u bits %u size %u\n", width, height, mips, fmt, R32(h + 4), R32(h + 16),
            R32(h + 72 + 12), size);
    uint32_t levels = mips;
    if (want_levels && want_levels < 0xFFFFFFF0u && want_levels < levels) levels = want_levels;
    int is_cube = (caps2 & 0x200u) != 0;
    if (cube && !is_cube) return D3DXERR_INVALIDDATA;
    uint32_t g = d3d_create_texture(is_cube ? OBJ_CUBE : OBJ_TEXTURE, width, height, levels, usage, fmt, pool);
    D3DObj *o = d3d_obj(g);
    uint32_t p = src + 128, end = src + size;
    int faces = is_cube ? 6 : 1;
    for (int f = 0; f < faces; f++)
        for (uint32_t l = 0; l < mips; l++) {
            uint32_t lw = width >> l ? width >> l : 1, lh = height >> l ? height >> l : 1;
            uint32_t n = d3d_level_size(fmt, lw, lh);
            if (p + n > end) break;
            if (l < levels) memcpy(HOST(o->mem[f][l]), HOST(p), n);
            p += n;
        }
    o->version++;
    if (info) {
        W32(info, width); W32(info + 4, height); W32(info + 8, 1); W32(info + 12, mips); W32(info + 16, fmt);
        W32(info + 20, is_cube ? 5u : 3u); W32(info + 24, 4u);           /* D3DXIFF_DDS */
    }
    W32(out, g);
    return 0;
}

/* (device, data, size, w, h, mips, usage, format, pool, filter, mipfilter, colorkey, info, palette, out) */
uint32_t x_D3DXCreateTextureFromFileInMemoryEx(CPU *c, uint32_t a) {
    (void)c;
    if (getenv("USF_TEXLOG")) fprintf(stderr, "D3DXCreateTextureFromFileInMemoryEx w %x h %x mips %x fmt %x pool %u filter %x mipfilter %x\n",
                                      ARG(3), ARG(4), ARG(5), ARG(7), ARG(8), ARG(9), ARG(10));
    const char *dump = getenv("USF_TEXDUMP");                       /* dir: keep every DDS the game loads (private/) */
    if (dump && ARG(2) < (64u << 20)) {
        char fn[600];
        static unsigned n;
        snprintf(fn, sizeof fn, "%s/%05u_%ux%u.dds", dump, n++, R32(ARG(1) + 16), R32(ARG(1) + 12));
        FILE *f = fopen(fn, "wb");
        if (f) { fwrite(HOST(ARG(1)), 1, ARG(2), f); fclose(f); }
    }
    uint32_t r = from_dds(ARG(1), ARG(2), ARG(5), ARG(6), ARG(8), ARG(12), ARG(14), 0);
    if (r) usf_log("D3DXCreateTextureFromFileInMemoryEx: not a DDS we read (%08x...)\n", R32(ARG(1)));
    return r;
}

/* (device, data, size, edge, mips, usage, format, pool, filter, mipfilter, colorkey, info, palette, out) */
uint32_t x_D3DXCreateCubeTextureFromFileInMemoryEx(CPU *c, uint32_t a) {
    (void)c;
    return from_dds(ARG(1), ARG(2), ARG(4), ARG(5), ARG(7), ARG(11), ARG(13), 1);
}

uint32_t x_D3DXCreateTextureFromFileExA(CPU *c, uint32_t a) {
    (void)c;
    usf_log("D3DXCreateTextureFromFileExA(%s): not supported\n", STR(ARG(1)));
    return D3DXERR_INVALIDDATA;
}

/* One pixel of an uncompressed D3D format to RGBA8 and back; 0 if the format is not handled. */
static int px_get(uint32_t f, const uint8_t *p, uint8_t o[4]) {
    uint32_t v;
    switch (f) {
    case 21: case 22: o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = f == 21 ? p[3] : 255; return 1;   /* A8R8G8B8, X8R8G8B8 */
    case 32: case 33: o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = f == 32 ? p[3] : 255; return 1;   /* A8B8G8R8, X8B8G8R8 */
    case 20: o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = 255; return 1;                            /* R8G8B8 */
    case 28: o[0] = o[1] = o[2] = 0; o[3] = p[0]; return 1;                                         /* A8 */
    case 50: o[0] = o[1] = o[2] = p[0]; o[3] = 255; return 1;                                       /* L8 */
    case 51: o[0] = o[1] = o[2] = p[0]; o[3] = p[1]; return 1;                                      /* A8L8 */
    case 23: v = p[0] | p[1] << 8; o[0] = (uint8_t)((v >> 11) * 255 / 31); o[1] = (uint8_t)((v >> 5 & 63) * 255 / 63);
        o[2] = (uint8_t)((v & 31) * 255 / 31); o[3] = 255; return 1;                                  /* R5G6B5 */
    case 25: v = p[0] | p[1] << 8; o[0] = (uint8_t)((v >> 10 & 31) * 255 / 31); o[1] = (uint8_t)((v >> 5 & 31) * 255 / 31);
        o[2] = (uint8_t)((v & 31) * 255 / 31); o[3] = v >> 15 ? 255 : 0; return 1;                    /* A1R5G5B5 */
    case 26: v = p[0] | p[1] << 8; o[0] = (uint8_t)((v >> 8 & 15) * 17); o[1] = (uint8_t)((v >> 4 & 15) * 17);
        o[2] = (uint8_t)((v & 15) * 17); o[3] = (uint8_t)((v >> 12) * 17); return 1;                  /* A4R4G4B4 */
    }
    return 0;
}
static int px_put(uint32_t f, uint8_t *p, const uint8_t c[4]) {
    uint32_t v;
    switch (f) {
    case 21: case 22: p[0] = c[2]; p[1] = c[1]; p[2] = c[0]; p[3] = f == 21 ? c[3] : 255; return 1;
    case 32: case 33: p[0] = c[0]; p[1] = c[1]; p[2] = c[2]; p[3] = f == 32 ? c[3] : 255; return 1;
    case 20: p[0] = c[2]; p[1] = c[1]; p[2] = c[0]; return 1;
    case 28: p[0] = c[3]; return 1;
    case 50: p[0] = (uint8_t)((c[0] * 77 + c[1] * 150 + c[2] * 29) >> 8); return 1;
    case 51: p[0] = (uint8_t)((c[0] * 77 + c[1] * 150 + c[2] * 29) >> 8); p[1] = c[3]; return 1;
    case 23: v = (uint32_t)(c[0] >> 3) << 11 | (uint32_t)(c[1] >> 2) << 5 | c[2] >> 3; break;
    case 25: v = (uint32_t)(c[3] >= 128) << 15 | (uint32_t)(c[0] >> 3) << 10 | (uint32_t)(c[1] >> 3) << 5 | c[2] >> 3; break;
    case 26: v = (uint32_t)(c[3] >> 4) << 12 | (uint32_t)(c[0] >> 4) << 8 | (uint32_t)(c[1] >> 4) << 4 | c[2] >> 4; break;
    default: return 0;
    }
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    return 1;
}

/* (dest surface, dest palette, dest rect, src memory, src format, src pitch, src palette, src rect, filter, key)
 * Copies the source rectangle to the destination rectangle's corner (no scaling); converts between the formats above. */
uint32_t x_D3DXLoadSurfaceFromMemory(CPU *c, uint32_t a) {
    (void)c;
    D3DObj *d = d3d_obj(ARG(0));
    if (!d) return 0x8876086Cu;
    uint32_t sfmt = ARG(4), spitch = ARG(5), drect = ARG(2), rect = ARG(7);
    uint32_t sx = rect ? R32(rect) : 0, sy = rect ? R32(rect + 4) : 0;
    uint32_t w = rect ? R32(rect + 8) - sx : d->width, h = rect ? R32(rect + 12) - sy : d->height;
    uint32_t dx = drect ? R32(drect) : 0, dy = drect ? R32(drect + 4) : 0;
    if (dx + w > d->width) w = d->width > dx ? d->width - dx : 0;
    if (dy + h > d->height) h = d->height > dy ? d->height - dy : 0;
    uint32_t sbpp = d3d_format_bpp(sfmt) / 8, dbpp = d3d_format_bpp(d->format) / 8;
    if (sfmt == d->format) {
        int blk = d3d_format_block(sfmt);
        uint32_t rows = blk ? (h + 3) / 4 : h, row = d3d_pitch(sfmt, w);
        uint32_t soff = blk ? sy / 4 * spitch + d3d_pitch(sfmt, sx) : sy * spitch + sx * sbpp;
        uint32_t doff = blk ? dy / 4 * d->pitch[0] + d3d_pitch(sfmt, dx) : dy * d->pitch[0] + dx * dbpp;
        for (uint32_t y = 0; y < rows && doff + y * d->pitch[0] < d->size[0]; y++)
            memcpy(HOST(d->mem[0][0] + doff + y * d->pitch[0]), HOST(ARG(3) + soff + y * spitch), row);
    } else {
        uint8_t px[4];
        if (d3d_format_block(sfmt) || d3d_format_block(d->format) || !px_get(sfmt, (const uint8_t *)"\0\0\0\0", px) ||
            !px_put(d->format, px, px)) {
            usf_log("D3DXLoadSurfaceFromMemory: format %u -> %u not converted\n", sfmt, d->format);
            return 0;
        }
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *sp = HOST(ARG(3) + (sy + y) * spitch + sx * sbpp);
            uint8_t *dp = HOST(d->mem[0][0] + (dy + y) * d->pitch[0] + dx * dbpp);
            for (uint32_t x = 0; x < w; x++) { px_get(sfmt, sp + x * sbpp, px); px_put(d->format, dp + x * dbpp, px); }
        }
    }
    d->version++;
    D3DObj *p = d3d_obj(d->parent);
    if (p) p->version++;
    return 0;
}

uint32_t *d3d_assemble(const char *src, uint32_t len, uint32_t *ntok, char *err, size_t errn);

/* ID3DXBuffer holding the bytecode: QueryInterface, AddRef, Release, GetBufferPointer, GetBufferSize */
static uint32_t xb_ok(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
static uint32_t xb_ptr(CPU *c, uint32_t a) { (void)c; return R32(ARG(0) + 8); }
static uint32_t xb_size(CPU *c, uint32_t a) { (void)c; return R32(ARG(0) + 12); }
static HleFn vt_xbuf[5] = {xb_ok, xb_ok, xb_ok, xb_ptr, xb_size};
static const uint8_t na_xbuf[5] = {3, 1, 1, 1, 1};
static uint32_t vt_xbuf_guest;

/* (src, len, defines, include, flags, ppShader, ppErrors) */
uint32_t x_D3DXAssembleShader(CPU *c, uint32_t a) {
    (void)c;
    char err[160] = "";
    uint32_t ntok = 0;
    uint32_t *tok = d3d_assemble((const char *)HOST(ARG(0)), ARG(1), &ntok, err, sizeof err);
    if (ARG(6)) W32(ARG(6), 0);
    if (!tok) {
        fprintf(stderr, "usf: D3DXAssembleShader failed: %s\n--- source ---\n%.*s\n---\n", err, (int)ARG(1), (const char *)HOST(ARG(0)));
        if (ARG(5)) W32(ARG(5), 0);
        return 0x80004005u;
    }
    if (!vt_xbuf_guest) vt_xbuf_guest = R32(obj_new_n("ID3DXBuffer", 5, vt_xbuf, na_xbuf, 16));
    uint32_t data = gmalloc(ntok * 4), b = gcalloc(16);
    memcpy(HOST(data), tok, ntok * 4);
    free(tok);
    W32(b, vt_xbuf_guest);
    W32(b + 8, data);
    W32(b + 12, ntok * 4);
    if (ARG(5)) W32(ARG(5), b);
    return 0;
}

/* version.dll / XInput battery: "not available" */
uint32_t x_XInputGetBatteryInformation(CPU *c, uint32_t a) { (void)c; (void)a; return 1167; }   /* not connected */
