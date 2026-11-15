/* Vulkan core: instance, device, memory, the per-frame command buffer / descriptor pool / upload ring, and Present.
 *
 * What it does: opens Vulkan 1.1 on the SDL window (optional features such as BC textures, anisotropy, independent
 * blend are enabled only when the device has them, so every phone GPU can run it); sub-allocates device memory in
 * 64 MiB blocks; keeps FRAMES frames in flight, each with its own command buffer, descriptor pool and a host-visible
 * ring for vertices, indices, constants and texture uploads; Present ends the frame by blitting the game's back
 * buffer to the swap chain image (scaled to the window) and starts the next frame.
 * What it does not do: free device memory (resources live until exit for now) or recreate the device.
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vk_int.h"
#include "../movie.h"

extern SDL_Window *g_window;
Vk vk;

static VkSurfaceKHR surf;
static VkSwapchainKHR swap;
static VkImage sw_img[8];
static uint32_t nsw;
static VkExtent2D sw_ext;
static VkCommandPool cpool;
static VkCommandBuffer cmds[FRAMES];
static VkSemaphore sem_acq[FRAMES], sem_done[FRAMES];
static VkFence fences[FRAMES];
static VkDescriptorPool dpools[FRAMES];
static VkBuffer rings[FRAMES];
static uint8_t *ring_ptrs[FRAMES];
static int ready;

/* ---- memory ---- */
typedef struct { VkDeviceMemory mem; uint32_t type; VkDeviceSize used; void *map; } Block;
static Block blocks[256];
static int nblocks;
#define BLOCK (64ull << 20)

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < vk.mem.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (vk.mem.memoryTypes[i].propertyFlags & flags) == flags) return i;
    hle_fatal("no Vulkan memory type for %x", flags);
}

VkDeviceMemory vk_alloc(VkMemoryRequirements req, VkMemoryPropertyFlags flags, void **map) {
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = mem_type(req.memoryTypeBits, flags);
    VkDeviceMemory m;
    VKC(vkAllocateMemory(vk.dev, &ai, NULL, &m));
    if (map) VKC(vkMapMemory(vk.dev, m, 0, VK_WHOLE_SIZE, 0, map));
    return m;
}

/* images: placed in shared blocks (offset returned through *off) */
static VkDeviceMemory block_alloc(VkMemoryRequirements req, VkDeviceSize *off) {
    uint32_t type = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (req.size > BLOCK / 2) { *off = 0; return vk_alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL); }
    for (int i = 0; i < nblocks; i++) {
        if (blocks[i].type != type) continue;
        VkDeviceSize o = (blocks[i].used + req.alignment - 1) & ~(req.alignment - 1);
        if (o + req.size <= BLOCK) { blocks[i].used = o + req.size; *off = o; return blocks[i].mem; }
    }
    if (nblocks >= 256) hle_fatal("out of GPU memory blocks");
    VkMemoryRequirements r2 = req;
    r2.size = BLOCK;
    blocks[nblocks].mem = vk_alloc(r2, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
    blocks[nblocks].type = type;
    blocks[nblocks].used = req.size;
    *off = 0;
    return blocks[nblocks++].mem;
}

void vk_bind_image(VkImage img);
void vk_bind_image(VkImage img) {
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk.dev, img, &req);
    VkDeviceSize off;
    VkDeviceMemory m = block_alloc(req, &off);
    VKC(vkBindImageMemory(vk.dev, img, m, off));
}

/* Submits what was recorded so far and waits, so the ring and the descriptor pool can be reused within a frame
 * (loading screens upload more than one ring holds). */
