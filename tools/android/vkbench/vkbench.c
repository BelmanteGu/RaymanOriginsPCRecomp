/* vkbench: does centroid interpolation of the vertex colour cost on this GPU without MSAA?
 * Draws LAYERS blended full-screen quads (texture x colour, as Rayman's sprite shader) into a 1280x720 RGBA8 image,
 * FRAMES times, with three fragment shaders: colour centroid (what DXVK emits for ps_2_0), colour plain, no colour.
 * Run from adb shell (no window). Prints ms per frame for each. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include "quad.vert.h"
#include "centroid.frag.h"
#include "plain.frag.h"
#include "nocolor.frag.h"

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
static VkImage image(VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags u, VkImageView *view) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, 0, VK_IMAGE_TYPE_2D, fmt, {w, h, 1}, 1, 1, 1,
                            VK_IMAGE_TILING_OPTIMAL, u, 0, 0, 0, 0};
    VkImage im; CK(vkCreateImage(dev, &ci, 0, &im));
    VkMemoryRequirements r; vkGetImageMemoryRequirements(dev, im, &r);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, r.size, memtype(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory m; CK(vkAllocateMemory(dev, &ai, 0, &m)); CK(vkBindImageMemory(dev, im, m, 0));
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, im, VK_IMAGE_VIEW_TYPE_2D, fmt, {0},
                                {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    CK(vkCreateImageView(dev, &vi, 0, view));
    return im;
}
static VkShaderModule mod(const unsigned char *c, unsigned n) {
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, n, (const uint32_t *)c};
    VkShaderModule m; CK(vkCreateShaderModule(dev, &ci, 0, &m)); return m;
}
int main(int argc, char **argv) {
    int layers = argc > 1 ? atoi(argv[1]) : 14, frames = argc > 2 ? atoi(argv[2]) : 60;
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "vkbench", 1, 0, 0, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app};
    VkInstance inst; CK(vkCreateInstance(&ii, 0, &inst));
    uint32_t n = 1; CK(vkEnumeratePhysicalDevices(inst, &n, &phys) < 0);
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(phys, &pp);
    printf("%s, driver %08x\n", pp.deviceName, pp.driverVersion);
    float pr = 1; VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &pr};
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0, 0, 1, &qi};
    CK(vkCreateDevice(phys, &di, 0, &dev)); vkGetDeviceQueue(dev, 0, 0, &q);
    VkImageView rtv, texv;
    VkImage rt = image(VK_FORMAT_R8G8B8A8_UNORM, W, H, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &rtv);
    VkImage tex = image(VK_FORMAT_R8G8B8A8_UNORM, 256, 256, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &texv);
    VkAttachmentDescription ad = {0, VK_FORMAT_R8G8B8A8_UNORM, 1, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                  VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sd = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, 0, 1, &ar};
    VkRenderPassCreateInfo rpi = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, 0, 0, 1, &ad, 1, &sd};
    VkRenderPass rp; CK(vkCreateRenderPass(dev, &rpi, 0, &rp));
    VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, 0, 0, rp, 1, &rtv, W, H, 1};
    VkFramebuffer fb; CK(vkCreateFramebuffer(dev, &fi, 0, &fb));
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, 0, 0, VK_FILTER_LINEAR, VK_FILTER_LINEAR};
    VkSampler smp; CK(vkCreateSampler(dev, &si, 0, &smp));
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo dli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 1, &b};
    VkDescriptorSetLayout dsl; CK(vkCreateDescriptorSetLayout(dev, &dli, 0, &dsl));
    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT, 0, 4};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &dsl, 1, &pcr};
    VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev, &pli, 0, &pl));
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, 0, 1, 1, &ps};
    VkDescriptorPool dp; CK(vkCreateDescriptorPool(dev, &dpi, 0, &dp));
    VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, dp, 1, &dsl};
    VkDescriptorSet ds; CK(vkAllocateDescriptorSets(dev, &dai, &ds));
    VkDescriptorImageInfo dii = {smp, texv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wd = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &dii};
    vkUpdateDescriptorSets(dev, 1, &wd, 0, 0);
    VkShaderModule vs = mod(quad_vert, quad_vert_len);
    struct { const char *name; VkShaderModule fs; } v[3] = {{"cor centroid (DXVK ps_2_0)", mod(centroid_frag, centroid_frag_len)},
        {"cor sem centroid", mod(plain_frag, plain_frag_len)}, {"sem cor (shader minimo)", mod(nocolor_frag, nocolor_frag_len)}};
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0};
    VkCommandPool cp; CK(vkCreateCommandPool(dev, &cpi, 0, &cp));
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, cp, 0, 1};
    VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence; CK(vkCreateFence(dev, &fci, 0, &fence));
    /* the texture: a clear colour, then shader-readable */
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CK(vkBeginCommandBuffer(cb, &bi));
    VkImageMemoryBarrier imb = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ~0u, ~0u, tex, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &imb);
    VkClearColorValue cc = {{0.6f, 0.7f, 0.5f, 0.8f}}; VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &rr);
    imb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; imb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    imb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; imb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, 0, 0, 0, 1, &imb);
    CK(vkEndCommandBuffer(cb));
    VkSubmitInfo sub = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb};
    CK(vkQueueSubmit(q, 1, &sub, fence)); vkWaitForFences(dev, 1, &fence, 1, ~0ull); vkResetFences(dev, 1, &fence);
    for (int round = 0; round < 2; round++)
    for (int k = 0; k < 3; k++) {
        VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, v[k].fs, "main"}};
        VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, 0, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP};
        VkViewport vp = {0, 0, W, H, 0, 1}; VkRect2D sc = {{0, 0}, {W, H}};
        VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0, 1, &vp, 1, &sc};
        VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, 0, 0, VK_SAMPLE_COUNT_1_BIT};
        VkPipelineColorBlendAttachmentState ba = {1, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
                                                  VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, 15};
        VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, 0, 0, 0, 0, 1, &ba};
        VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, 0, 0, 2, st, &vi, &ia, 0, &vps, &rs, &ms, 0, &cbs, 0, pl, rp};
        VkPipeline pipe; CK(vkCreateGraphicsPipelines(dev, 0, 1, &gi, 0, &pipe));
        double t0 = 0;
        for (int f = 0; f <= frames; f++) {
            if (f == 1) t0 = now();                              /* frame 0 warms up */
            CK(vkBeginCommandBuffer(cb, &bi));
            VkClearValue cv = {{{0, 0, 0, 1}}};
            VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, 0, rp, fb, {{0, 0}, {W, H}}, 1, &cv};
            vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds, 0, 0);
            for (int l = 0; l < layers; l++) { float lf = (float)l; vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 4, &lf); vkCmdDraw(cb, 4, 1, 0, 0); }
            vkCmdEndRenderPass(cb);
            CK(vkEndCommandBuffer(cb));
            CK(vkQueueSubmit(q, 1, &sub, fence)); vkWaitForFences(dev, 1, &fence, 1, ~0ull); vkResetFences(dev, 1, &fence);
        }
        double ms_ = (now() - t0) / frames;
        if (round) printf("%-28s %6.2f ms/frame  (%d camadas 1280x720 = %.1f Mpx, %.0f Mpx/s)\n", v[k].name, ms_, layers,
                          layers * W * H / 1e6, layers * W * H / 1e3 / ms_);
        vkDestroyPipeline(dev, pipe, 0);
    }
    return 0;
}
