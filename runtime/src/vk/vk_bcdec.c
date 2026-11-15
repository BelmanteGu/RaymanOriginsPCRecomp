/* DXT1/3/5 (BC1/2/3) to RGBA8 on the CPU, for GPUs without BC textures (most phones: Adreno, Mali report
 * textureCompressionBC = false). Used by vk_res.c when uploading a DXT texture to such a GPU.
 * What it does not do: re-compress to ASTC/ETC2 (the picture is right; the memory is 4x/8x the DXT size).
 */
#include <stdint.h>
#include <string.h>

static void rgb565(uint16_t c, uint8_t *o) {
    o[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
    o[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
    o[2] = (uint8_t)((c & 31) * 255 / 31);
    o[3] = 255;
}

/* the colour part of a block (8 bytes) into 16 RGBA pixels; dxt1 allows the 3-colour + transparent mode */
static void color_block(const uint8_t *b, uint8_t px[16][4], int dxt1) {
    uint16_t c0 = (uint16_t)(b[0] | b[1] << 8), c1 = (uint16_t)(b[2] | b[3] << 8);
    uint8_t pal[4][4];
    rgb565(c0, pal[0]);
    rgb565(c1, pal[1]);
    if (c0 > c1 || !dxt1) {
        for (int k = 0; k < 3; k++) {
            pal[2][k] = (uint8_t)((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = (uint8_t)((pal[0][k] + 2 * pal[1][k]) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int k = 0; k < 3; k++) pal[2][k] = (uint8_t)((pal[0][k] + pal[1][k]) / 2);
        pal[2][3] = 255;
        pal[3][0] = pal[3][1] = pal[3][2] = 0; pal[3][3] = 0;
    }
    uint32_t idx = (uint32_t)b[4] | (uint32_t)b[5] << 8 | (uint32_t)b[6] << 16 | (uint32_t)b[7] << 24;
    for (int i = 0; i < 16; i++) memcpy(px[i], pal[(idx >> (2 * i)) & 3], 4);
}

/* kind: 1 = DXT1, 2 = DXT2/3 (explicit 4-bit alpha), 3 = DXT4/5 (interpolated alpha) */
void bc_decode(int kind, const uint8_t *src, uint8_t *dst, uint32_t w, uint32_t h) {
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bsize = kind == 1 ? 8 : 16;
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t *b = src + (by * bw + bx) * bsize;
            uint8_t px[16][4];
            color_block(kind == 1 ? b : b + 8, px, kind == 1);
            if (kind == 2) {
                for (int i = 0; i < 16; i++) px[i][3] = (uint8_t)(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
            } else if (kind == 3) {
                uint8_t a[8] = {b[0], b[1]};
                if (a[0] > a[1]) for (int k = 1; k < 7; k++) a[k + 1] = (uint8_t)(((7 - k) * a[0] + k * a[1]) / 7);
                else { for (int k = 1; k < 5; k++) a[k + 1] = (uint8_t)(((5 - k) * a[0] + k * a[1]) / 5); a[6] = 0; a[7] = 255; }
                uint64_t bits = 0;
                for (int k = 0; k < 6; k++) bits |= (uint64_t)b[2 + k] << (8 * k);
                for (int i = 0; i < 16; i++) px[i][3] = a[(bits >> (3 * i)) & 7];
            }
            for (int i = 0; i < 16; i++) {
                uint32_t x = bx * 4 + (uint32_t)(i & 3), y = by * 4 + (uint32_t)(i >> 2);
                if (x < w && y < h) memcpy(dst + ((size_t)y * w + x) * 4, px[i], 4);
            }
        }
}
