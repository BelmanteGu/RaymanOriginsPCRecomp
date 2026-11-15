/* Vulkan images for Direct3D 9 resources: textures, cube and volume textures, surfaces, render targets, depth
 * buffers; uploads from guest memory; samplers.
 *
 * What it does: maps D3D9 formats to Vulkan formats that need no conversion (component swizzles for L8, A8, A8L8,
 * X8R8G8B8, A4R4G4B4...), creates each image on first use, uploads every face and level from the guest memory the
 * game wrote through Lock when its version changed, and gives render targets and depth surfaces attachment views
 * (a texture's level when the surface belongs to a texture). Samplers are cached by the D3D9 sampler states.
 * What it does not do: decompress BC formats for GPUs without BC support (Android: next step), read GPU images back
 * into guest memory (GetRenderTargetData copies the guest memory only), or free images.
 */
#include <stdlib.h>
#include <string.h>

#include "vk_int.h"

GpuImage g_backbuffer_img;
static GpuImage dummies[5];

#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define SWZ(r, g, b, a) (VkComponentMapping){VK_COMPONENT_SWIZZLE_##r, VK_COMPONENT_SWIZZLE_##g, VK_COMPONENT_SWIZZLE_##b, VK_COMPONENT_SWIZZLE_##a}

static int is_depth(uint32_t f) {
    return f == 75 || f == 77 || f == 80 || f == 71 || f == 79 || f == 82 || f == 70 || f == 73 ||
           f == FOURCC('I', 'N', 'T', 'Z') || f == FOURCC('D', 'F', '2', '4') || f == FOURCC('D', 'F', '1', '6') ||
           f == FOURCC('R', 'A', 'W', 'Z');
}

VkFormat vk_d3d_format(uint32_t f, VkComponentMapping *swz, int *convert) {
    *swz = SWZ(IDENTITY, IDENTITY, IDENTITY, IDENTITY);
    *convert = 0;
    if (is_depth(f)) return f == 80 ? VK_FORMAT_D16_UNORM : vk.depth_format;
    switch (f) {
    case 21: return VK_FORMAT_B8G8R8A8_UNORM;
    case 22: *swz = SWZ(IDENTITY, IDENTITY, IDENTITY, ONE); return VK_FORMAT_B8G8R8A8_UNORM;
    case 32: return VK_FORMAT_R8G8B8A8_UNORM;
    case 33: *swz = SWZ(IDENTITY, IDENTITY, IDENTITY, ONE); return VK_FORMAT_R8G8B8A8_UNORM;
    case 20: *convert = 1; return VK_FORMAT_B8G8R8A8_UNORM;                        /* R8G8B8: expanded on upload */
    case 23: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case 25: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
    case 24: *swz = SWZ(IDENTITY, IDENTITY, IDENTITY, ONE); return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
    case 26: *swz = SWZ(G, R, A, B); return VK_FORMAT_B4G4R4A4_UNORM_PACK16;       /* A4R4G4B4 read as B4G4R4A4 */
    case 28: *swz = SWZ(ZERO, ZERO, ZERO, R); return VK_FORMAT_R8_UNORM;           /* A8 */
    case 50: *swz = SWZ(R, R, R, ONE); return VK_FORMAT_R8_UNORM;                  /* L8 */
    case 51: *swz = SWZ(R, R, R, G); return VK_FORMAT_R8G8_UNORM;                  /* A8L8 */
    case 34: return VK_FORMAT_R16G16_UNORM;
    case 35: return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    case 31: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 36: return VK_FORMAT_R16G16B16A16_UNORM;
    case 111: return VK_FORMAT_R16_SFLOAT;
    case 112: return VK_FORMAT_R16G16_SFLOAT;
    case 113: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 114: return VK_FORMAT_R32_SFLOAT;
    case 115: return VK_FORMAT_R32G32_SFLOAT;
    case 116: return VK_FORMAT_R32G32B32A32_SFLOAT;
    /* DXT: BC on GPUs that have it; else decoded to RGBA8 on upload (vk_bcdec.c), as on most phones */
    case FOURCC('D', 'X', 'T', '1'): if (!vk.feats.textureCompressionBC) { *convert = 2; return VK_FORMAT_R8G8B8A8_UNORM; }
        return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case FOURCC('D', 'X', 'T', '2'): case FOURCC('D', 'X', 'T', '3'):
        if (!vk.feats.textureCompressionBC) { *convert = 3; return VK_FORMAT_R8G8B8A8_UNORM; }
        return VK_FORMAT_BC2_UNORM_BLOCK;
    case FOURCC('D', 'X', 'T', '4'): case FOURCC('D', 'X', 'T', '5'):
        if (!vk.feats.textureCompressionBC) { *convert = 4; return VK_FORMAT_R8G8B8A8_UNORM; }
        return VK_FORMAT_BC3_UNORM_BLOCK;
    default: return VK_FORMAT_B8G8R8A8_UNORM;                                    /* NULL / unknown: a colour target */
    }
}

