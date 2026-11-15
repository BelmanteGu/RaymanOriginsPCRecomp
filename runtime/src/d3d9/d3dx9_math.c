/* D3DX 9 math (matrices, vectors, planes) and the texture helpers Rayman Origins imports.
 *
 * What it does: the D3DX formulas (row vectors, D3DXMATRIX row-major, right-handed projections) in float, as the
 * DLL computes them: multiply, transpose, inverse, scaling, translation, rotations, orthographic / perspective
 * projections, look-at, vector normalize / transform / project / unproject, planes. D3DXCreateTexture makes a texture
 * on our device; D3DXFilterTexture is accepted (the renderer builds the mip chain when it uploads).
 * D3DXCompileShader reports what it was asked for: compiling HLSL source is not supported (the game's effects come
 * compiled; see d3dx9_effect.c).
 * What it does not do: the rest of D3DX.
 */
#include <math.h>
#include <string.h>

#include "d3d9.h"

typedef struct { float m[4][4]; } M4;
static M4 mget(uint32_t p) { M4 r; memcpy(&r, HOST(p), 64); return r; }
static uint32_t mput(uint32_t p, const M4 *m) { memcpy(HOST(p), m, 64); return p; }
static M4 ident(void) { M4 r = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}}; return r; }
static M4 mul(const M4 *a, const M4 *b) {
    M4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j] + a->m[i][3] * b->m[3][j];
    return r;
}
static float fa(uint32_t a, int i) { return RF32(a + 4u * (uint32_t)i); }   /* float argument i */

uint32_t x_D3DXMatrixMultiply(CPU *c, uint32_t a) { (void)c; M4 x = mget(ARG(1)), y = mget(ARG(2)), r = mul(&x, &y); return mput(ARG(0), &r); }
uint32_t x_D3DXMatrixTranspose(CPU *c, uint32_t a) {
    (void)c;
    M4 x = mget(ARG(1)), r;
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) r.m[i][j] = x.m[j][i];
    return mput(ARG(0), &r);
}
uint32_t x_D3DXMatrixInverse(CPU *c, uint32_t a) {               /* (out, &determinant, m) */
    (void)c;
    M4 x = mget(ARG(2));
    float inv[16], *m = &x.m[0][0];
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (ARG(1)) WF32(ARG(1), det);
    if (det == 0.0f) return 0;
    for (int i = 0; i < 16; i++) inv[i] /= det;
    memcpy(HOST(ARG(0)), inv, 64);
    return ARG(0);
}
uint32_t x_D3DXMatrixScaling(CPU *c, uint32_t a) {
    (void)c;
    M4 r = ident();
    r.m[0][0] = fa(a, 1); r.m[1][1] = fa(a, 2); r.m[2][2] = fa(a, 3);
    return mput(ARG(0), &r);
}
uint32_t x_D3DXMatrixTranslation(CPU *c, uint32_t a) {
    (void)c;
    M4 r = ident();
    r.m[3][0] = fa(a, 1); r.m[3][1] = fa(a, 2); r.m[3][2] = fa(a, 3);
    return mput(ARG(0), &r);
}
uint32_t x_D3DXMatrixRotationZ(CPU *c, uint32_t a) {
    (void)c;
    M4 r = ident();
    float s = sinf(fa(a, 1)), co = cosf(fa(a, 1));
    r.m[0][0] = co; r.m[0][1] = s; r.m[1][0] = -s; r.m[1][1] = co;
    return mput(ARG(0), &r);
}
uint32_t x_D3DXMatrixRotationYawPitchRoll(CPU *c, uint32_t a) {  /* roll (Z), then pitch (X), then yaw (Y) */
    (void)c;
    float sy = sinf(fa(a, 1)), cy = cosf(fa(a, 1)), sp = sinf(fa(a, 2)), cp = cosf(fa(a, 2));
    float sr = sinf(fa(a, 3)), cr = cosf(fa(a, 3));
    M4 r = ident();
    r.m[0][0] = cr * cy + sr * sp * sy; r.m[0][1] = sr * cp; r.m[0][2] = sr * sp * cy - cr * sy;
    r.m[1][0] = cr * sp * sy - sr * cy; r.m[1][1] = cr * cp; r.m[1][2] = sr * sy + cr * sp * cy;
    r.m[2][0] = cp * sy; r.m[2][1] = -sp; r.m[2][2] = cp * cy;
    return mput(ARG(0), &r);
}
uint32_t x_D3DXMatrixOrthoOffCenterRH(CPU *c, uint32_t a) {      /* (out, l, r, b, t, zn, zf) */
    (void)c;
    float l = fa(a, 1), r_ = fa(a, 2), b = fa(a, 3), t = fa(a, 4), zn = fa(a, 5), zf = fa(a, 6);
    M4 r = ident();
    r.m[0][0] = 2.0f / (r_ - l); r.m[1][1] = 2.0f / (t - b); r.m[2][2] = 1.0f / (zn - zf);
    r.m[3][0] = (l + r_) / (l - r_); r.m[3][1] = (t + b) / (b - t); r.m[3][2] = zn / (zn - zf);
    return mput(ARG(0), &r);
}
uint32_t x_D3DXMatrixPerspectiveFovRH(CPU *c, uint32_t a) {      /* (out, fovy, aspect, zn, zf) */
    (void)c;
    float ys = 1.0f / tanf(fa(a, 1) / 2.0f), xs = ys / fa(a, 2), zn = fa(a, 3), zf = fa(a, 4);
    M4 r;
    memset(&r, 0, sizeof r);
    r.m[0][0] = xs; r.m[1][1] = ys; r.m[2][2] = zf / (zn - zf); r.m[2][3] = -1.0f; r.m[3][2] = zn * zf / (zn - zf);
    return mput(ARG(0), &r);
}

