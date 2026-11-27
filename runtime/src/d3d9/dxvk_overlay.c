/* The touch layer with DXVK: touch.c's controls drawn over the game's back buffer just before Present.
 *
 * What it does: on DXVK's device (host calls, the game never sees it):
 *   - the buttons and the stick are textured quads: the 8 drawings (touch_art, 256x256 RGBA premultiplied) in one
 *     2048x256 atlas with its mip levels, made once; each quad's diffuse colour carries its opacity and the pressed
 *     brightening (touch.c lists them: touch_quads). A few dozen vertices a frame;
 *   - shapes and text (the native menu, the layout editor, the fps counter) still come as a picture of the screen
 *     (touch_picture), drawn first, only when there is one, and uploaded only when it changes.
 * Fixed-function pipeline, premultiplied-alpha blending. The game's state is kept in a state block (D3DSBT_ALL) and
 * its render target / depth buffer are put back after the draw.
 * Android (DXVK with tools/android/dxvk_mobile_overlay_fsr.patch, DXVK_AndroidOverlay found): the layer is drawn
 * into a texture of the screen's own resolution instead of the back buffer, and DXVK blends it over each presented
 * image inside the controls' rectangles only: the controls stay sharp whatever the game's resolution (the
 * launcher's quality modes), and the game's image alone is enlarged (FSR 1.0 EASU unless "fsr=0" in port_settings).
 * What it does not do: anything when the layer is off (touch_picture returns NULL): no cost then.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "dxvk_bridge.h"

typedef struct { int art; float cx, cy, half, a; int bright; } TouchQuad;   /* touch.c */
const uint8_t *touch_picture(int w, int h, int *changed);
const TouchQuad *touch_quads(int *n);
int touch_picture_has(void);
extern int touch_gpu_sprites;
extern const uint8_t touch_art[];

#define M(o, s) dxb_method((o), (s))
#define U(x) ((uint64_t)(uintptr_t)(x))
#define ART 256
#define NART 8
#define ATLAS_LEVELS 6                                   /* 256 .. 8 pixels a drawing */
static void *tex, *atlas;
static uint32_t tw, th;
extern SDL_Window *g_window;
static void (*ovl_fn)(void *tex, const int32_t *rects, uint32_t n);   /* DXVK_AndroidOverlay, or NULL */
static void *ovl_tex;
static uint32_t ow, oh;
static int ovl_set;                                          /* an overlay is handed to DXVK now */

/* DXVK just loaded: its overlay entry, and FSR for enlarging (off: "fsr=0" in port_settings or USF_FSR=0) */
void dxb_overlay_init(void *lib);
void dxb_overlay_init(void *lib) {
    ovl_fn = (void (*)(void *, const int32_t *, uint32_t))dlsym(lib, "DXVK_AndroidOverlay");
    if (!ovl_fn) return;
    int port_setting(const char *key, int def);
    const char *e = getenv("USF_FSR");
    setenv("DXVK_FSR", (e ? *e == '1' : port_setting("fsr", 1)) ? "1" : "0", 1);
    fprintf(stderr, "usf: touch layer at the screen's resolution; FSR %s\n", getenv("DXVK_FSR"));
}
static void ovl_clear(void) { if (ovl_set) { ovl_fn(NULL, NULL, 0); ovl_set = 0; } }

static void release(void *o) { if (o) M(o, 2)(o, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0); }

