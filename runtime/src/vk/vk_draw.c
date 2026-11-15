/* Draws: Direct3D 9 state -> Vulkan pipelines, render passes, vertex input, constants, descriptors.
 *
 * What it does: compiles each game shader once (sm3.c GLSL -> SPIR-V with libshaderc); keeps render passes and
 * framebuffers for each set of render targets; builds pipelines keyed by everything D3D9 bakes into them (shaders,
 * vertex declaration and strides, topology, blend, depth/stencil, cull, colour masks, fill mode, depth bias); per draw
 * copies the vertex range, indices and constants into the frame's ring, writes one descriptor set and draws. Viewport
 * is flipped (negative height, Vulkan 1.1 core) so D3D9 clip space works unchanged; the half-pixel offset is in the
 * vertex shader. Clear is vkCmdClearAttachments over the viewport.
 * What it does not do: fixed-function vertex or pixel processing (draws without both shaders are skipped and
 * counted), instancing (stream frequency), MSAA.
 */
#include <shaderc/shaderc.h>
#include <stdlib.h>
#include <string.h>

#include "sm3.h"
#include "vk_int.h"

static VkDescriptorSetLayout dsl;
static VkPipelineLayout playout;
static VkBuffer defaults_buf;                 /* (0,0,0,1) for vertex inputs the declaration does not provide */
static shaderc_compiler_t sc;
static uint32_t skipped_ff, skipped_bad;

typedef struct { VkShaderModule mod; Sm3Shader info; int bad; } GpuShader;

static GpuShader *shader(D3DObj *o) {
    if (!o) return NULL;
    if (o->gpu) return o->gpu;
    GpuShader *s = calloc(1, sizeof *s);
    o->gpu = s;
    if (sm3_translate((const uint32_t *)HOST(o->code), o->code_size / 4, &s->info)) {
        fprintf(stderr, "usf: shader %u not translated: %s\n", R32(o->guest + 8), s->info.error);
        s->bad = 1;
        return s;
    }
    shaderc_compile_options_t opt = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(opt, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);
    shaderc_compile_options_set_optimization_level(opt, shaderc_optimization_level_performance);
    shaderc_compilation_result_t r = shaderc_compile_into_spv(sc, s->info.glsl, strlen(s->info.glsl),
        s->info.is_ps ? shaderc_fragment_shader : shaderc_vertex_shader, "game", "main", opt);
    shaderc_compile_options_release(opt);
    if (shaderc_result_get_compilation_status(r) != shaderc_compilation_status_success) {
        fprintf(stderr, "usf: shader compile failed: %s\n", shaderc_result_get_error_message(r));
        s->bad = 1;
    } else {
        VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = shaderc_result_get_length(r);
        ci.pCode = (const uint32_t *)shaderc_result_get_bytes(r);
        VKC(vkCreateShaderModule(vk.dev, &ci, NULL, &s->mod));
    }
    shaderc_result_release(r);
    return s;
}

/* ---- render passes and framebuffers ---- */
typedef struct { VkFormat f[5]; uint32_t n; VkRenderPass rp; } RpEnt;
static RpEnt rps[64];
static int nrps;

static VkRenderPass render_pass(const VkFormat *f, uint32_t ncol, int has_depth) {
    VkFormat key[5] = {0};
    for (uint32_t i = 0; i < ncol; i++) key[i] = f[i];
    if (has_depth) key[4] = f[4];
    for (int i = 0; i < nrps; i++) if (rps[i].n == ncol && !memcmp(rps[i].f, key, sizeof key)) return rps[i].rp;
    VkAttachmentDescription at[5];
    VkAttachmentReference cref[4], dref;
    uint32_t na = 0;
    for (uint32_t i = 0; i < ncol + (uint32_t)(has_depth != 0); i++) {
        int d = i == ncol;
        VkAttachmentDescription *a = &at[na];
        memset(a, 0, sizeof *a);
        a->format = d ? f[4] : f[i];
        a->samples = VK_SAMPLE_COUNT_1_BIT;
        a->loadOp = a->stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        a->storeOp = a->stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        a->initialLayout = a->finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        if (d) dref = (VkAttachmentReference){na, VK_IMAGE_LAYOUT_GENERAL};
        else cref[i] = (VkAttachmentReference){na, VK_IMAGE_LAYOUT_GENERAL};
        na++;
    }
    VkSubpassDescription sub = {0};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = ncol;
    sub.pColorAttachments = cref;
    sub.pDepthStencilAttachment = has_depth ? &dref : NULL;
    VkRenderPassCreateInfo ci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = na;
    ci.pAttachments = at;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    VkRenderPass rp;
    VKC(vkCreateRenderPass(vk.dev, &ci, NULL, &rp));
    if (nrps < 64) { memcpy(rps[nrps].f, key, sizeof key); rps[nrps].n = ncol; rps[nrps++].rp = rp; }
    return rp;
}

