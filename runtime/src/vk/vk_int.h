/* Internal interface of the Vulkan renderer (vk_core.c, vk_res.c, vk_draw.c).
 *
 * What it is for: the Direct3D 9 layer records state (d3d9.h, g_dev); these files turn it into Vulkan 1.1 work:
 * vk_core owns the device, memory, the per-frame command buffer and upload ring, and presentation; vk_res owns images
 * (textures, surfaces, render targets), uploads from guest memory and samplers; vk_draw owns shaders, pipelines,
 * render passes, vertex input and draws. All images stay in VK_IMAGE_LAYOUT_GENERAL, and a render-pass change or an
 * upload ends the current pass with a full barrier: simple and correct first, faster later.
 * What it does not do: run on more than one queue or thread.
 */
#ifndef USF_VK_INT_H
#define USF_VK_INT_H

#include <stdint.h>
#include <vulkan/vulkan.h>

#include "../d3d9/d3d9.h"

#define FRAMES 2
#define RING_SIZE (96u << 20)

typedef struct {
    VkInstance inst;
    VkPhysicalDevice gpu;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfam;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceFeatures feats;
    uint32_t vtx_widen;                 /* bit t: D3D vertex type t has no Vulkan vertex format here (SCALED): widened */
    VkPhysicalDeviceMemoryProperties mem;
    VkCommandBuffer cmd;               /* the current frame's command buffer (recording) */
    uint32_t frame;                    /* frame counter */
    VkDescriptorPool dpool;            /* the current frame's descriptor pool */
    VkBuffer ring;                     /* the current frame's upload ring */
    uint8_t *ring_ptr;
    uint32_t ring_used;
    int in_pass;                       /* a render pass is open */
    VkFormat depth_format;             /* D24S8 if supported, else D32S8 */
} Vk;
extern Vk vk;

#define VKC(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) hle_fatal("%s failed: %d", #x, (int)_r); } while (0)

/* vk_core.c */
VkDeviceMemory vk_alloc(VkMemoryRequirements req, VkMemoryPropertyFlags flags, void **map);
uint32_t vk_ring_alloc(uint32_t size, uint32_t align, void **ptr);   /* offset in vk.ring */
void vk_end_pass(void);                                             /* ends the open pass + full barrier */
void vk_flush(void);                                                /* submit + wait, mid-frame */
void vk_ring_reserve(uint32_t size);                                /* flush first if size would not fit */
void vk_bind_image(VkImage img);
void vk_barrier(void);

/* vk_res.c */
typedef struct {
    VkImage img;
    VkImageView view;                  /* sampled view (with the format's swizzle) */
    VkImageView att[MAX_LEVELS];       /* attachment views per level (render targets / depth) */
    VkImageView cube_att[6];
    VkFormat fmt;
    uint32_t w, h, levels, layers, version, depth, dirty_gpu;
    int convert;                       /* CPU conversion before upload (see vk_res.c) */
} GpuImage;

GpuImage *vk_texture(D3DObj *o);                                    /* create / upload as needed */
GpuImage *vk_target(uint32_t surface_guest, VkImageView *view, int *w, int *h);   /* a surface as an attachment */
VkSampler vk_sampler(const uint32_t *ss);
GpuImage *vk_dummy_texture(int type);                               /* 2 = 2D, 3 = cube, 4 = 3D */
VkFormat vk_d3d_format(uint32_t fmt, VkComponentMapping *swz, int *convert);
void vk_res_init(void);
void vk_copy_surface(uint32_t src, uint32_t dst);                   /* StretchRect between GPU surfaces */
extern GpuImage g_backbuffer_img;

/* vk_draw.c */
void vk_draw_init(void);
void vk_draw_frame_start(void);

#endif