typedef struct { float x, y, z; } V3;
static V3 vget(uint32_t p) { V3 v; memcpy(&v, HOST(p), 12); return v; }
static V3 sub(V3 a, V3 b) { return (V3){a.x - b.x, a.y - b.y, a.z - b.z}; }
static V3 cross(V3 a, V3 b) { return (V3){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 norm(V3 a) { float l = sqrtf(dot(a, a)); return l ? (V3){a.x / l, a.y / l, a.z / l} : (V3){0, 0, 0}; }

uint32_t x_D3DXMatrixLookAtRH(CPU *c, uint32_t a) {              /* (out, eye, at, up) */
    (void)c;
    V3 eye = vget(ARG(1)), z = norm(sub(eye, vget(ARG(2)))), x = norm(cross(vget(ARG(3)), z)), y = cross(z, x);
    M4 r = {{{x.x, y.x, z.x, 0}, {x.y, y.y, z.y, 0}, {x.z, y.z, z.z, 0}, {-dot(x, eye), -dot(y, eye), -dot(z, eye), 1}}};
    return mput(ARG(0), &r);
}
uint32_t x_D3DXVec3Normalize(CPU *c, uint32_t a) { (void)c; V3 v = norm(vget(ARG(1))); memcpy(HOST(ARG(0)), &v, 12); return ARG(0); }
uint32_t x_D3DXVec3TransformNormal(CPU *c, uint32_t a) {
    (void)c;
    V3 v = vget(ARG(1));
    M4 m = mget(ARG(2));
    V3 r = {v.x * m.m[0][0] + v.y * m.m[1][0] + v.z * m.m[2][0], v.x * m.m[0][1] + v.y * m.m[1][1] + v.z * m.m[2][1],
            v.x * m.m[0][2] + v.y * m.m[1][2] + v.z * m.m[2][2]};
    memcpy(HOST(ARG(0)), &r, 12);
    return ARG(0);
}
static void xform(V3 v, const M4 *m, float out[4]) {
    for (int j = 0; j < 4; j++) out[j] = v.x * m->m[0][j] + v.y * m->m[1][j] + v.z * m->m[2][j] + m->m[3][j];
}
static M4 wvp(uint32_t proj, uint32_t view, uint32_t world) {
    M4 r = ident(), t;
    if (world) { t = mget(world); r = mul(&r, &t); }
    if (view) { t = mget(view); r = mul(&r, &t); }
    if (proj) { t = mget(proj); r = mul(&r, &t); }
    return r;
}
uint32_t x_D3DXVec3Project(CPU *c, uint32_t a) {                 /* (out, v, viewport, proj, view, world) */
    (void)c;
    M4 m = wvp(ARG(3), ARG(4), ARG(5));
    float o[4];
    xform(vget(ARG(1)), &m, o);
    if (o[3]) { o[0] /= o[3]; o[1] /= o[3]; o[2] /= o[3]; }
    if (ARG(2)) {
        uint32_t vp = ARG(2);
        o[0] = (float)R32(vp) + (1.0f + o[0]) * (float)R32(vp + 8) / 2.0f;
        o[1] = (float)R32(vp + 4) + (1.0f - o[1]) * (float)R32(vp + 12) / 2.0f;
        o[2] = RF32(vp + 16) + o[2] * (RF32(vp + 20) - RF32(vp + 16));
    }
    memcpy(HOST(ARG(0)), o, 12);
    return ARG(0);
}
uint32_t x_D3DXMatrixInverse(CPU *c, uint32_t a);
uint32_t x_D3DXVec3Unproject(CPU *c, uint32_t a) {               /* (out, v, viewport, proj, view, world) */
    M4 m = wvp(ARG(3), ARG(4), ARG(5));
    uint32_t tmp = gmalloc(160);
    mput(tmp, &m);
    W32(tmp + 128, tmp + 64); W32(tmp + 132, 0); W32(tmp + 136, tmp);
    uint32_t ok = x_D3DXMatrixInverse(c, tmp + 128);
    M4 inv = mget(tmp + 64);
    gfree(tmp);
    if (!ok) return 0;
    V3 v = vget(ARG(1));
    if (ARG(2)) {
        uint32_t vp = ARG(2);
        v.x = 2.0f * (v.x - (float)R32(vp)) / (float)R32(vp + 8) - 1.0f;
        v.y = 1.0f - 2.0f * (v.y - (float)R32(vp + 4)) / (float)R32(vp + 12);
        v.z = (v.z - RF32(vp + 16)) / (RF32(vp + 20) - RF32(vp + 16));
    }
    float o[4];
    xform(v, &inv, o);
    if (o[3]) { o[0] /= o[3]; o[1] /= o[3]; o[2] /= o[3]; }
    memcpy(HOST(ARG(0)), o, 12);
    return ARG(0);
}
uint32_t x_D3DXPlaneNormalize(CPU *c, uint32_t a) {
    (void)c;
    float p[4];
    memcpy(p, HOST(ARG(1)), 16);
    float l = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    for (int i = 0; i < 4; i++) p[i] = l ? p[i] / l : 0.0f;
    memcpy(HOST(ARG(0)), p, 16);
    return ARG(0);
}
uint32_t x_D3DXPlaneFromPoints(CPU *c, uint32_t a) {
    (void)c;
    V3 p1 = vget(ARG(1)), n = norm(cross(sub(vget(ARG(2)), p1), sub(vget(ARG(3)), p1)));
    float p[4] = {n.x, n.y, n.z, -dot(n, p1)};
    memcpy(HOST(ARG(0)), p, 16);
    return ARG(0);
}
uint32_t x_D3DXPlaneIntersectLine(CPU *c, uint32_t a) {          /* (out, plane, v1, v2) */
    (void)c;
    float pl[4];
    memcpy(pl, HOST(ARG(1)), 16);
    V3 n = {pl[0], pl[1], pl[2]}, v1 = vget(ARG(2)), d = sub(vget(ARG(3)), v1);
    float den = dot(n, d);
    if (den == 0.0f) return 0;
    float t = -(dot(n, v1) + pl[3]) / den;
    V3 r = {v1.x + t * d.x, v1.y + t * d.y, v1.z + t * d.z};
    memcpy(HOST(ARG(0)), &r, 12);
    return ARG(0);
}

/* ---- textures and shader compilation ---- */
uint32_t x_D3DXCreateTexture(CPU *c, uint32_t a) {              /* (device, w, h, mips, usage, format, pool, &tex) */
    (void)c;
    uint32_t w = ARG(1) ? ARG(1) : 1, h = ARG(2) ? ARG(2) : 1;
    W32(ARG(7), d3d_create_texture(OBJ_TEXTURE, w, h, ARG(3), ARG(4), ARG(5), ARG(6)));
    return 0;
}
uint32_t x_D3DXFilterTexture(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t x_D3DXCompileShader(CPU *c, uint32_t a) {              /* (src, len, defines, include, function, profile, ...) */
    (void)c;
    fprintf(stderr, "usf: D3DXCompileShader(%s, %s): HLSL compilation is not supported\n",
            ARG(4) ? STR(ARG(4)) : "?", ARG(5) ? STR(ARG(5)) : "?");
    if (ARG(7)) W32(ARG(7), 0);
    if (ARG(8)) W32(ARG(8), 0);
    if (ARG(9)) W32(ARG(9), 0);
    return 0x80004005u;
}