void vk_flush(void) {
    vk_end_pass();
    vkEndCommandBuffer(vk.cmd);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &vk.cmd;
    VKC(vkQueueSubmit(vk.queue, 1, &si, VK_NULL_HANDLE));
    vkQueueWaitIdle(vk.queue);
    vk.ring_used = 0;
    vkResetDescriptorPool(vk.dev, vk.dpool, 0);
    vkResetCommandBuffer(vk.cmd, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(vk.cmd, &bi);
    vk_draw_frame_start();
}

void vk_ring_reserve(uint32_t size) {
    if (vk.ring_used + size + 4096 > RING_SIZE) vk_flush();
    if (size + 4096 > RING_SIZE) hle_fatal("an upload of %u bytes does not fit the ring", size);
}

uint32_t vk_ring_alloc(uint32_t size, uint32_t align, void **ptr) {
    if (align < 16) align = 16;
    uint32_t o = (vk.ring_used + align - 1) & ~(align - 1);
    if (o + size > RING_SIZE) hle_fatal("upload ring full (%u bytes this frame)", o + size);
    vk.ring_used = o + size;
    if (ptr) *ptr = vk.ring_ptr + o;
    return o;
}

void vk_barrier(void) {
    VkMemoryBarrier b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(vk.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, NULL,
                         0, NULL);
}

void vk_end_pass(void) {
    if (vk.in_pass) { vkCmdEndRenderPass(vk.cmd); vk.in_pass = 0; }
    vk_barrier();
}

/* ---- swap chain ---- */
static void make_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.gpu, surf, &caps);
    sw_ext = caps.currentExtent;
    if (sw_ext.width == 0xFFFFFFFFu) { int w, h; SDL_GetWindowSizeInPixels(g_window, &w, &h); sw_ext.width = (uint32_t)w; sw_ext.height = (uint32_t)h; }
    /* Android: the surface comes in the panel's own (portrait) orientation with a 90/270 rotation; we draw the
     * landscape picture as it is and let the compositor rotate it (identity transform, width and height swapped). */
    VkSurfaceTransformFlagBitsKHR xf = caps.currentTransform;
    if (xf & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)) {
        uint32_t t = sw_ext.width; sw_ext.width = sw_ext.height; sw_ext.height = t;
    }
    if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) xf = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    uint32_t nf = 64;
    VkSurfaceFormatKHR fmts[64];
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk.gpu, surf, &nf, fmts);
    VkSurfaceFormatKHR fmt = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) { fmt = fmts[i]; break; }
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surf;
    ci.minImageCount = caps.maxImageCount && caps.minImageCount + 1 > caps.maxImageCount ? caps.minImageCount : caps.minImageCount + 1;
    ci.imageFormat = fmt.format;
    ci.imageColorSpace = fmt.colorSpace;
    ci.imageExtent = sw_ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.preTransform = xf;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = getenv("USF_NOVSYNC") ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swap;
    VkSwapchainKHR ns;
    VKC(vkCreateSwapchainKHR(vk.dev, &ci, NULL, &ns));
    if (swap) vkDestroySwapchainKHR(vk.dev, swap, NULL);
    swap = ns;
    nsw = 8;
    vkGetSwapchainImagesKHR(vk.dev, swap, &nsw, sw_img);
}

