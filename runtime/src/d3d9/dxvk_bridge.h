/* The DXVK bridge: the game's Direct3D 9 calls answered by DXVK built as a native Linux library (dxvk-native,
 * libdxvk_d3d9.so) instead of the runtime's own D3D9-on-Vulkan layer.
 *
 * What it is for: every D3D9 object the game holds is a guest wrapper (a 16-byte guest object: vtable of thunks, and
 * the index of its Wrap); a call goes through dxb_dispatch, which converts the arguments by the method's signature
 * (dxvk_sigs.c) and calls DXVK's method through its vtable. Locks hand the game a copy in guest memory (DXVK's
 * memory is outside the 4 GiB guest window) that is written back on Unlock.
 * What it does not do: the runtime's own renderer features (ultrawide, captures); USF_DXVK=0 selects that renderer.
 */
#ifndef USF_DXVK_BRIDGE_H
#define USF_DXVK_BRIDGE_H

#include <stdint.h>

#include "hle.h"

enum { DXB_D3D9 = 1, DXB_DEVICE, DXB_TEXTURE, DXB_CUBE, DXB_VOLTEX, DXB_SURFACE, DXB_VOLUME, DXB_VB, DXB_IB, DXB_VS,
       DXB_PS, DXB_DECL, DXB_QUERY, DXB_STATEBLOCK, DXB_SWAPCHAIN, DXB_NTYPES };

typedef struct Lock { uint32_t key, guest, size; void *host; struct Lock *next; } Lock;   /* one open lock */
typedef struct { uint32_t guest, size, epoch; } TexShadow;   /* a texture level's guest copy */
typedef struct {
    void *nat;                 /* DXVK's object */
    int type;
    uint32_t guest;            /* the guest wrapper */
    uint32_t shadow, shadow_size;   /* buffers: a guest copy of the whole buffer, kept between locks */
    uint32_t rate_t0, rate_n;  /* buffers: locks in the current second */
    int checked;               /* buffers: locked rarely, so writes past the locked range are looked for */
    uint8_t *slice;            /* dynamic buffers: DXVK's mapping of the current slice (offset 0) */
    uint32_t dlo, dhi;         /* dynamic buffers: bytes written by the game, not yet copied to the slice */
    uint32_t isz;              /* index buffers: bytes per index (0 until asked) */
    uint32_t gen;              /* buffers: locks so far (index ranges remembered for an older one are stale) */
    TexShadow *tsh;            /* textures and surfaces: one guest copy per face and level, kept between locks */
    void *ring[4];             /* large dynamic buffers: DXVK buffers the game's one rotates through on DISCARD */
    uint32_t ring_last[4];     /* the frame each stopped being the current one */
    int ring_i, ring_on;       /* the current one; 1 ring made, -1 not possible */
    void *rb[4];               /* small render targets read back by the CPU: system-memory copies, a ring */
    uint32_t rb_n;             /* readbacks made */
    int rb_open, rb_off;       /* a readback lock is open (no DXVK lock to undo); -1: readback not possible */
    uint32_t code_hash;        /* shaders: FNV-1a of the bytecode; textures: of level 0's first 4 KB (analysis) */
    uint32_t parent, plevel;   /* surfaces from GetSurfaceLevel: the texture and the level (analysis) */
    Lock *locks;
} Wrap;

extern const char *const dxb_sig_d3d9[17], *const dxb_sig_device[119], *const dxb_sig_texture[22],
    *const dxb_sig_cube[22], *const dxb_sig_voltex[22], *const dxb_sig_surface[17], *const dxb_sig_volume[11],
    *const dxb_sig_buffer[14], *const dxb_sig_shader[5], *const dxb_sig_query[8], *const dxb_sig_stateblock[6],
    *const dxb_sig_swapchain[10];

int dxb_enabled(void);                                    /* the DXVK library is loaded (USF_DXVK, default on) */
uint32_t dxb_wrap(void *nat, int type);                   /* the guest wrapper of a DXVK object (made once) */
Wrap *dxb_of(uint32_t guest);                             /* NULL if not a wrapper */
void *dxb_nat(uint32_t guest);                            /* DXVK object of a wrapper (NULL for 0) */
uint32_t dxb_device_guest(void);

/* calling DXVK: method `slot` of a native COM object (System V: `this` and integer arguments in registers) */
typedef uint64_t (*DxbFn)(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                          uint64_t, uint64_t, uint64_t);
static inline DxbFn dxb_method(void *nat, int slot) { return ((DxbFn *)*(void **)nat)[slot]; }

uint32_t dxb_d3dx_texture(CPU *c, uint32_t device, uint32_t src, uint32_t size, uint32_t levels, uint32_t usage,
                          uint32_t pool, uint32_t info, uint32_t out, int kind);   /* dxvk_d3dx.c */

/* bridge internals shared by dxvk_bridge.c and dxvk_locks.c (locks, buffer rings, draw-time copies) */
int dxl_block_format(uint32_t f);
void dxl_lock_push(Wrap *w, uint32_t key, uint32_t guest, uint32_t size, void *host);
Lock *dxl_lock_pop(Wrap *w, uint32_t key);
uint32_t dxl_lock_rect(Wrap *w, int slot, uint64_t face, uint64_t level, uint32_t g_lr, uint32_t g_rect,                           uint32_t flags, const uint32_t *desc, uint32_t key);
int dxl_lock_readback(Wrap *w, uint32_t g_lr, uint32_t g_rect, uint32_t flags, const uint32_t *desc);
uint32_t dxl_unlock(Wrap *w, int slot, uint32_t key, uint64_t a1, uint64_t a2);
uint32_t dxl_lock_box(Wrap *w, int slot, uint64_t level, uint32_t g_lb, uint32_t g_box, uint32_t flags,                          uint32_t depth, uint32_t key);
uint32_t dxl_ring_discard(Wrap *w, const uint32_t *desc, uint32_t flags);
void dxl_ring_free(Wrap *w);
void dxl_flush_range(Wrap *w, uint32_t lo, uint32_t hi);
uint32_t dxl_index_size(Wrap *ib);
void dxl_index_range(Wrap *ib, uint32_t start, uint32_t n, uint32_t isz, uint32_t *lo, uint32_t *hi);
uint32_t dxl_vcount(uint32_t type, uint32_t prims);
void dxl_flush_draw(int indexed, uint32_t type, int32_t base, uint32_t first, uint32_t num, uint32_t start_index,                        uint32_t prims);
extern uint32_t gpu_epoch, stream_buf[16], stream_off[16], stream_stride[16], stream_freq[16], cur_ib;
extern uint64_t copied_buf, copied_tex_in, copied_tex_out, checked_bytes, checked_locks;
extern int usf_stalls;
uint32_t dxl_lock_buffer(Wrap *w, uint32_t off, uint32_t size, uint32_t g_pp, uint32_t flags);
uint32_t dxl_unlock_buffer(Wrap *w);
extern Wrap *wraps;
extern uint32_t device_guest;
void hput(void *p, uint32_t v);
void hdel(void *p);
double now_ms(void);
uint32_t usf_ms(void);
#include <pthread.h>
extern pthread_mutex_t wlock;

#endif
