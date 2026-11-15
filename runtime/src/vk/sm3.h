/* Direct3D 9 shader bytecode (vs/ps 1.1 - 3.0) to GLSL 450 for Vulkan.
 *
 * Interface between the translator (sm3.c) and the renderer: where varyings, attributes, constants and samplers go.
 *  - VS inputs: location from (usage, index), see sm3_attr_location; the renderer binds vertex elements there.
 *  - varyings: location from (usage, index), see sm3_varying_location (same on both stages).
 *  - constants: set 0, binding 0 (VS) / 1 (PS): std140 { vec4 c[256]; ivec4 i[16]; ivec4 b; vec4 misc; }.
 *    VS misc.xy: the half-pixel offset (D3D9 pixel centres); PS misc.x: alpha reference (0..1), misc.y: alpha
 *    function (D3DCMP), misc.z: 1 when the render target is sRGB-written.
 *  - samplers: PS s0..s15 at binding 2..17, VS s0..s3 at binding 18..21 (combined image samplers).
 * What it does not do: predicated instructions or texture-bump (ps 1.x) instructions (reported as unsupported).
 */
#ifndef USF_SM3_H
#define USF_SM3_H

#include <stdint.h>

typedef struct {
    int is_ps;
    int major, minor;
    uint32_t sampler_type[16];         /* 2 = 2D, 3 = cube, 4 = volume (from dcl); 0 unused */
    uint32_t inputs_mask;              /* VS: attribute locations read */
    char *glsl;                        /* malloc'd */
    char error[160];
} Sm3Shader;

int sm3_translate(const uint32_t *tokens, uint32_t ntokens, Sm3Shader *out);   /* 0 on success */
int sm3_attr_location(uint32_t usage, uint32_t index);                        /* -1: not mapped */
int sm3_varying_location(uint32_t usage, uint32_t index);

#endif