static void begin_frame(void) {
    uint32_t f = vk.frame % FRAMES;
    vkWaitForFences(vk.dev, 1, &fences[f], VK_TRUE, UINT64_MAX);
    vk.cmd = cmds[f];
    vk.dpool = dpools[f];
    vk.ring = rings[f];
    vk.ring_ptr = ring_ptrs[f];
    vk.ring_used = 0;
    vkResetDescriptorPool(vk.dev, vk.dpool, 0);
    vkResetCommandBuffer(vk.cmd, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(vk.cmd, &bi);
    vk_draw_frame_start();
}

static void pick_device(void) {
    uint32_t n = 8;
    VkPhysicalDevice gpus[8];
    vkEnumeratePhysicalDevices(vk.inst, &n, gpus);
    int best = -1, best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties pr;
        vkGetPhysicalDeviceProperties(gpus[i], &pr);
        uint32_t nq = 16;
        VkQueueFamilyProperties qp[16];
        vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &nq, qp);
        for (uint32_t q = 0; q < nq; q++) {
            VkBool32 present = 0;
            vkGetPhysicalDeviceSurfaceSupportKHR(gpus[i], q, surf, &present);
            if (q == 0) fprintf(stderr, "usf: Vulkan device %u: %s, family 0 present=%u\n", i, pr.deviceName, present);
            if (!(qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            int score = pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (getenv("USF_GPU") && strstr(pr.deviceName, getenv("USF_GPU"))) score = 10;
            if (score > best_score) { best_score = score; best = (int)i; vk.qfam = q; }
            break;
        }
    }
    if (best < 0) hle_fatal("no Vulkan device can present to the window");
    vk.gpu = gpus[best];
    vkGetPhysicalDeviceProperties(vk.gpu, &vk.props);
    vkGetPhysicalDeviceFeatures(vk.gpu, &vk.feats);
    {   /* SCALED vertex formats are optional (most phones lack them): those D3D types are widened to floats */
        static const struct { uint32_t t; VkFormat f; } sc[] = {{4, VK_FORMAT_B8G8R8A8_UNORM}, {5, VK_FORMAT_R8G8B8A8_USCALED},
            {6, VK_FORMAT_R16G16_SSCALED}, {7, VK_FORMAT_R16G16B16A16_SSCALED}, {8, VK_FORMAT_R8G8B8A8_UNORM},
            {9, VK_FORMAT_R16G16_SNORM}, {10, VK_FORMAT_R16G16B16A16_SNORM}, {11, VK_FORMAT_R16G16_UNORM},
            {12, VK_FORMAT_R16G16B16A16_UNORM}, {13, VK_FORMAT_A2B10G10R10_USCALED_PACK32},
            {14, VK_FORMAT_A2B10G10R10_SNORM_PACK32}, {15, VK_FORMAT_R16G16_SFLOAT}, {16, VK_FORMAT_R16G16B16A16_SFLOAT}};
        for (unsigned i = 0; i < sizeof sc / sizeof sc[0]; i++) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(vk.gpu, sc[i].f, &fp);
            if (!(fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) vk.vtx_widen |= 1u << sc[i].t;
        }
        if (getenv("USF_VTX_WIDEN")) vk.vtx_widen |= (1u << 5) | (1u << 6) | (1u << 7) | (1u << 13);   /* test on the PC */
        if (vk.vtx_widen) fprintf(stderr, "usf: vertex types widened to float on the CPU: mask %x\n", vk.vtx_widen);
    }
    vkGetPhysicalDeviceMemoryProperties(vk.gpu, &vk.mem);
    fprintf(stderr, "usf: Vulkan device: %s (API %u.%u)\n", vk.props.deviceName, VK_API_VERSION_MAJOR(vk.props.apiVersion),
            VK_API_VERSION_MINOR(vk.props.apiVersion));
}

int r_init(uint32_t w, uint32_t h) {
    (void)w; (void)h;
    if (ready) return 0;
    if (!g_window) hle_fatal("r_init: no window yet");
    Uint32 next = 0;
    const char *const *exts = SDL_Vulkan_GetInstanceExtensions(&next);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "RaymanOrigins-PC";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = next;
    ici.ppEnabledExtensionNames = exts;
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    if (getenv("USF_VALIDATION")) { ici.enabledLayerCount = 1; ici.ppEnabledLayerNames = layers; }
    VKC(vkCreateInstance(&ici, NULL, &vk.inst));
    if (!SDL_Vulkan_CreateSurface(g_window, vk.inst, NULL, &surf)) hle_fatal("SDL_Vulkan_CreateSurface: %s", SDL_GetError());
    pick_device();
    VkPhysicalDeviceFeatures en = {0};                    /* only what the device has */
    en.textureCompressionBC = vk.feats.textureCompressionBC;
    en.samplerAnisotropy = vk.feats.samplerAnisotropy;
    en.independentBlend = vk.feats.independentBlend;
    en.fillModeNonSolid = vk.feats.fillModeNonSolid;
    en.depthClamp = vk.feats.depthClamp;
    en.depthBiasClamp = vk.feats.depthBiasClamp;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = vk.qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char *dext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dext;
    dci.pEnabledFeatures = &en;
    VKC(vkCreateDevice(vk.gpu, &dci, NULL, &vk.dev));
    vkGetDeviceQueue(vk.dev, vk.qfam, 0, &vk.queue);
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(vk.gpu, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
    vk.depth_format = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ? VK_FORMAT_D24_UNORM_S8_UINT
                                                                                                   : VK_FORMAT_D32_SFLOAT_S8_UINT;
    make_swapchain();
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = vk.qfam;
    VKC(vkCreateCommandPool(vk.dev, &pci, NULL, &cpool));
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = cpool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = FRAMES;
    VKC(vkAllocateCommandBuffers(vk.dev, &cai, cmds));
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 40000}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 400000}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 20000;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = ps;
    for (int i = 0; i < FRAMES; i++) {
        VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VKC(vkCreateSemaphore(vk.dev, &sci, NULL, &sem_acq[i]));
        VKC(vkCreateSemaphore(vk.dev, &sci, NULL, &sem_done[i]));
        VKC(vkCreateFence(vk.dev, &fci, NULL, &fences[i]));
        VKC(vkCreateDescriptorPool(vk.dev, &dpi, NULL, &dpools[i]));
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = RING_SIZE;
        bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VKC(vkCreateBuffer(vk.dev, &bci, NULL, &rings[i]));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(vk.dev, rings[i], &req);
        void *map;
        VkDeviceMemory m = vk_alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &map);
        VKC(vkBindBufferMemory(vk.dev, rings[i], m, 0));
        ring_ptrs[i] = map;
    }
    vk_res_init();
    vk_draw_init();
    if (!g_headless) SDL_ShowWindow(g_window);
    ready = 1;
    begin_frame();
    return 0;
}

