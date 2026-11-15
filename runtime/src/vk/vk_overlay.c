/* The touch layer on the GPU: touch.c's RGBA picture blended over the game's back buffer before it is presented.
 *
 * What it does: one image the size of the back buffer (re-uploaded through the frame's upload ring only when the
 * picture changes), a render pass that loads the back buffer, and a pipeline that draws one full-screen triangle
 * sampling the picture with premultiplied-alpha blending. Everything is created on first use and again when the back
 * buffer changes size.
 * What it does not do: anything when the layer is off (touch_picture returns NULL): no cost then.
 */
#include <shaderc/shaderc.h>
#include <stdlib.h>
#include <string.h>

#include "vk_int.h"

const uint8_t *touch_picture(int w, int h, int *changed);

static VkRenderPass rp;
static VkPipeline pipe;
static VkPipelineLayout layout;
static VkDescriptorSetLayout dsl;
static VkDescriptorPool pool;
static VkDescriptorSet ds;
static VkSampler samp;
static VkImage img;
static VkImageView view;
static VkDeviceMemory mem;
static VkFramebuffer fb;
static VkImageView fb_view;
static uint32_t ow, oh;
static VkFormat bb_fmt;

static VkShaderModule compile(const char *src, shaderc_shader_kind kind) {
    shaderc_compiler_t sc = shaderc_compiler_initialize();
    shaderc_compile_options_t opt = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(opt, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);
    shaderc_compilation_result_t r = shaderc_compile_into_spv(sc, src, strlen(src), kind, "overlay", "main", opt);
    if (shaderc_result_get_compilation_status(r) != shaderc_compilation_status_success)
        hle_fatal("overlay shader: %s", shaderc_result_get_error_message(r));
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = shaderc_result_get_length(r);
    ci.pCode = (const uint32_t *)shaderc_result_get_bytes(r);
    VkShaderModule m;
    VKC(vkCreateShaderModule(vk.dev, &ci, NULL, &m));
    shaderc_result_release(r);
    shaderc_compile_options_release(opt);
    shaderc_compiler_release(sc);
    return m;
}

