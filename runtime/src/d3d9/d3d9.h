/* The Direct3D 9 layer: COM objects the game holds (guest memory) tied to host-side state.
 *
 * What it is for: Direct3DCreate9 hands the game an IDirect3D9; from it the device, and from the device every
 * resource. Each object is a guest object (vtable of thunks, see objects.c) whose dword at +8 is the index of its
 * host-side D3DObj. Resource memory the game writes through Lock lives in guest memory, so the game's pointers work.
 * The renderer (vk_*.c) reads these objects to draw.
 * What it does not do: draw by itself (d3d9_device.c records state; the Vulkan renderer turns it into frames).
 */
#ifndef USF_D3D9_H
#define USF_D3D9_H

#include <stdint.h>

#include "../hle.h"

enum { OBJ_D3D9 = 1, OBJ_DEVICE, OBJ_TEXTURE, OBJ_CUBE, OBJ_VOLUME, OBJ_SURFACE, OBJ_VB, OBJ_IB, OBJ_VS, OBJ_PS,
       OBJ_DECL, OBJ_QUERY, OBJ_STATEBLOCK, OBJ_SWAPCHAIN };

#define MAX_LEVELS 16

typedef struct D3DObj {
    int kind;
    uint32_t guest;                  /* the guest object (what the game holds) */
    uint32_t refs;
    uint32_t format, usage, pool;
    uint32_t width, height, depth, levels;
    uint32_t mem[6][MAX_LEVELS];     /* guest memory per face and level (textures), or [0][0] for buffers */
    uint32_t pitch[MAX_LEVELS], size[MAX_LEVELS];
    uint32_t parent;                 /* surfaces of a texture: the texture's guest object */
    uint32_t face, level;
    uint32_t code, code_size;        /* shaders and vertex declarations: a copy of the game's bytes (guest) */
    uint32_t query_type;
    uint32_t version;                /* bumped on every Unlock: the renderer re-uploads when it changes */
    void *gpu;                       /* renderer-side data */
} D3DObj;

D3DObj *d3d_obj(uint32_t guest);                    /* NULL if the guest pointer is not one of ours */
uint32_t d3d_new(int kind, D3DObj **out);           /* a new object with the vtable of its kind */
uint32_t d3d_format_bpp(uint32_t fmt);              /* bits per pixel; block formats: per 4x4 block / 16 */
int d3d_format_block(uint32_t fmt);
uint32_t d3d_shader_hash(uint32_t guest);            /* FNV-1a of a shader's bytecode */                 /* 1 for DXT1..5 */
uint32_t d3d_pitch(uint32_t fmt, uint32_t w);
uint32_t d3d_level_size(uint32_t fmt, uint32_t w, uint32_t h);
uint32_t d3d_new_surface(uint32_t fmt, uint32_t w, uint32_t h, uint32_t usage, uint32_t parent, uint32_t face,
                         uint32_t level, uint32_t mem);

uint32_t d3d_create_shader(int kind, uint32_t bytecode);        /* OBJ_VS / OBJ_PS from bytecode in guest memory */
uint32_t d3d_create_texture(int kind, uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool);

/* device state the renderer reads */
typedef struct {
    uint32_t rt[4], ds;              /* guest surfaces */
    uint32_t vp[6];                  /* viewport: x y w h, minz maxz as float bits */
    uint32_t rs[256];                /* render states */
    uint32_t ss[16][14];             /* sampler states */
    uint32_t tex[16];
    uint32_t vs, ps, decl, fvf, ib;
    uint32_t stream[16], stream_off[16], stream_stride[16], stream_freq[16];
    float vsc[256][4], psc[224][4];
    int32_t vsi[16][4], psi[16][4];
    uint32_t vsb[16], psb[16];
    uint32_t scissor[4];
    uint32_t backbuffer, auto_ds;
    uint32_t bb_w, bb_h, bb_fmt;
    uint32_t frames, draws;
} DevState;

extern DevState g_dev;

/* renderer entry points (vk_present.c) */
int r_init(uint32_t w, uint32_t h);
void r_clear(uint32_t flags, uint32_t color, float z, uint32_t stencil);
void r_draw(int indexed, uint32_t prim, uint32_t base_vertex, uint32_t min_index, uint32_t num_vertices,
            uint32_t start, uint32_t count, uint32_t up_data, uint32_t up_stride, uint32_t up_indices, uint32_t up_ifmt);
void r_present(void);

#endif
