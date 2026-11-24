/* D3DXCreate{,Cube,Volume}TextureFromFileInMemoryEx for the DXVK bridge: the game's DDS files become DXVK textures.
 *
 * What it does: reads the DDS header (the same formats as d3dx9.c), creates the texture through DXVK's device
 * (CreateTexture / CreateCubeTexture / CreateVolumeTexture with the game's usage and pool) and copies every level
 * (and face, and depth slice) row by row into DXVK's locked memory, whose pitch may differ from the file's.
 * What it does not do: other image formats, resizing or format conversion (the file's size and format are used).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d9.h"
#include "dxvk_bridge.h"

#define D3DXERR_INVALIDDATA 0x88760B59u
#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
uint32_t d3dx_dds_format(uint32_t h);                    /* d3dx9.c */

static int is_block(uint32_t f) { return f == FOURCC('D', 'X', 'T', '1') || f == FOURCC('D', 'X', 'T', '2') || f == FOURCC('D', 'X', 'T', '3') || f == FOURCC('D', 'X', 'T', '4') || f == FOURCC('D', 'X', 'T', '5'); }

/* copy one level (rows of the file, packed) into a locked level (rows `pitch` apart) */
static void copy_rows(uint8_t *dst, uint32_t pitch, uint32_t src, uint32_t fmt, uint32_t w, uint32_t h, uint32_t slices,
                      uint32_t slice_pitch) {
    uint32_t rows = is_block(fmt) ? (h + 3) / 4 : h, size = d3d_level_size(fmt, w, h), row = rows ? size / rows : 0;
    for (uint32_t z = 0; z < slices; z++)
        for (uint32_t y = 0; y < rows; y++)
            memcpy(dst + (size_t)z * slice_pitch + (size_t)y * pitch, HOST(src + z * size + y * row), row);
}

/* kind: 2 texture, 3 cube, 4 volume. levels/usage/pool as the game asked; info: D3DXIMAGE_INFO or 0. */
uint32_t dxb_d3dx_texture(CPU *c, uint32_t device, uint32_t src, uint32_t size, uint32_t want, uint32_t usage,
                          uint32_t pool, uint32_t info, uint32_t out, int kind) {
    (void)c;
    void *dev = dxb_nat(device);
    if (!dev || size < 128 || R32(src) != FOURCC('D', 'D', 'S', ' ')) return D3DXERR_INVALIDDATA;
    uint32_t h = src + 4;
    uint32_t height = R32(h + 8), width = R32(h + 12), depth = R32(h + 20), mips = R32(h + 24), caps2 = R32(h + 108);
    uint32_t fmt = d3dx_dds_format(h);
    if (!fmt || !width || !height) return D3DXERR_INVALIDDATA;
    if (!mips) mips = 1;
    if (getenv("USF_TEXLOG")) fprintf(stderr, "usf: dds %ux%u mips %u fmt %08x\n", width, height, mips, fmt);
    int is_cube = (caps2 & 0x200u) != 0, is_vol = (caps2 & 0x200000u) != 0;
    if ((kind == 3) != is_cube) return D3DXERR_INVALIDDATA;
    if (!is_vol || !depth) depth = 1;
    uint32_t levels = mips;
    if (want && want < 0xFFFFFFF0u && want < levels) levels = want;
    void *tex = NULL;
    int32_t r;
    uint32_t bc = kind != 4 && dxe_on() && dxe_block(fmt) ? fmt : 0, mk = bc ? 32u : fmt;   /* (dxvk_bcemu.c) */
    if (kind == 4) r = (int32_t)dxb_method(dev, 24)(dev, width, height, depth, levels, usage, fmt, pool, (uint64_t)(uintptr_t)&tex, 0, 0, 0);
    else if (kind == 3) r = (int32_t)dxb_method(dev, 25)(dev, width, levels, usage, mk, pool, (uint64_t)(uintptr_t)&tex, 0, 0, 0, 0, 0);
    else r = (int32_t)dxb_method(dev, 23)(dev, width, height, levels, usage, mk, pool, (uint64_t)(uintptr_t)&tex, 0, 0, 0, 0);
    if (r < 0 || !tex) return r < 0 ? (uint32_t)r : D3DXERR_INVALIDDATA;
    uint32_t p = src + 128, end = src + size;
    int faces = is_cube ? 6 : 1;
    for (int f = 0; f < faces; f++)
        for (uint32_t l = 0; l < mips; l++) {
            uint32_t lw = width >> l ? width >> l : 1, lh = height >> l ? height >> l : 1, ld = depth >> l ? depth >> l : 1;
            uint32_t n = d3d_level_size(fmt, lw, lh) * ld;
            if (p + n > end) break;
            if (l < levels) {
                if (kind == 4) {
                    struct { int32_t row, slice; uint8_t *bits; } lb = {0};
                    if ((int32_t)dxb_method(tex, 19)(tex, l, (uint64_t)(uintptr_t)&lb, 0, 0, 0, 0, 0, 0, 0, 0, 0) >= 0) {
                        copy_rows(lb.bits, (uint32_t)lb.row, p, fmt, lw, lh, ld, (uint32_t)lb.slice);
                        dxb_method(tex, 20)(tex, l, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
                    }
                } else {
                    struct { int32_t pitch, pad; uint8_t *bits; } lr = {0};
                    int32_t lk = kind == 3
                        ? (int32_t)dxb_method(tex, 19)(tex, (uint64_t)f, l, (uint64_t)(uintptr_t)&lr, 0, 0, 0, 0, 0, 0, 0, 0)
                        : (int32_t)dxb_method(tex, 19)(tex, l, (uint64_t)(uintptr_t)&lr, 0, 0, 0, 0, 0, 0, 0, 0, 0);
                    if (lk >= 0) {
                        if (bc) dxe_decode(fmt, HOST(p), (lw + 3) / 4 * (fmt == 0x31545844u ? 8u : 16u), (lh + 3) / 4, lr.bits, (uint32_t)lr.pitch, lw, lh);
                        else copy_rows(lr.bits, (uint32_t)lr.pitch, p, fmt, lw, lh, 1, 0);
                        if (kind == 3) dxb_method(tex, 20)(tex, (uint64_t)f, l, 0, 0, 0, 0, 0, 0, 0, 0, 0);
                        else dxb_method(tex, 20)(tex, l, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
                    }
                }
            }
            p += n;
        }
    if (info) {
        W32(info, width); W32(info + 4, height); W32(info + 8, depth); W32(info + 12, mips); W32(info + 16, fmt);
        W32(info + 20, kind == 4 ? 4u : kind == 3 ? 5u : 3u); W32(info + 24, 4u);   /* resource type, D3DXIFF_DDS */
    }
    W32(out, dxb_wrap(tex, kind == 4 ? DXB_VOLTEX : kind == 3 ? DXB_CUBE : DXB_TEXTURE));
    Wrap *w = dxb_of(R32(out));
    if (w) w->bcfmt = bc;
    if (w && size > 128) {                                /* (analysis) content hash of the first 4 KB after the header */
        uint32_t h = 2166136261u, n = size - 128 < 4096 ? size - 128 : 4096;
        const uint8_t *b = HOST(src + 128);
        for (uint32_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
        w->code_hash = h;
    }
    return 0;
}