static void to_general(VkImage img, VkImageAspectFlags aspect) {
    vk_end_pass();
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = (VkImageSubresourceRange){aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(vk.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static VkImageView make_view(VkImage img, VkImageViewType type, VkFormat fmt, VkComponentMapping swz, VkImageAspectFlags aspect,
                             uint32_t level, uint32_t nlevels, uint32_t layer, uint32_t nlayers) {
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img;
    vi.viewType = type;
    vi.format = fmt;
    vi.components = swz;
    vi.subresourceRange = (VkImageSubresourceRange){aspect, level, nlevels, layer, nlayers};
    VkImageView v;
    VKC(vkCreateImageView(vk.dev, &vi, NULL, &v));
    return v;
}

/* kind: 2 = 2D, 3 = cube, 4 = 3D */
static void create(GpuImage *g, int kind, uint32_t d3dfmt, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, int target) {
    VkComponentMapping swz;
    g->fmt = vk_d3d_format(d3dfmt, &swz, &g->convert);
    g->depth = is_depth(d3dfmt);
    g->w = w; g->h = h; g->levels = levels ? levels : 1; g->layers = kind == 3 ? 6 : 1;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = kind == 4 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = g->fmt;
    ci.extent = (VkExtent3D){w, h, kind == 4 ? d : 1};
    ci.mipLevels = g->levels;
    ci.arrayLayers = g->layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (g->depth) ci.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    else if (target) ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (kind == 3) ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    VKC(vkCreateImage(vk.dev, &ci, NULL, &g->img));
    vk_bind_image(g->img);
    VkImageAspectFlags asp = g->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageAspectFlags all = g->depth && g->fmt != VK_FORMAT_D16_UNORM ? asp | VK_IMAGE_ASPECT_STENCIL_BIT : asp;
    if (g->depth) swz = SWZ(R, R, R, ONE);
    g->view = make_view(g->img, kind == 3 ? VK_IMAGE_VIEW_TYPE_CUBE : kind == 4 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D,
                        g->fmt, swz, asp, 0, g->levels, 0, g->layers);
    if (target || g->depth) {
        VkComponentMapping id = SWZ(IDENTITY, IDENTITY, IDENTITY, IDENTITY);
        for (uint32_t l = 0; l < g->levels && l < MAX_LEVELS; l++)
            g->att[l] = make_view(g->img, VK_IMAGE_VIEW_TYPE_2D, g->fmt, id, all, l, 1, 0, 1);
        if (kind == 3)
            for (uint32_t f = 0; f < 6; f++) g->cube_att[f] = make_view(g->img, VK_IMAGE_VIEW_TYPE_2D, g->fmt, id, all, 0, 1, f, 1);
    }
    to_general(g->img, all);
    if (target && !g->depth) {                               /* render targets start black, as on D3D9 drivers */
        VkClearColorValue z = {{0, 0, 0, 0}};
        VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, 0, g->levels, 0, g->layers};
        vkCmdClearColorImage(vk.cmd, g->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &r);
        vk_barrier();
    }
}

static void upload(GpuImage *g, D3DObj *o) {
    int faces = o->kind == OBJ_CUBE ? 6 : 1;
    uint32_t total = 0;
    for (uint32_t l = 0; l < g->levels; l++) {
        uint32_t lw = g->w >> l ? g->w >> l : 1, lh = g->h >> l ? g->h >> l : 1;
        total += ((g->convert ? lw * lh * 4 : o->size[l]) + 15u) * (uint32_t)faces;
    }
    vk_ring_reserve(total);
    vk_end_pass();
    VkBufferImageCopy regs[6 * MAX_LEVELS];
    uint32_t n = 0;
    for (int f = 0; f < faces; f++)
        for (uint32_t l = 0; l < g->levels; l++) {
            uint32_t lw = g->w >> l ? g->w >> l : 1, lh = g->h >> l ? g->h >> l : 1;
            uint32_t ld = o->kind == OBJ_VOLUME ? (o->depth >> l ? o->depth >> l : 1) : 1;
            uint8_t *dst;
            uint32_t sz = g->convert ? lw * lh * 4 : o->size[l];
            uint32_t off = vk_ring_alloc(sz, 16, (void **)&dst);
            const uint8_t *s = HOST(o->mem[f][l]);
            if (g->convert >= 2) {
                void bc_decode(int kind, const uint8_t *src, uint8_t *dst, uint32_t w, uint32_t h);
                bc_decode(g->convert - 1, s, dst, lw, lh);
            } else if (g->convert == 1) {
                for (uint32_t i = 0; i < lw * lh; i++) { dst[4 * i] = s[3 * i]; dst[4 * i + 1] = s[3 * i + 1]; dst[4 * i + 2] = s[3 * i + 2]; dst[4 * i + 3] = 255; }
            } else {
                memcpy(dst, s, sz);
            }
            VkBufferImageCopy *r = &regs[n++];
            memset(r, 0, sizeof *r);
            r->bufferOffset = off;
            r->imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, l, (uint32_t)f, 1};
            r->imageExtent = (VkExtent3D){lw, lh, ld};
        }
    vkCmdCopyBufferToImage(vk.cmd, vk.ring, g->img, VK_IMAGE_LAYOUT_GENERAL, n, regs);
    vk_barrier();
    g->version = o->version;
}

GpuImage *vk_texture(D3DObj *o) {
    if (!o || (o->kind != OBJ_TEXTURE && o->kind != OBJ_CUBE && o->kind != OBJ_VOLUME)) return NULL;
    GpuImage *g = o->gpu;
    if (!g) {
        g = o->gpu = calloc(1, sizeof *g);
        create(g, o->kind == OBJ_CUBE ? 3 : o->kind == OBJ_VOLUME ? 4 : 2, o->format, o->width, o->height, o->depth,
               o->levels, (o->usage & 1u) != 0);
        g->version = (o->usage & 3u) ? o->version : (uint32_t)-1;
    }
    if (!(o->usage & 3u) && !g->depth && g->version != o->version) upload(g, o);
    return g;
}

GpuImage *vk_target(uint32_t sg, VkImageView *view, int *w, int *h) {
    D3DObj *s = d3d_obj(sg);
    if (!s) return NULL;
    GpuImage *g;
    if (sg == g_dev.backbuffer) {
        g = &g_backbuffer_img;
        if (!g->img || g->w != s->width || g->h != s->height) create(g, 2, 21, s->width, s->height, 1, 1, 1);
        *view = g->att[0];
    } else if (s->parent) {
        D3DObj *p = d3d_obj(s->parent);
        g = vk_texture(p);
        if (!g || !(g->att[0] || g->depth)) return NULL;
        *view = p->kind == OBJ_CUBE ? g->cube_att[s->face] : g->att[s->level];
    } else {
        g = s->gpu;
        if (!g) { g = s->gpu = calloc(1, sizeof *g); create(g, 2, s->format, s->width, s->height, 1, 1, 1); }
        *view = g->att[0];
    }
    *w = (int)s->width;
    *h = (int)s->height;
    return g;
}

GpuImage *vk_dummy_texture(int type) {
    GpuImage *g = &dummies[type];
    if (!g->img) {
        create(g, type, 21, 1, 1, 1, 1, 0);
        uint8_t *p;
        uint32_t off = vk_ring_alloc(4 * 6, 16, (void **)&p);
        for (int i = 0; i < 6; i++) { p[4 * i] = 0; p[4 * i + 1] = 0; p[4 * i + 2] = 0; p[4 * i + 3] = 255; }
        VkBufferImageCopy r = {0};
        r.bufferOffset = off;
        r.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, (uint32_t)g->layers};
        r.imageExtent = (VkExtent3D){1, 1, 1};
        vkCmdCopyBufferToImage(vk.cmd, vk.ring, g->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        vk_barrier();
    }
    return g;
}

/* ---- samplers ---- */
typedef struct { uint32_t key[10]; VkSampler s; } SampEnt;
static SampEnt samps[512];
static int nsamps;

static VkSamplerAddressMode addr(uint32_t a) {
    switch (a) {
    case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 3: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;     /* mirror once: not in Vulkan 1.1 core */
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

VkSampler vk_sampler(const uint32_t *ss) {
    uint32_t key[10] = {ss[1], ss[2], ss[3], ss[4], ss[5], ss[6], ss[7], ss[8], ss[9], ss[10]};
    for (int i = 0; i < nsamps; i++) if (!memcmp(samps[i].key, key, sizeof key)) return samps[i].s;
    VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = ss[5] >= 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = ss[6] >= 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = ss[7] == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = addr(ss[1]);
    ci.addressModeV = addr(ss[2]);
    ci.addressModeW = addr(ss[3]);
    float bias;
    memcpy(&bias, &ss[8], 4);
    ci.mipLodBias = bias;
    if ((ss[5] == 3 || ss[6] == 3) && vk.feats.samplerAnisotropy) {
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = ss[10] > 1 ? (float)(ss[10] > 16 ? 16 : ss[10]) : 1.0f;
    }
    ci.minLod = (float)ss[9];
    ci.maxLod = ss[7] == 0 ? (float)ss[9] + 0.25f : 1000.0f;   /* MIPFILTER NONE: base level only */
    uint32_t bc = ss[4];
    ci.borderColor = (bc >> 24) < 128 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
                   : (bc & 0xFFFFFF) > 0x7F7F7F ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    VkSampler s;
    VKC(vkCreateSampler(vk.dev, &ci, NULL, &s));
    if (nsamps < 512) { memcpy(samps[nsamps].key, key, sizeof key); samps[nsamps++].s = s; }
    return s;
}

/* StretchRect between two surfaces on the GPU (whole surfaces, scaled) */
void vk_copy_surface(uint32_t src, uint32_t dst) {
    VkImageView v;
    int sw, sh, dw, dh;
    GpuImage *s = vk_target(src, &v, &sw, &sh), *d = vk_target(dst, &v, &dw, &dh);
    if (!s || !d || s->depth != d->depth) return;
    D3DObj *so = d3d_obj(src), *dob = d3d_obj(dst);
    vk_end_pass();
    VkImageBlit bl = {0};
    VkImageAspectFlags asp = s->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    bl.srcSubresource = (VkImageSubresourceLayers){asp, so->parent ? so->level : 0, so->parent ? so->face : 0, 1};
    bl.srcOffsets[1] = (VkOffset3D){sw, sh, 1};
    bl.dstSubresource = (VkImageSubresourceLayers){asp, dob->parent ? dob->level : 0, dob->parent ? dob->face : 0, 1};
    bl.dstOffsets[1] = (VkOffset3D){dw, dh, 1};
    vkCmdBlitImage(vk.cmd, s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &bl,
                   s->depth ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    vk_barrier();
}

void vk_res_init(void) {}