typedef struct { VkImageView v[5]; uint32_t w, h; VkRenderPass rp; VkFramebuffer fb; } FbEnt;
static FbEnt fbs[1024];
static int nfbs;

static struct {
    VkImageView v[5];
    VkFormat f[5];
    uint32_t ncol, w, h;
    int has_depth;
    VkRenderPass rp;
} cur;

static VkFramebuffer framebuffer(void) {
    for (int i = 0; i < nfbs; i++)
        if (fbs[i].rp == cur.rp && fbs[i].w == cur.w && fbs[i].h == cur.h && !memcmp(fbs[i].v, cur.v, sizeof cur.v)) return fbs[i].fb;
    VkImageView att[5];
    uint32_t n = 0;
    for (uint32_t i = 0; i < cur.ncol; i++) att[n++] = cur.v[i];
    if (cur.has_depth) att[n++] = cur.v[4];
    VkFramebufferCreateInfo ci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    ci.renderPass = cur.rp;
    ci.attachmentCount = n;
    ci.pAttachments = att;
    ci.width = cur.w;
    ci.height = cur.h;
    ci.layers = 1;
    VkFramebuffer fb;
    VKC(vkCreateFramebuffer(vk.dev, &ci, NULL, &fb));
    int slot = nfbs < 1024 ? nfbs++ : (int)(vk.frame % 1024);
    memcpy(fbs[slot].v, cur.v, sizeof cur.v);
    fbs[slot].w = cur.w; fbs[slot].h = cur.h; fbs[slot].rp = cur.rp; fbs[slot].fb = fb;
    return fb;
}