/* USF_SHOT="300,900": the back buffer of those frames is written to private/shots/frame_<n>.ppm (for the docs) */
static VkBuffer shot_buf;
static uint8_t *shot_ptr;
static int shot_pending;

static int want_shot(uint32_t frame) {
    const char *l = getenv("USF_SHOT");
    if (!l) return 0;
    for (const char *p = l; *p;) {
        if ((uint32_t)strtoul(p, NULL, 10) == frame) return 1;
        p = strchr(p, ',');
        if (!p) break;
        p++;
    }
    return 0;
}

static void shot_record(void) {
    GpuImage *g = &g_backbuffer_img;
    if (!g->img) return;
    if (!shot_buf) {
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = 4096u * 4096u * 4u;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VKC(vkCreateBuffer(vk.dev, &bci, NULL, &shot_buf));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(vk.dev, shot_buf, &req);
        VkDeviceMemory m = vk_alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, (void **)&shot_ptr);
        VKC(vkBindBufferMemory(vk.dev, shot_buf, m, 0));
    }
    VkBufferImageCopy r = {0};
    r.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = (VkExtent3D){g->w, g->h, 1};
    vkCmdCopyImageToBuffer(vk.cmd, g->img, VK_IMAGE_LAYOUT_GENERAL, shot_buf, 1, &r);
    shot_pending = 1;
}