/* the drawings side by side, with box-filtered mip levels (each drawing halves within its own cell); BGRA */
static void *make_atlas(void *dev) {
    void *t = NULL;
    if ((int32_t)M(dev, 23)(dev, ART * NART, ART, ATLAS_LEVELS, 0, 21, 1 /* MANAGED */, U(&t), 0, 0, 0, 0) < 0 || !t) return NULL;
    uint32_t W = ART * NART, H = ART;
    uint8_t *cur = malloc((size_t)W * H * 4);
    for (int a = 0; a < NART; a++)
        for (uint32_t y = 0; y < ART; y++)
            for (uint32_t x = 0; x < ART; x++) {
                const uint8_t *s = touch_art + (((size_t)a * ART + y) * ART + x) * 4;
                uint8_t *d = cur + ((size_t)y * W + (size_t)a * ART + x) * 4;
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
            }
    for (int l = 0; l < ATLAS_LEVELS; l++) {
        struct { int32_t pitch; int32_t pad; uint8_t *bits; } lr;
        if ((int32_t)M(t, 19)(t, (uint64_t)l, U(&lr), 0, 0, 0, 0, 0, 0, 0, 0, 0) >= 0) {
            for (uint32_t y = 0; y < H; y++) memcpy(lr.bits + (size_t)y * (uint32_t)lr.pitch, cur + (size_t)y * W * 4, (size_t)W * 4);
            M(t, 20)(t, (uint64_t)l, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        }
        if (l + 1 == ATLAS_LEVELS) break;
        uint32_t w2 = W / 2, h2 = H / 2;
        uint8_t *nx = malloc((size_t)w2 * h2 * 4);
        for (uint32_t y = 0; y < h2; y++)
            for (uint32_t x = 0; x < w2; x++)
                for (int c = 0; c < 4; c++) {
                    const uint8_t *p = cur + ((size_t)(2 * y) * W + 2 * x) * 4 + c;
                    nx[((size_t)y * w2 + x) * 4 + c] = (uint8_t)((p[0] + p[4] + p[(size_t)W * 4] + p[(size_t)W * 4 + 4] + 2) / 4);
                }
        free(cur);
        cur = nx; W = w2; H = h2;
    }
    free(cur);
    return t;
}

void dxb_overlay_draw(void *dev);
void dxb_overlay_draw(void *dev) {
    void *bb = NULL;
    uint32_t w = 0, h = 0;
    int over = 0;                                     /* drawing into the screen-sized overlay */
    if (ovl_fn && g_window) {
        int sw = 0, sh = 0;
        SDL_GetWindowSizeInPixels(g_window, &sw, &sh);
        if (sw < sh) { int t = sw; sw = sh; sh = t; }                                    /* (the game is landscape) */
        if (sw > 0 && (!ovl_tex || ow != (uint32_t)sw || oh != (uint32_t)sh)) {
            ovl_clear();
            release(ovl_tex);
            ovl_tex = NULL;
            /* CreateTexture: RENDERTARGET, A8R8G8B8, DEFAULT */
            if ((int32_t)M(dev, 23)(dev, (uint64_t)sw, (uint64_t)sh, 1, 1, 21, 0, U(&ovl_tex), 0, 0, 0, 0) < 0) ovl_tex = NULL;
            ow = (uint32_t)sw; oh = (uint32_t)sh;
        }
        if (ovl_tex && (int32_t)M(ovl_tex, 18)(ovl_tex, 0, U(&bb), 0, 0, 0, 0, 0, 0, 0, 0, 0) >= 0 && bb) {   /* GetSurfaceLevel */
            over = 1; w = ow; h = oh;
        }
    }
    if (!over) {
        if ((int32_t)M(dev, 18)(dev, 0, 0, 0, U(&bb), 0, 0, 0, 0, 0, 0, 0) < 0 || !bb) return;   /* GetBackBuffer */
        uint32_t desc[8];
        M(bb, 12)(bb, U(desc), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);                               /* GetDesc */
        w = desc[6]; h = desc[7];
    }
    int changed = 0;
    touch_gpu_sprites = 1;
    const uint8_t *pic = touch_picture((int)w, (int)h, &changed);
    if (!pic) { if (over) ovl_clear(); release(bb); return; }
    int nq = 0;
    const TouchQuad *q = touch_quads(&nq);
    int has_pic = touch_picture_has();
    if (!atlas) atlas = make_atlas(dev);
    if (has_pic && (!tex || tw != w || th != h)) {
        release(tex);
        tex = NULL;
        if ((int32_t)M(dev, 23)(dev, w, h, 1, 0x200, 21, 0, U(&tex), 0, 0, 0, 0) < 0 || !tex) has_pic = 0;
        else { tw = w; th = h; changed = 1; }                                               /* DYNAMIC, A8R8G8B8, DEFAULT */
    }
    if (has_pic && changed) {
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
    if (!has_pic && (!nq || !atlas)) { if (over) ovl_clear(); release(bb); return; }
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
    M(dev, 67)(dev, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0);                                        /* stage 1 off */
    static const uint32_t ss[][2] = {{1, 3}, {2, 3}, {11, 0}};                             /* clamp U / V, no sRGB */
    for (unsigned i = 0; i < 3; i++) M(dev, 69)(dev, 0, ss[i][0], ss[i][1], 0, 0, 0, 0, 0, 0, 0, 0);
    M(dev, 41)(dev, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);                                       /* BeginScene */
    if (over) M(dev, 43)(dev, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0);                              /* Clear: transparent */
    if (has_pic) {                                    /* the shapes and text: one full-screen quad, point sampled */
        M(dev, 89)(dev, 0x104, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);                               /* FVF XYZRHW | TEX1 */
        M(dev, 65)(dev, 0, U(tex), 0, 0, 0, 0, 0, 0, 0, 0, 0);
        static const uint32_t ts[][2] = {{1, 2}, {2, 2}, {4, 2}, {5, 2}};   /* colour and alpha: SELECTARG1 texture */
        for (unsigned i = 0; i < 4; i++) M(dev, 67)(dev, 0, ts[i][0], ts[i][1], 0, 0, 0, 0, 0, 0, 0, 0);
        static const uint32_t pt[][2] = {{5, 1}, {6, 1}, {7, 0}};                          /* point, no mips */
        for (unsigned i = 0; i < 3; i++) M(dev, 69)(dev, 0, pt[i][0], pt[i][1], 0, 0, 0, 0, 0, 0, 0, 0);
        float x0 = -0.5f, y0 = -0.5f, x1 = (float)w - 0.5f, y1 = (float)h - 0.5f;
        float v[4][6] = {{x0, y0, 0, 1, 0, 0}, {x1, y0, 0, 1, 1, 0}, {x0, y1, 0, 1, 0, 1}, {x1, y1, 0, 1, 1, 1}};
        M(dev, 83)(dev, 5, 2, U(v), 24, 0, 0, 0, 0, 0, 0, 0);                                /* DrawPrimitiveUP strip */
    }
    if (nq && atlas) {                                /* the drawings: one quad each, bilinear with mips */
        struct { float x, y, z, rhw; uint32_t col; float u, v; } vx[32 * 6];
        int n = 0;
        for (int i = 0; i < nq && i < 32; i++) {
            float a = q[i].a < 0 ? 0 : q[i].a > 1 ? 1 : q[i].a, lift = q[i].bright ? 1.35f : 1.0f, c = a * lift;
            uint32_t cc = (uint32_t)((c > 1 ? 1 : c) * 255.0f + 0.5f), aa = (uint32_t)(a * 255.0f + 0.5f);
            uint32_t col = aa << 24 | cc << 16 | cc << 8 | cc;
            float X0 = q[i].cx - q[i].half - 0.5f, X1 = q[i].cx + q[i].half - 0.5f;
            float Y0 = q[i].cy - q[i].half - 0.5f, Y1 = q[i].cy + q[i].half - 0.5f;
            float U0 = (float)q[i].art / NART, U1 = (float)(q[i].art + 1) / NART;
            float cs[6][4] = {{X0, Y0, U0, 0}, {X1, Y0, U1, 0}, {X0, Y1, U0, 1}, {X1, Y0, U1, 0}, {X1, Y1, U1, 1}, {X0, Y1, U0, 1}};
            for (int k = 0; k < 6; k++, n++) {
                vx[n].x = cs[k][0]; vx[n].y = cs[k][1]; vx[n].z = 0; vx[n].rhw = 1;
                vx[n].col = col; vx[n].u = cs[k][2]; vx[n].v = cs[k][3];
            }
        }
        M(dev, 89)(dev, 0x144, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);                               /* FVF XYZRHW | DIFFUSE | TEX1 */
        M(dev, 65)(dev, 0, U(atlas), 0, 0, 0, 0, 0, 0, 0, 0, 0);
        static const uint32_t ts[][2] = {{1, 4}, {2, 2}, {3, 0}, {4, 4}, {5, 2}, {6, 0}};   /* colour and alpha: texture x diffuse */
        for (unsigned i = 0; i < 6; i++) M(dev, 67)(dev, 0, ts[i][0], ts[i][1], 0, 0, 0, 0, 0, 0, 0, 0);
        static const uint32_t lin[][2] = {{5, 2}, {6, 2}, {7, 2}};                         /* linear, linear mips */
        for (unsigned i = 0; i < 3; i++) M(dev, 69)(dev, 0, lin[i][0], lin[i][1], 0, 0, 0, 0, 0, 0, 0, 0);
        M(dev, 83)(dev, 4, (uint64_t)(n / 3), U(vx), sizeof vx[0], 0, 0, 0, 0, 0, 0, 0);    /* DrawPrimitiveUP list */
    }
    M(dev, 42)(dev, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);                                       /* EndScene */
    if (sb) { M(sb, 5)(sb, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0); release(sb); }                  /* Apply: the game's state */
    M(dev, 37)(dev, 0, U(rt), 0, 0, 0, 0, 0, 0, 0, 0, 0);
    M(dev, 39)(dev, U(ds), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    release(rt); release(ds); release(bb);
    if (over) {                                       /* DXVK blends it inside these rectangles (x, y, w, h) */
        int32_t r[33 * 4];
        uint32_t n = 0;
        if (has_pic) { r[0] = 0; r[1] = 0; r[2] = (int32_t)w; r[3] = (int32_t)h; n = 1; }
        else for (int i = 0; i < nq && i < 32; i++) {
            float x0 = q[i].cx - q[i].half - 2, y0 = q[i].cy - q[i].half - 2, x1 = q[i].cx + q[i].half + 2, y1 = q[i].cy + q[i].half + 2;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > (float)w) x1 = (float)w;
            if (y1 > (float)h) y1 = (float)h;
            if (x1 <= x0 || y1 <= y0) continue;
            r[n * 4] = (int32_t)x0; r[n * 4 + 1] = (int32_t)y0; r[n * 4 + 2] = (int32_t)(x1 - (int32_t)x0 + 1); r[n * 4 + 3] = (int32_t)(y1 - (int32_t)y0 + 1);
            n++;
        }
        ovl_fn(ovl_tex, r, n);
        ovl_set = 1;
    }
}