/* Opens a render pass on the current render targets (or keeps the open one). 0 if there is nothing to draw to. */
static int ensure_pass(void) {
    VkImageView v[5] = {0};
    VkFormat f[5] = {0};
    uint32_t ncol = 0, w = 0, h = 0;
    for (int i = 0; i < 4; i++) {
        if (!g_dev.rt[i]) break;
        int tw, th;
        GpuImage *g = vk_target(g_dev.rt[i], &v[i], &tw, &th);
        if (!g || g->depth || !v[i]) break;
        f[i] = g->fmt;
        if (!w || (uint32_t)tw < w) w = (uint32_t)tw;
        if (!h || (uint32_t)th < h) h = (uint32_t)th;
        ncol++;
    }
    int has_depth = 0;
    if (g_dev.ds) {
        int tw, th;
        GpuImage *g = vk_target(g_dev.ds, &v[4], &tw, &th);
        if (g && g->depth && v[4]) {
            has_depth = 1;
            f[4] = g->fmt;
            if (!w || (uint32_t)tw < w) w = (uint32_t)tw;
            if (!h || (uint32_t)th < h) h = (uint32_t)th;
        }
    }
    if (!ncol && !has_depth) return 0;
    if (vk.in_pass && cur.ncol == ncol && cur.has_depth == has_depth && cur.w == w && cur.h == h && !memcmp(cur.v, v, sizeof v))
        return 1;
    vk_end_pass();
    memcpy(cur.v, v, sizeof v);
    memcpy(cur.f, f, sizeof f);
    cur.ncol = ncol; cur.w = w; cur.h = h; cur.has_depth = has_depth;
    cur.rp = render_pass(f, ncol, has_depth);
    VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = cur.rp;
    bi.framebuffer = framebuffer();
    bi.renderArea.extent = (VkExtent2D){w, h};
    vkCmdBeginRenderPass(vk.cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    vk.in_pass = 1;
    return 1;
}

/* ---- ultrawide: the 2D at its proportions (see d3d9_device.c apply_aspect for the 3D) ----
 * With a wide aspect A, per draw on a screen-sized target (the back buffer, or the 16:9 scene target that the game
 * stretches onto it):
 *  - Scaleform (its five vertex shaders) lays the 1280x720 movie out "no border": scaled to the width and cut at the
 *    top and bottom. It is rescaled to fill the screen stretched (menus, USF_ASPECT_2D=stretch, the default) or to
 *    keep its proportions in the middle (over a 3D scene, and with USF_ASPECT_2D=fit).
 *  - In a frame with a 3D scene (>= 16 depth-tested draws in the previous frame), the game's own 2D drawn with an
 *    ordinary texture (HUD, K.O. lettering, subtitles) is narrowed by (16/9)/A about the centre: the HUD keeps its
 *    proportions. Full-screen passes read render targets and are left alone, as are untextured fades. */
extern float g_aspect;
static uint32_t depth_draws, depth_draws_prev;
void vk_aspect_frame_end(void);
void vk_aspect_frame_end(void) { depth_draws_prev = depth_draws; depth_draws = 0; }
static struct { uint32_t base, stride, n, comps; } av;          /* the draw's positions (set by r_draw) */
/* a draw from edge to edge of the target (a cover: vignette, flash, background): its positions through c0 and c3 */
static int is_cover(void) {
    if (!av.n || av.n > 64 || !av.stride) return 0;
    float mn = 1e9f, mx = -1e9f;
    const float *c0 = g_dev.vsc[0], *c3 = g_dev.vsc[3];
    for (uint32_t i = 0; i < av.n; i++) {
        float p[4] = {0, 0, 0, 1};
        for (uint32_t k = 0; k < av.comps; k++) { uint32_t u = R32(av.base + i * av.stride + 4 * k); memcpy(&p[k], &u, 4); }
        if (av.comps < 4) p[3] = 1;
        float x = p[0] * c0[0] + p[1] * c0[1] + p[2] * c0[2] + p[3] * c0[3];
        float w = p[0] * c3[0] + p[1] * c3[1] + p[2] * c3[2] + p[3] * c3[3];
        if (w == 0) return 0;
        x /= w;
        if (x < mn) mn = x;
        if (x > mx) mx = x;
    }
    return mn <= -0.97f && mx >= 0.97f;
}
static int is_scaleform(uint32_t h) {
    return h == 0x8359ee0du || h == 0xa5813c47u || h == 0xeeb6de99u || h == 0xa7b69242u || h == 0x1aa52b0du;
}
static void aspect_scale(float *sx, float *sy) {
    *sx = *sy = 1.0f;
    if (g_dev.rs[7] && g_dev.ds) depth_draws++;
    return;                       /* Rayman Origins: UbiArt places its own 2D for the wide aspect (game.h) */
    if (g_aspect < 16.0f / 9.0f + 0.002f || cur.h != g_dev.bb_h || cur.w * 9 < cur.h * 16 * 98 / 100) return;
    if (g_dev.rs[7] && g_dev.ds) return;                                         /* 3D */
    int scene = depth_draws_prev >= 16 || depth_draws >= 16;
    if (is_scaleform(d3d_shader_hash(g_dev.vs))) {
        if (cur.w != g_dev.bb_w) return;
        float fx = (float)cur.w / 1280.0f, fy = (float)cur.h / 720.0f, nb = fx > fy ? fx : fy;
        const char *m = getenv("USF_ASPECT_2D");
        if (scene || (m && !strcmp(m, "fit"))) { float f = (fx < fy ? fx : fy) / nb; *sx = *sy = f; }
        else { *sx = fx / nb; *sy = fy / nb; }
        return;
    }
    D3DObj *t = d3d_obj(g_dev.tex[0]);
    if (scene && t && !(t->usage & 3u) && !is_cover()) *sx = (16.0f / 9.0f) / g_aspect;
}

static float as_x = 1.0f, as_y = 1.0f;                     /* this draw's clip-space scale (vertex shader misc.zw) */
static void aspect_compute(void) { aspect_scale(&as_x, &as_y); }

static void set_viewport_scissor(void) {
    float vx = (float)g_dev.vp[0], vy = (float)g_dev.vp[1], vw = (float)g_dev.vp[2], vh = (float)g_dev.vp[3], zn, zf;
    memcpy(&zn, &g_dev.vp[4], 4);
    memcpy(&zf, &g_dev.vp[5], 4);
    if (vw <= 0 || vh <= 0) { vw = (float)cur.w; vh = (float)cur.h; vx = vy = 0; }
    float asx = as_x, asy = as_y;                                /* the clip scale is in the shader; scissor here */
    VkViewport vp = {vx, vy + vh, vw, -vh, zn, zf};
    vkCmdSetViewport(vk.cmd, 0, 1, &vp);
    VkRect2D sc = {{(int)vx, (int)vy}, {(uint32_t)vw, (uint32_t)vh}};
    if (sc.offset.x < 0) { sc.extent.width = (uint32_t)((int)sc.extent.width + sc.offset.x); sc.offset.x = 0; }
    if (sc.offset.y < 0) { sc.extent.height = (uint32_t)((int)sc.extent.height + sc.offset.y); sc.offset.y = 0; }
    if (g_dev.rs[174]) {                                         /* SCISSORTESTENABLE */
        int32_t l = (int32_t)g_dev.scissor[0], t = (int32_t)g_dev.scissor[1], r = (int32_t)g_dev.scissor[2], b = (int32_t)g_dev.scissor[3];
        if (asx != 1.0f || asy != 1.0f) {
            float cx = cur.w * 0.5f, cy = cur.h * 0.5f;
            l = (int32_t)(cx + (l - cx) * asx); r = (int32_t)(cx + (r - cx) * asx);
            t = (int32_t)(cy + (t - cy) * asy); b = (int32_t)(cy + (b - cy) * asy);
        }
        if (l > sc.offset.x) sc.offset.x = l;
        if (t > sc.offset.y) sc.offset.y = t;
        int32_t rr = (int32_t)(vx + vw) < r ? (int32_t)(vx + vw) : r, bb = (int32_t)(vy + vh) < b ? (int32_t)(vy + vh) : b;
        sc.extent.width = rr > sc.offset.x ? (uint32_t)(rr - sc.offset.x) : 0;
        sc.extent.height = bb > sc.offset.y ? (uint32_t)(bb - sc.offset.y) : 0;
    }
    if (sc.offset.x < 0) sc.offset.x = 0;
    if (sc.offset.y < 0) sc.offset.y = 0;
    if (sc.offset.x + sc.extent.width > cur.w) sc.extent.width = cur.w > (uint32_t)sc.offset.x ? cur.w - (uint32_t)sc.offset.x : 0;
    if (sc.offset.y + sc.extent.height > cur.h) sc.extent.height = cur.h > (uint32_t)sc.offset.y ? cur.h - (uint32_t)sc.offset.y : 0;
    vkCmdSetScissor(vk.cmd, 0, 1, &sc);
    vkCmdSetStencilReference(vk.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, g_dev.rs[57]);
    float bf[4] = {((g_dev.rs[193] >> 16) & 255) / 255.0f, ((g_dev.rs[193] >> 8) & 255) / 255.0f, (g_dev.rs[193] & 255) / 255.0f,
                   (g_dev.rs[193] >> 24) / 255.0f};
    vkCmdSetBlendConstants(vk.cmd, bf);
}

void r_clear(uint32_t flags, uint32_t color, float z, uint32_t stencil) {
    if (!vk.dev || !ensure_pass()) return;
    VkClearAttachment ca[5];
    uint32_t n = 0;
    if (flags & 1u)
        for (uint32_t i = 0; i < cur.ncol; i++) {
            ca[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            ca[n].colorAttachment = i;
            ca[n].clearValue.color = (VkClearColorValue){{((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f,
                                                          (color & 255) / 255.0f, (color >> 24) / 255.0f}};
            n++;
        }
    if ((flags & 6u) && cur.has_depth) {
        ca[n].aspectMask = ((flags & 2u) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                           ((flags & 4u) && cur.f[4] != VK_FORMAT_D16_UNORM ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        ca[n].clearValue.depthStencil = (VkClearDepthStencilValue){z, stencil};
        if (ca[n].aspectMask) n++;
    }
    if (!n) return;
    int32_t x = (int32_t)g_dev.vp[0], y = (int32_t)g_dev.vp[1];
    uint32_t w = g_dev.vp[2], h = g_dev.vp[3];
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (!w || (uint32_t)x + w > cur.w) w = cur.w - (uint32_t)x;
    if (!h || (uint32_t)y + h > cur.h) h = cur.h - (uint32_t)y;
    VkClearRect r = {{{x, y}, {w, h}}, 0, 1};
    vkCmdClearAttachments(vk.cmd, n, ca, 1, &r);
}

#include "vk_pipeline.inc"