static void make_pipeline(VkFormat fmt) {
    VkAttachmentDescription at = {0, fmt, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
                                  VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                  VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription sub = {0};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkRenderPassCreateInfo rci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rci.attachmentCount = 1; rci.pAttachments = &at; rci.subpassCount = 1; rci.pSubpasses = &sub;
    VKC(vkCreateRenderPass(vk.dev, &rci, NULL, &rp));

    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dci.bindingCount = 1; dci.pBindings = &b;
    VKC(vkCreateDescriptorSetLayout(vk.dev, &dci, NULL, &dsl));
    VkPipelineLayoutCreateInfo pci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pci.setLayoutCount = 1; pci.pSetLayouts = &dsl;
    VKC(vkCreatePipelineLayout(vk.dev, &pci, NULL, &layout));
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 1; pi.poolSizeCount = 1; pi.pPoolSizes = &ps;
    VKC(vkCreateDescriptorPool(vk.dev, &pi, NULL, &pool));
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &dsl;
    VKC(vkAllocateDescriptorSets(vk.dev, &ai, &ds));
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VKC(vkCreateSampler(vk.dev, &sci, NULL, &samp));

    VkShaderModule vs = compile("#version 450\nlayout(location=0) out vec2 uv;\nvoid main(){ uv = vec2((gl_VertexIndex << 1) & 2,"
                                " gl_VertexIndex & 2); gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0); }\n", shaderc_vertex_shader);
    VkShaderModule fs = compile("#version 450\nlayout(location=0) in vec2 uv;\nlayout(location=0) out vec4 o;\n"
                                "layout(set=0,binding=0) uniform sampler2D t;\nvoid main(){ o = texture(t, uv); }\n",
                                shaderc_fragment_shader);
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba = {VK_TRUE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                              VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD, 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &ba;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gci.stageCount = 2; gci.pStages = st; gci.pVertexInputState = &vi; gci.pInputAssemblyState = &ia;
    gci.pViewportState = &vp; gci.pRasterizationState = &rs; gci.pMultisampleState = &ms; gci.pColorBlendState = &cb;
    gci.pDynamicState = &dy; gci.layout = layout; gci.renderPass = rp;
    VKC(vkCreateGraphicsPipelines(vk.dev, VK_NULL_HANDLE, 1, &gci, NULL, &pipe));
    vkDestroyShaderModule(vk.dev, vs, NULL);
    vkDestroyShaderModule(vk.dev, fs, NULL);
    bb_fmt = fmt;
}

static void make_image(uint32_t w, uint32_t h) {
    if (img) { vkDeviceWaitIdle(vk.dev); vkDestroyImageView(vk.dev, view, NULL); vkDestroyImage(vk.dev, img, NULL); vkFreeMemory(vk.dev, mem, NULL); }
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = VK_FORMAT_R8G8B8A8_UNORM; ci.extent = (VkExtent3D){w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VKC(vkCreateImage(vk.dev, &ci, NULL, &img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk.dev, img, &req);
    mem = vk_alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
    VKC(vkBindImageMemory(vk.dev, img, mem, 0));
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = ci.format;
    vci.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VKC(vkCreateImageView(vk.dev, &vci, NULL, &view));
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img; b.subresourceRange = vci.subresourceRange;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(vk.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkDescriptorImageInfo ii = {samp, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = ds; wr.descriptorCount = 1; wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(vk.dev, 1, &wr, 0, NULL);
    ow = w; oh = h;
}

/* Called by r_present after the frame's last draw, before the back buffer goes to the screen. */
void vk_overlay_draw(void);
void vk_overlay_draw(void) {
    GpuImage *bb = &g_backbuffer_img;
    if (!bb->img || !bb->att[0]) return;
    int changed = 0;
    const uint8_t *pic = touch_picture((int)bb->w, (int)bb->h, &changed);
    if (!pic) return;
    vk_end_pass();
    if (!pipe) make_pipeline(bb->fmt);
    if (ow != bb->w || oh != bb->h) { make_image(bb->w, bb->h); changed = 1; }
    if (changed) {
        uint32_t size = bb->w * bb->h * 4;
        vk_ring_reserve(size);
        void *dst;
        uint32_t off = vk_ring_alloc(size, 16, &dst);
        memcpy(dst, pic, size);
        VkBufferImageCopy cp = {0};
        cp.bufferOffset = off;
        cp.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        cp.imageExtent = (VkExtent3D){bb->w, bb->h, 1};
        vkCmdCopyBufferToImage(vk.cmd, vk.ring, img, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
        vk_barrier();
    }
    if (fb_view != bb->att[0]) {
        if (fb) { vkDeviceWaitIdle(vk.dev); vkDestroyFramebuffer(vk.dev, fb, NULL); }
        VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = rp; fci.attachmentCount = 1; fci.pAttachments = &bb->att[0];
        fci.width = bb->w; fci.height = bb->h; fci.layers = 1;
        VKC(vkCreateFramebuffer(vk.dev, &fci, NULL, &fb));
        fb_view = bb->att[0];
    }
    VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = rp; bi.framebuffer = fb; bi.renderArea.extent = (VkExtent2D){bb->w, bb->h};
    vkCmdBeginRenderPass(vk.cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport v = {0, 0, (float)bb->w, (float)bb->h, 0, 1};
    VkRect2D sc = {{0, 0}, {bb->w, bb->h}};
    vkCmdSetViewport(vk.cmd, 0, 1, &v);
    vkCmdSetScissor(vk.cmd, 0, 1, &sc);
    vkCmdBindPipeline(vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &ds, 0, NULL);
    vkCmdDraw(vk.cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(vk.cmd);
    vk_barrier();
    (void)bb_fmt;
}
