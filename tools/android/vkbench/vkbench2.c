/* vkbench2: what else, besides the centroid colour (vkbench.c), makes the game's sprites slower than plain layers?
 * Draws LAYERS blended layers covering a 1280x720 RGBA8 target (texture x colour, no centroid) and lets each
 * difference from the game be switched on alone:
 *   tex=small|bc3      a 256x256 RGBA8 clear texture, or a 2048x2048 BC3 (DXT5) with all its mips, ~1 texel a pixel
 *   grid=<n>           each layer cut into n x n cells (2 triangles each) instead of one quad
 *   draws=<n>          the layers' cells spread over n draws a frame, each with its own texture binding
 *   depth=0|1          a D24S8 attachment loaded and stored (no depth test), as DXVK binds the game's depth buffer
 *   passes=<n>         the frame split into n render passes (colour and depth LOAD after the first)
 * Prints ms a frame (the GPU's time: submit and wait, warmed up). Run from adb shell (no window). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include "quad2.vert.h"
#include "plain.frag.h"
#include "kgsl_counters.h"

#define W 1280
#define H 720
#define CK(x) do { VkResult r_ = (x); if (r_) { fprintf(stderr, "%s failed: %d\n", #x, r_); exit(1); } } while (0)
static VkDevice dev; static VkPhysicalDevice phys; static VkQueue q;
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags f) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & f) == f) return i;
    exit(2);
}
static VkDeviceMemory bind_image(VkImage im) {
    VkMemoryRequirements r; vkGetImageMemoryRequirements(dev, im, &r);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, r.size, memtype(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory m; CK(vkAllocateMemory(dev, &ai, 0, &m)); CK(vkBindImageMemory(dev, im, m, 0)); return m;
}
static VkImageCreateFlags g_flags;
static VkImage image(VkFormat fmt, uint32_t w, uint32_t h, uint32_t mips, VkImageUsageFlags u, VkImageAspectFlags asp, VkImageView *view) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, g_flags, VK_IMAGE_TYPE_2D, fmt, {w, h, 1}, mips, 1, 1,
                            VK_IMAGE_TILING_OPTIMAL, u, 0, 0, 0, 0};
    VkImage im; CK(vkCreateImage(dev, &ci, 0, &im)); bind_image(im);
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, im, VK_IMAGE_VIEW_TYPE_2D, fmt, {0}, {asp, 0, mips, 0, 1}};
    CK(vkCreateImageView(dev, &vi, 0, view));
    return im;
}
static VkShaderModule mod(const unsigned char *c, unsigned n) {
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, n, (const uint32_t *)c};
    VkShaderModule m; CK(vkCreateShaderModule(dev, &ci, 0, &m)); return m;
}
static const char *opt(int argc, char **argv, const char *k, const char *def) {
    size_t n = strlen(k);
    for (int i = 1; i < argc; i++) if (!strncmp(argv[i], k, n) && argv[i][n] == '=') return argv[i] + n + 1;
    return def;
}
int main(int argc, char **argv) {
    int layers = atoi(opt(argc, argv, "layers", "14")), frames = atoi(opt(argc, argv, "frames", "60"));
    const char *texs = opt(argc, argv, "tex", "small");
    int bc3 = !strcmp(texs, "bc3"), big = !strcmp(texs, "rgba"), ntex = atoi(opt(argc, argv, "ntex", "8")), grid = atoi(opt(argc, argv, "grid", "1"));
    int bgra = !strcmp(opt(argc, argv, "rt", "rgba"), "bgra"), mutable_ = atoi(opt(argc, argv, "mutable", "0")), rtuse = atoi(opt(argc, argv, "rtuse", "0"));
    int draws = atoi(opt(argc, argv, "draws", "0")), depth = atoi(opt(argc, argv, "depth", "0")), passes = atoi(opt(argc, argv, "passes", "1"));
    if (grid < 1) grid = 1;
    if (passes < 1) passes = 1;
    int cells = grid * grid, total = layers * cells;               /* cells a frame */
    if (draws < layers) draws = layers;
    if (draws > total) draws = total;
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "vkbench2", 1, 0, 0, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app};
    VkInstance inst; CK(vkCreateInstance(&ii, 0, &inst));
    uint32_t n = 1; CK(vkEnumeratePhysicalDevices(inst, &n, &phys) < 0);
    VkPhysicalDeviceFeatures have; vkGetPhysicalDeviceFeatures(phys, &have);
    VkPhysicalDeviceFeatures want = {0}; want.textureCompressionBC = have.textureCompressionBC;
    if (bc3 && !have.textureCompressionBC) { fprintf(stderr, "no BC on this driver\n"); return 1; }
    float pr = 1; VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &pr};
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0, 0, 1, &qi, 0, 0, 0, 0, &want};
    CK(vkCreateDevice(phys, &di, 0, &dev)); vkGetDeviceQueue(dev, 0, 0, &q);
    VkImageView rtv, dsv = 0, texv[8];
    VkFormat rtf = bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    g_flags = mutable_ ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;      /* as DXVK makes D3D9 render targets (sRGB views) */
    image(rtf, W, H, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (rtuse ? VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
          VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0), VK_IMAGE_ASPECT_COLOR_BIT, &rtv);
    g_flags = 0;
    if (depth) image(VK_FORMAT_D24_UNORM_S8_UINT, W, H, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                     VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, &dsv);
    /* textures: 8 of them (draws alternate), filled once */
    uint32_t tw = (bc3 || big) ? 2048 : 256, mips = (bc3 || big) ? 12 : 1;
    if (ntex < 1 || ntex > 8) ntex = 8;
    VkFormat tf = bc3 ? VK_FORMAT_BC3_UNORM_BLOCK : VK_FORMAT_R8G8B8A8_UNORM;
    VkImage tex[8];
    for (int t = 0; t < 8; t++) tex[t] = image(tf, tw, tw, mips, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, &texv[t]);
    VkDeviceSize staging = bc3 ? (VkDeviceSize)tw * tw * 2 : big ? (VkDeviceSize)tw * tw * 4 : 16;      /* BC3: 1 byte a texel; mips fit in 2x */
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, staging, VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    VkBuffer sb; CK(vkCreateBuffer(dev, &bci, 0, &sb));
    VkMemoryRequirements br; vkGetBufferMemoryRequirements(dev, sb, &br);
    VkMemoryAllocateInfo bai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, br.size, memtype(br.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory sbm; CK(vkAllocateMemory(dev, &bai, 0, &sbm)); CK(vkBindBufferMemory(dev, sb, sbm, 0));
    uint8_t *map; CK(vkMapMemory(dev, sbm, 0, staging, 0, (void **)&map));
    uint32_t seed = 12345;
    for (VkDeviceSize i = 0; i < staging; i += 16) {       /* BC3 blocks: alpha endpoints 255/128, noisy colour blocks */
        for (int k = 0; k < 16; k++) { seed = seed * 1664525u + 1013904223u; map[i + k] = (uint8_t)(seed >> 24); }
        map[i] = 255; map[i + 1] = 128;
    }
    /* render pass: colour (and depth); the first pass clears, later ones load */
    VkRenderPass rp[2];
    for (int l = 0; l < 2; l++) {
        VkAttachmentDescription ad[2] = {
            {0, rtf, 1, l ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
             VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, l ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
            {0, VK_FORMAT_D24_UNORM_S8_UINT, 1, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_LOAD,
             VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
        VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, dr = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sd = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, 0, 1, &ar, 0, depth ? &dr : 0};
        VkRenderPassCreateInfo rpi = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, 0, 0, depth ? 2u : 1u, ad, 1, &sd};
        CK(vkCreateRenderPass(dev, &rpi, 0, &rp[l]));
    }
    VkImageView fbv[2] = {rtv, dsv};
    VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, 0, 0, rp[0], depth ? 2u : 1u, fbv, W, H, 1};
    VkFramebuffer fb; CK(vkCreateFramebuffer(dev, &fi, 0, &fb));
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, 0, 0, VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR,
                              VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT};
    si.maxLod = 16;
    VkSampler smp; CK(vkCreateSampler(dev, &si, 0, &smp));
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo dli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 1, &b};
    VkDescriptorSetLayout dsl; CK(vkCreateDescriptorSetLayout(dev, &dli, 0, &dsl));
    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &dsl, 1, &pcr};
    VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev, &pli, 0, &pl));
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, 0, 8, 1, &ps};
    VkDescriptorPool dp; CK(vkCreateDescriptorPool(dev, &dpi, 0, &dp));
    VkDescriptorSet ds[8];
    for (int t = 0; t < 8; t++) {
        VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, dp, 1, &dsl};
        CK(vkAllocateDescriptorSets(dev, &dai, &ds[t]));
        VkDescriptorImageInfo dii = {smp, texv[t], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet wd = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, ds[t], 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &dii};
        vkUpdateDescriptorSets(dev, 1, &wd, 0, 0);
    }
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0};
    VkCommandPool cp; CK(vkCreateCommandPool(dev, &cpi, 0, &cp));
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, cp, 0, 1};
    VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence; CK(vkCreateFence(dev, &fci, 0, &fence));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkSubmitInfo sub = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb};
    /* upload the textures (and put the depth buffer in its layout) */
    CK(vkBeginCommandBuffer(cb, &bi));
    for (int t = 0; t < 8; t++) {
        VkImageMemoryBarrier imb = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ~0u, ~0u, tex[t], {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1}};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &imb);
        if (bc3 || big) {
            for (uint32_t m = 0; m < mips; m++) {
                uint32_t s = tw >> m; if (!s) s = 1;
                VkBufferImageCopy c = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1}, {0, 0, 0}, {s, s, 1}};
                vkCmdCopyBufferToImage(cb, sb, tex[t], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            }
        } else {
            VkClearColorValue cc = {{0.6f, 0.7f, 0.5f, 0.8f}}; VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cb, tex[t], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &rr);
        }
        imb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; imb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        imb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; imb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, 0, 0, 0, 1, &imb);
    }
    CK(vkEndCommandBuffer(cb));
    CK(vkQueueSubmit(q, 1, &sub, fence)); vkWaitForFences(dev, 1, &fence, 1, ~0ull); vkResetFences(dev, 1, &fence);
    if (depth) {    /* the depth buffer into its attachment layout once: a pass that clears it */
        VkAttachmentDescription ad = {0, VK_FORMAT_D24_UNORM_S8_UINT, 1, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
            VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkAttachmentReference dr = {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sd = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, 0, 0, 0, 0, &dr};
        VkRenderPassCreateInfo rpi = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, 0, 0, 1, &ad, 1, &sd};
        VkRenderPass zp; CK(vkCreateRenderPass(dev, &rpi, 0, &zp));
        VkFramebufferCreateInfo zfi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, 0, 0, zp, 1, &dsv, W, H, 1};
        VkFramebuffer zfb; CK(vkCreateFramebuffer(dev, &zfi, 0, &zfb));
        VkClearValue zc = {.depthStencil = {1, 0}};
        CK(vkBeginCommandBuffer(cb, &bi));
        VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, 0, zp, zfb, {{0, 0}, {W, H}}, 1, &zc};
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE); vkCmdEndRenderPass(cb);
        CK(vkEndCommandBuffer(cb));
        CK(vkQueueSubmit(q, 1, &sub, fence)); vkWaitForFences(dev, 1, &fence, 1, ~0ull); vkResetFences(dev, 1, &fence);
    }
    VkShaderModule vs = mod(quad2_vert, quad2_vert_len), fs = mod(plain_frag, plain_frag_len);
    const char *fsfile = opt(argc, argv, "fs", NULL);       /* a SPIR-V fragment shader from a file (DXVK's own, rebound) */
    if (fsfile) {
        FILE *ff = fopen(fsfile, "rb");
        if (!ff) { perror(fsfile); return 1; }
        static unsigned char buf[1 << 20]; size_t nb = fread(buf, 1, sizeof buf, ff); fclose(ff);
        fs = mod(buf, (unsigned)nb);
    }
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, 0, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport vp = {0, 0, W, H, 0, 1}; VkRect2D sc = {{0, 0}, {W, H}};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0, 1, &vp, 1, &sc};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, 0, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo dss = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState ba = {1, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
                                              VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, 15};
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, 0, 0, 0, 0, 1, &ba};
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, 0, 0, 2, st, &vi, &ia, 0, &vps, &rs, &ms,
                                       depth ? &dss : 0, &cbs, 0, pl, rp[0]};
    VkPipeline pipe; CK(vkCreateGraphicsPipelines(dev, 0, 1, &gi, 0, &pipe));
    int per_draw = (total + draws - 1) / draws;          /* cells a draw */
    double t0 = 0;
    kc kk; uint64_t c0[MAXC] = {0}, c1[MAXC] = {0};
    const char *clist = opt(argc, argv, "counters", NULL);
    int kon = clist && kc_open(&kk, clist);
    for (int f = 0; f <= frames; f++) {
        if (f == 1) { t0 = now(); if (kon) kc_read(&kk, c0); }
        CK(vkBeginCommandBuffer(cb, &bi));
        int cell = 0, d = 0;
        for (int p = 0; p < passes; p++) {
            VkClearValue cv[2] = {{{{0, 0, 0, 1}}}, {.depthStencil = {1, 0}}};
            VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, 0, rp[p ? 1 : 0], fb, {{0, 0}, {W, H}}, depth ? 2u : 1u, cv};
            vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            int end = total * (p + 1) / passes;
            while (cell < end) {
                int c = per_draw < end - cell ? per_draw : end - cell;
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds[d % ntex], 0, 0);
                /* push: grid size, the texture's scale (about a texel a pixel), and the first cell's layer */
                float pc[4] = {(float)grid, (bc3 || big) ? 1280.0f / 2048.0f : 3.0f, (float)(cell / cells), (float)cells};
                vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, pc);
                vkCmdDraw(cb, 6u * (uint32_t)c, 1, 6u * (uint32_t)(cell % cells), 0);
                cell += c; d++;
            }
            vkCmdEndRenderPass(cb);
        }
        CK(vkEndCommandBuffer(cb));
        CK(vkQueueSubmit(q, 1, &sub, fence)); vkWaitForFences(dev, 1, &fence, 1, ~0ull); vkResetFences(dev, 1, &fence);
    }
    double tms = (now() - t0) / frames;
    if (kon) {
        kc_read(&kk, c1);
        double px = (double)layers * W * H;
        for (unsigned i = 1; i < kk.nc; i++)
            printf("  %u:%u %10.3f M/frame  %.3f per pixel\n", kk.grp[i], kk.cnt[i], (double)(c1[i] - c0[i]) / frames / 1e6,
                   (double)(c1[i] - c0[i]) / frames / px);
        printf("  GPU cycles %.2f M/frame\n", (double)(c1[0] - c0[0]) / frames / 1e6);
    }
    printf("fs=%s rt=%s mutable=%d rtuse=%d tex=%-5s ntex=%d grid=%-3d draws=%-4d depth=%d passes=%d: %6.2f ms/frame\n", fsfile ? fsfile : "glsl", bgra ? "bgra" : "rgba", mutable_, rtuse, texs, ntex, grid, draws, depth, passes, tms);
    return 0;
}