/* Debug: a copy of one texture as the GPU holds it (asked by the draw log), written with the frame's screenshot. */
static GpuImage *tex_dbg;
static VkBuffer tex_dbg_buf;
static uint8_t *tex_dbg_ptr;
void vk_debug_texture(GpuImage *g) {
    if (tex_dbg || !g || g->w * g->h > 2048u * 2048u) return;
    if (!tex_dbg_buf) {
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = 2048u * 2048u * 4u;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VKC(vkCreateBuffer(vk.dev, &bci, NULL, &tex_dbg_buf));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(vk.dev, tex_dbg_buf, &req);
        VkDeviceMemory m = vk_alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, (void **)&tex_dbg_ptr);
        VKC(vkBindBufferMemory(vk.dev, tex_dbg_buf, m, 0));
    }
    vk_end_pass();
    VkBufferImageCopy r = {0};
    r.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = (VkExtent3D){g->w, g->h, 1};
    vkCmdCopyImageToBuffer(vk.cmd, g->img, VK_IMAGE_LAYOUT_GENERAL, tex_dbg_buf, 1, &r);
    vk_barrier();
    tex_dbg = g;
}

static void shot_write(uint32_t frame) {
    vkQueueWaitIdle(vk.queue);
    if (tex_dbg) {
        char tp[600];
        snprintf(tp, sizeof tp, "%s/gputex_%05u_%ux%u.bgra", getenv("USF_SHOT_DIR") ? getenv("USF_SHOT_DIR") : "/tmp", frame, tex_dbg->w, tex_dbg->h);
        FILE *tf = fopen(tp, "wb");
        if (tf) { fwrite(tex_dbg_ptr, 4, tex_dbg->w * tex_dbg->h, tf); fclose(tf); }
        tex_dbg = NULL;
    }
    char path[256];
    snprintf(path, sizeof path, "%s/frame_%05u.ppm", getenv("USF_SHOT_DIR") ? getenv("USF_SHOT_DIR") : "/tmp", frame);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    GpuImage *g = &g_backbuffer_img;
    fprintf(f, "P6\n%u %u\n255\n", g->w, g->h);
    for (uint32_t i = 0; i < g->w * g->h; i++) { uint8_t px[3] = {shot_ptr[4 * i + 2], shot_ptr[4 * i + 1], shot_ptr[4 * i]}; fwrite(px, 1, 3, f); }
    fclose(f);
    fprintf(stderr, "usf: screenshot %s\n", path);
    shot_pending = 0;
}

/* While a movie plays (movie.c), its newest picture replaces the back buffer: black, then the picture scaled to fit
 * with its aspect kept. The picture goes through a texture whose memory is host-side guest memory. */
