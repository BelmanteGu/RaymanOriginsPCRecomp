/* DXT textures on GPUs without BC formats, for the DXVK bridge (see dxvk_bridge.h).
 *
 * Why: the game's textures are DXT1/3/5. Many phone GPUs (the Redmi 10C's Adreno 610, for one) have no BC formats in
 * Vulkan: DXVK cannot make the texture (D3DERR_OUTOFVIDEOMEMORY) and the game crashes filling the one it did not get.
 * What it does: when DXVK's IDirect3D9 says DXT1 textures are not available (or USF_BC_EMU=1; USF_BC_EMU=0 never),
 * a DXT texture or surface is made as A8B8G8R8 (bytes R G B A) and its wrapper remembers the DXT format: the game
 * still sees DXT (GetLevelDesc / GetDesc, CheckDeviceFormat, its lock's pitch and its guest copy), and each Unlock
 * decodes the locked rows of blocks into DXVK's level (vk_bcdec.c). Textures from DDS files (dxvk_d3dx.c) likewise.
 * What it does not do: DXT volume textures; re-compressing to a phone format (the level takes 4x / 8x the memory).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d9.h"
#include "dxvk_bridge.h"

#define A8B8G8R8 32u
void bc_decode(int kind, const uint8_t *src, uint8_t *dst, uint32_t w, uint32_t h);   /* vk/vk_bcdec.c */

static int emu = -1;
/* once DXVK's IDirect3D9 exists: does the GPU take DXT1 textures? */
void dxe_probe(void *d3d9) {
    const char *e = getenv("USF_BC_EMU");
    if (e && *e) emu = *e != '0';
    else {
        /* CheckDeviceFormat(adapter 0, HAL, X8R8G8B8, usage 0, D3DRTYPE_TEXTURE, DXT1) */
        int32_t r = (int32_t)dxb_method(d3d9, 10)(d3d9, 0, 1, 22, 0, 3, 0x31545844u, 0, 0, 0, 0, 0);
        emu = r < 0;
    }
    if (emu) fprintf(stderr, "usf: the GPU has no DXT textures: they are decoded to RGBA8 on the CPU\n");
}
int dxe_on(void) { return emu > 0; }
int dxe_block(uint32_t f) { return f == 0x31545844u || f == 0x32545844u || f == 0x33545844u || f == 0x34545844u || f == 0x35545844u; }
static int kind_of(uint32_t f) { return f == 0x31545844u ? 1 : f == 0x32545844u || f == 0x33545844u ? 2 : 3; }

/* A device create call about to go to DXVK: the format argument is swapped when it is DXT; returns the DXT format
 * (to remember on the new wrapper) or 0. slot 23 CreateTexture, 25 CreateCubeTexture, 36 CreateOffscreenPlainSurface */
uint32_t dxe_create(int slot, uint64_t *x) {
    int at = slot == 23 ? 4 : slot == 25 ? 3 : slot == 36 ? 2 : -1;
    if (at < 0 || !dxe_on() || !dxe_block((uint32_t)x[at])) return 0;
    uint32_t f = (uint32_t)x[at];
    x[at] = A8B8G8R8;
    return f;
}

/* Unlock of an emulated level: `rows` of blocks (pitch `gp` in the guest copy) into DXVK's RGBA rows (pitch np),
 * at most w x h pixels */
void dxe_decode(uint32_t fmt, const uint8_t *src, uint32_t gp, uint32_t rows, uint8_t *dst, uint32_t np, uint32_t w, uint32_t h) {
    if (!w || !h) return;
    uint8_t *strip = malloc((size_t)w * 4 * 4);
    for (uint32_t r = 0; r < rows && r * 4 < h; r++) {
        uint32_t lines = h - r * 4 < 4 ? h - r * 4 : 4;
        bc_decode(kind_of(fmt), src + (size_t)r * gp, strip, w, lines);
        for (uint32_t y = 0; y < lines; y++) memcpy(dst + (size_t)(r * 4 + y) * np, strip + (size_t)y * w * 4, (size_t)w * 4);
    }
    free(strip);
}
