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

#endif