static void movie_overlay(void) {
    static D3DObj *tex;
    static unsigned seq;
    static int mw, mh;
    if (!movie_active() || !g_backbuffer_img.img) { seq = 0; mw = 0; return; }
    if (!tex) { uint32_t g = d3d_create_texture(OBJ_TEXTURE, 1920, 1088, 1, 0, 21, 1); tex = d3d_obj(g); }
    int w, h;
    if (movie_frame(HOST(tex->mem[0][0]), 1920, 1088, &w, &h, &seq)) {
        /* rows were written w wide: the texture is 1920 wide, so spread them out to its pitch (last row first) */
        uint8_t *m = HOST(tex->mem[0][0]);
        for (int y = h - 1; y > 0; y--) memmove(m + (size_t)y * 1920 * 4, m + (size_t)y * w * 4, (size_t)w * 4);
        tex->version++;
        mw = w; mh = h;
    }
    if (!mw) return;
    GpuImage *g = vk_texture(tex);
    vk_end_pass();
    VkClearColorValue black = {{0, 0, 0, 1}};
    VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(vk.cmd, g_backbuffer_img.img, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &rr);
    vk_barrier();
    int bw = (int)g_backbuffer_img.w, bh = (int)g_backbuffer_img.h;
    int dw = bw, dh = (int)((int64_t)bw * mh / mw);
    if (dh > bh) { dh = bh; dw = (int)((int64_t)bh * mw / mh); }
    VkImageBlit bl = {0};
    bl.srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.srcOffsets[1] = (VkOffset3D){mw, mh, 1};
    bl.dstSubresource = bl.srcSubresource;
    bl.dstOffsets[0] = (VkOffset3D){(bw - dw) / 2, (bh - dh) / 2, 0};
    bl.dstOffsets[1] = (VkOffset3D){(bw - dw) / 2 + dw, (bh - dh) / 2 + dh, 1};
    vkCmdBlitImage(vk.cmd, g->img, VK_IMAGE_LAYOUT_GENERAL, g_backbuffer_img.img, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
    vk_barrier();
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

void vk_aspect_frame_end(void);
static void fps_log(void) {
    vk_aspect_frame_end();
    static double t0;
    static uint32_t f0, d0;
    double t = now_ms();
    if (!t0) t0 = t;
    if (t - t0 >= 2000.0) {
        fprintf(stderr, "usf: %.1f fps, %u draws/frame, ring %u KB\n", (g_dev.frames - f0) * 1000.0 / (t - t0),
                (g_dev.draws - d0) / (g_dev.frames - f0 ? g_dev.frames - f0 : 1), vk.ring_used >> 10);
        t0 = t; f0 = g_dev.frames; d0 = g_dev.draws;
    }
}

/* USF_HEADLESS: the frame is drawn and can be captured, but nothing goes to the screen */
static void present_headless(void) {
    uint32_t f = vk.frame % FRAMES;
    movie_overlay();
    if (want_shot(g_dev.frames)) shot_record();
    vkEndCommandBuffer(vk.cmd);
    vkResetFences(vk.dev, 1, &fences[f]);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &vk.cmd;
    VKC(vkQueueSubmit(vk.queue, 1, &si, fences[f]));
    if (shot_pending) shot_write(g_dev.frames);
}

void vk_overlay_draw(void);
void r_present(void) {
    if (!ready) return;
    vk_end_pass();
    vk_overlay_draw();                                         /* the touch layer over the frame (vk_overlay.c) */
    if (g_headless) {
        present_headless();
        fps_log();
        vk.frame++;
        begin_frame();
        return;
    }
    uint32_t f = vk.frame % FRAMES, idx;
    VkResult r = vkAcquireNextImageKHR(vk.dev, swap, UINT64_MAX, sem_acq[f], VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        vkEndCommandBuffer(vk.cmd);
        vkDeviceWaitIdle(vk.dev);
        make_swapchain();
        begin_frame();
        return;
    }
    movie_overlay();
    if (want_shot(g_dev.frames)) shot_record();
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = sw_img[idx];
    b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(vk.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    if (g_backbuffer_img.img) {
        VkImageBlit bl = {0};
        bl.srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bl.srcOffsets[1] = (VkOffset3D){(int)g_backbuffer_img.w, (int)g_backbuffer_img.h, 1};
        bl.dstSubresource = bl.srcSubresource;
        bl.dstOffsets[1] = (VkOffset3D){(int)sw_ext.width, (int)sw_ext.height, 1};
        vkCmdBlitImage(vk.cmd, g_backbuffer_img.img, VK_IMAGE_LAYOUT_GENERAL, sw_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &bl, VK_FILTER_LINEAR);
    }
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = 0;
    vkCmdPipelineBarrier(vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    vkEndCommandBuffer(vk.cmd);
    vkResetFences(vk.dev, 1, &fences[f]);
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &sem_acq[f]; si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1; si.pCommandBuffers = &vk.cmd;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &sem_done[f];
    VKC(vkQueueSubmit(vk.queue, 1, &si, fences[f]));
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &sem_done[f];
    pi.swapchainCount = 1; pi.pSwapchains = &swap; pi.pImageIndices = &idx;
    r = vkQueuePresentKHR(vk.queue, &pi);
    if (shot_pending) shot_write(g_dev.frames);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) { vkDeviceWaitIdle(vk.dev); make_swapchain(); }
    fps_log();
    vk.frame++;
    begin_frame();
}
