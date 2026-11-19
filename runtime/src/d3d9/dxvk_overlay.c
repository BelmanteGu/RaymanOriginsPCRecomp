/* The touch layer with DXVK: touch.c's picture blended over the game's back buffer just before Present.
 *
 * What it does: on DXVK's device (host calls, the game never sees it) one dynamic A8R8G8B8 texture the size of the back
 * buffer, refilled only when the picture changes (touch.c's RGBA, premultiplied, becomes BGRA), then one full-screen
 * quad with the fixed-function pipeline and premultiplied-alpha blending. The game's state is kept in a state block
 * (D3DSBT_ALL) and its render target / depth buffer are put back after the draw.
 * What it does not do: anything when the layer is off (touch_picture returns NULL): no cost then.
 */
#include <stdlib.h>
#include <string.h>

#include "dxvk_bridge.h"

const uint8_t *touch_picture(int w, int h, int *changed);

#define M(o, s) dxb_method((o), (s))
#define U(x) ((uint64_t)(uintptr_t)(x))
static void *tex;
static uint32_t tw, th;

static void release(void *o) { if (o) M(o, 2)(o, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0); }

void dxb_overlay_draw(void *dev);
void dxb_overlay_draw(void *dev) {
    void *bb = NULL;
    if ((int32_t)M(dev, 18)(dev, 0, 0, 0, U(&bb), 0, 0, 0, 0, 0, 0, 0) < 0 || !bb) return;   /* GetBackBuffer */
    uint32_t desc[8];
    M(bb, 12)(bb, U(desc), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);                                   /* GetDesc */
    uint32_t w = desc[6], h = desc[7];
    int changed = 0;
    const uint8_t *pic = touch_picture((int)w, (int)h, &changed);
    if (!pic) { release(bb); return; }
    if (!tex || tw != w || th != h) {
        release(tex);
        tex = NULL;
        if ((int32_t)M(dev, 23)(dev, w, h, 1, 0x200, 21, 0, U(&tex), 0, 0, 0, 0) < 0 || !tex) { release(bb); return; }
        tw = w; th = h; changed = 1;                                                        /* DYNAMIC, A8R8G8B8, DEFAULT */
    }
    if (changed) {
        struct { int32_t pitch; int32_t pad; uint8_t *bits; } lr;
        if ((int32_t)M(tex, 19)(tex, 0, U(&lr), 0, 0x2000, 0, 0, 0, 0, 0, 0, 0) >= 0) {   /* LockRect, DISCARD */
            for (uint32_t y = 0; y < h; y++) {
                const uint8_t *s = pic + (size_t)y * w * 4;
                uint8_t *d = lr.bits + (size_t)y * (uint32_t)lr.pitch;
                for (uint32_t x = 0; x < w; x++, s += 4, d += 4) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3]; }
            }
            M(tex, 20)(tex, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);                              /* UnlockRect */
        }
    }
    void *sb = NULL, *rt = NULL, *ds = NULL;
    M(dev, 59)(dev, 1, U(&sb), 0, 0, 0, 0, 0, 0, 0, 0, 0);                                  /* CreateStateBlock(ALL) */
    M(dev, 38)(dev, 0, U(&rt), 0, 0, 0, 0, 0, 0, 0, 0, 0);                                  /* GetRenderTarget(0) */
    M(dev, 40)(dev, U(&ds), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);                                  /* GetDepthStencilSurface */
    M(dev, 37)(dev, 0, U(bb), 0, 0, 0, 0, 0, 0, 0, 0, 0);
    M(dev, 39)(dev, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    uint32_t vp[6] = {0, 0, w, h, 0, 0x3F800000u};
    M(dev, 47)(dev, U(vp), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);                                   /* SetViewport */
    static const uint32_t rs[][2] = {
        {7, 0}, {14, 0}, {22, 1}, {27, 1}, {19, 2}, {20, 6}, {171, 1}, {15, 0}, {52, 0}, {174, 0}, {168, 0xF},
        {28, 0}, {137, 0}, {194, 0}, {206, 0}};   /* Z off, no Z write, no cull, blend ONE / INVSRCALPHA, add, no alpha
                                                     test, stencil, scissor; all colour; no fog, lighting, sRGB, separate alpha */
    for (unsigned i = 0; i < sizeof rs / sizeof rs[0]; i++) M(dev, 57)(dev, rs[i][0], rs[i][1], 0, 0, 0, 0, 0, 0, 0, 0, 0);
    M(dev, 92)(dev, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);                                       /* no vertex shader */
    M(dev, 107)(dev, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);                                      /* no pixel shader */
    M(dev, 89)(dev, 0x104, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);                                   /* FVF XYZRHW | TEX1 */
    M(dev, 65)(dev, 0, U(tex), 0, 0, 0, 0, 0, 0, 0, 0, 0);
    static const uint32_t ts[][2] = {{1, 2}, {2, 2}, {4, 2}, {5, 2}};   /* colour and alpha: SELECTARG1 texture */
    for (unsigned i = 0; i < 4; i++) M(dev, 67)(dev, 0, ts[i][0], ts[i][1], 0, 0, 0, 0, 0, 0, 0, 0);
    M(dev, 67)(dev, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0);                                        /* stage 1 off */
    static const uint32_t ss[][2] = {{5, 1}, {6, 1}, {7, 0}, {1, 3}, {2, 3}, {11, 0}};   /* clamp, point filter, no mips, no sRGB */
    for (unsigned i = 0; i < 6; i++) M(dev, 69)(dev, 0, ss[i][0], ss[i][1], 0, 0, 0, 0, 0, 0, 0, 0);
    float x0 = -0.5f, y0 = -0.5f, x1 = (float)w - 0.5f, y1 = (float)h - 0.5f;
    float v[4][6] = {{x0, y0, 0, 1, 0, 0}, {x1, y0, 0, 1, 1, 0}, {x0, y1, 0, 1, 0, 1}, {x1, y1, 0, 1, 1, 1}};
    M(dev, 41)(dev, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);                                       /* BeginScene */
    M(dev, 83)(dev, 5, 2, U(v), 24, 0, 0, 0, 0, 0, 0, 0);                                    /* DrawPrimitiveUP strip */
    M(dev, 42)(dev, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);                                       /* EndScene */
    if (sb) { M(sb, 5)(sb, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0); release(sb); }                  /* Apply: the game's state */
    M(dev, 37)(dev, 0, U(rt), 0, 0, 0, 0, 0, 0, 0, 0, 0);
    M(dev, 39)(dev, U(ds), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    release(rt); release(ds); release(bb);
}
