/* (analysis) USF_GEO_DUMP=<file>, USF_GEO_TEX=#<hash>,...: the geometry of the draws that sample one of these textures,
 * frame by frame, so an animation the engine computes (a skeleton moving the pieces of an atlas: the title logo, the
 * menu's bars) can be played back elsewhere from the atlas alone (tools/launcher_mock.py), without recording pictures.
 *
 * What it does: for each such draw, one record: "GEO" frame, texture hash, primitive type, vertex count, index count,
 * stride, vertex-shader constants c0..c211 (matrices; the patch shader's control points from c17, colours from c145), then the vertices (stride bytes each) and the
 * indices (16-bit) as the game wrote them (the bridge's guest copies of the buffers).
 * What it does not do: draws from user pointers (DrawPrimitiveUP), more than stream 0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d9.h"
#include "dxvk_bridge.h"

#define NVSC 212
static float vsc[NVSC][4];                               /* vertex-shader constants c0..c211 as last set */

void dxb_geo_consts(uint32_t start, uint32_t data, uint32_t count) {
    for (uint32_t i = 0; i < count; i++)
        if (start + i < NVSC) memcpy(vsc[start + i], HOST(data + 16 * i), 16);
}

/* slot 81: (type, start vertex, prims); 82: (type, base vertex, min index, num vertices, start index, prims) */
void dxb_geo_draw(int indexed, uint32_t type, int32_t base, uint32_t first, uint32_t nverts, uint32_t start_index,
                  uint32_t prims, uint32_t tex_hash) {
    static FILE *f;
    if (!f) f = fopen(getenv("USF_GEO_DUMP"), "wb");
    Wrap *vb = dxb_of(stream_buf[0]);
    if (!f || !vb || !vb->shadow) return;
    uint32_t stride = stream_stride[0], off = stream_off[0];
    uint32_t nidx = type == 4 ? prims * 3 : type == 5 ? prims + 2 : prims * 2;
    uint16_t *idx = NULL;
    uint32_t vlo = first, vn = nverts;
    if (indexed) {
        Wrap *ib = dxb_of(cur_ib);
        if (!ib || !ib->shadow) return;
        idx = malloc(2u * nidx);
        memcpy(idx, HOST(ib->shadow + 2u * start_index), 2u * nidx);     /* (the game's indices are 16-bit) */
        vlo = (uint32_t)(base + (int32_t)first);
    } else vn = nidx;
    uint32_t hdr[7] = {0x4F4547u, g_dev.frames, tex_hash, type, vn, indexed ? nidx : 0, stride};
    fwrite(hdr, 4, 7, f);
    fwrite(vsc, 16, NVSC, f);
    uint32_t va = vb->shadow + off + vlo * stride;
    if (va + vn * stride <= vb->shadow + vb->shadow_size) fwrite(HOST(va), stride, vn, f);
    else { void *z = calloc(vn, stride); fwrite(z, stride, vn, f); free(z); }
    if (idx) {
        for (uint32_t i = 0; i < nidx; i++) idx[i] = (uint16_t)(idx[i] - first);   /* relative to the vertices written */
        fwrite(idx, 2, nidx, f);
        free(idx);
    }
}
