/* ID3DXEffect for the game's compiled effects (fx_2_0 binaries: shaders/compiled/win32/NAME.fx.ckd).
 *
 * What it does: D3DXCreateEffect parses the binary (the layout D3DX writes: parameters with types and defaults,
 * techniques -> passes -> states, then strings and resources holding the shader bytecode), creates every vertex /
 * pixel shader on our D3D9 device and reads each shader's constant table (CTAB) to know which register every
 * parameter lands in. The game sets parameters (SetMatrix, SetVector, SetFloat, SetTexture...) by handle or by name;
 * BeginPass / CommitChanges set the pass's shaders and upload the parameters each shader uses, the way D3DX does:
 * matrices follow the register class of the constant (rows or columns), samplers get their texture and states.
 * What it does not do: render states set inside passes other than the shaders (the game's effects have none),
 * parameter blocks, annotations, or compiling effects from HLSL source (D3DXERR_INVALIDDATA for those).
 */
#include <stdlib.h>
#include <string.h>

#include "d3d9.h"

#define MAXP 64
#define MAXT 32
#define MAXS 16
#define FXERR 0x88760B59u                                      /* D3DXERR_INVALIDDATA */

enum { C_SCALAR, C_VECTOR, C_MROWS, C_MCOLS, C_OBJECT, C_STRUCT };

typedef struct { int param, set, reg, count, cls, rows, cols; } Map;   /* one CTAB constant */
typedef struct { uint32_t shader; int vs; Map *map; int nmap; } Shader;
typedef struct {
    char name[64], semantic[64];
    uint32_t type, cls, rows, cols, elems;
    uint32_t nfloat;                                           /* floats of storage (rows * cols per element) */
    float *f;                                                  /* value, row-major as the game sets it */
    uint32_t tex;                                              /* texture parameters: the guest texture */
    uint32_t obj;                                              /* object parameters: their object id */
    int tex_param;                                             /* samplers: the texture parameter they read */
    uint32_t ss[14]; uint32_t ss_set;                          /* samplers: states (D3DSAMP_*) and which are set */
} Param;
typedef struct { char name[64]; uint32_t vs_obj, ps_obj; } Pass;
typedef struct { char name[64]; int npass; Pass pass[4]; } Tech;
typedef struct {
    Param p[MAXP]; int np;
    Tech t[MAXT]; int nt;
    Shader sh[2 * MAXT * 4]; int nsh;
    uint32_t obj_shader[256];                                  /* object id -> index in sh[] + 1 */
    uint32_t handles;                                          /* guest array: one dword per handle */
    int cur_tech, cur_pass;
    uint32_t guest;
} Effect;

static Effect *effects[64];
static int neffects;

/* ---- reading the binary ---- */
typedef struct { const uint8_t *b; uint32_t size; } Blob;
static uint32_t rd(const Blob *x, uint32_t off) { uint32_t v = 0; if (off + 4 <= x->size) memcpy(&v, x->b + off, 4); return v; }
static void rstr(const Blob *x, uint32_t off, char *out, size_t n) {
    uint32_t len = rd(x, off);
    if (len >= n) len = (uint32_t)n - 1;
    if (off + 4 + len > x->size) len = 0;
    memcpy(out, x->b + off + 4, len);
    out[len] = 0;
}

static void read_param(Effect *e, const Blob *x, uint32_t td, uint32_t val) {
    Param *p = &e->p[e->np];
    memset(p, 0, sizeof *p);
    p->tex_param = -1;
    p->type = rd(x, td);
    p->cls = rd(x, td + 4);
    rstr(x, rd(x, td + 8), p->name, sizeof p->name);
    if (rd(x, td + 12)) rstr(x, rd(x, td + 12), p->semantic, sizeof p->semantic);
    p->elems = rd(x, td + 16);
    uint32_t n = p->elems ? p->elems : 1;
    if (p->cls <= C_MCOLS) {
        p->cols = rd(x, td + 20);
        p->rows = rd(x, td + 24);
        p->nfloat = n * p->rows * p->cols;
        p->f = calloc(p->nfloat ? p->nfloat : 1, 4);
        if (val + 4 * p->nfloat <= x->size) memcpy(p->f, x->b + val, 4 * p->nfloat);
    } else if (p->cls == C_OBJECT) {
        if (p->type >= 10 && p->type <= 14) {                   /* sampler: its states follow the value */
            uint32_t ns = rd(x, val);
            for (uint32_t k = 0; k < ns; k++) {
                uint32_t op = rd(x, val + 4 + 16 * k), sv = rd(x, val + 16 + 16 * k);
                if (op > 164 && op < 164 + 14) { p->ss[op - 164] = rd(x, sv); p->ss_set |= 1u << (op - 164); }
            }
        } else {
            p->obj = rd(x, val);
        }
    }
    e->np++;
}

/* CTAB of a shader: name -> register set, index and count */
static void read_ctab(Effect *e, Shader *s, uint32_t code, uint32_t size) {
    for (uint32_t i = 1; i * 4 < size; ) {
        uint32_t t = R32(code + 4 * i);
        if ((t & 0xFFFF) != 0xFFFE) break;
        uint32_t len = (t >> 16) & 0x7FFF;
        if (R32(code + 4 * i + 4) == 0x42415443u) {             /* "CTAB" */
            uint32_t ct = code + 4 * i + 8, n = R32(ct + 12), info = ct + R32(ct + 16);
            s->map = calloc(n ? n : 1, sizeof(Map));
            for (uint32_t k = 0; k < n; k++) {
                uint32_t ci = info + 20 * k;
                const char *name = (const char *)HOST(ct + R32(ci));
                Map m = {-1, R16(ci + 4), R16(ci + 6), R16(ci + 8), 0, 0, 0};
                uint32_t ty = ct + R32(ci + 12);
                m.cls = R16(ty); m.rows = R16(ty + 4); m.cols = R16(ty + 6);
                for (int q = 0; q < e->np; q++) if (!strcmp(e->p[q].name, name)) m.param = q;
                if (m.param >= 0) s->map[s->nmap++] = m;
            }
            return;
        }
        i += 1 + len;
    }
}

static int parse(Effect *e, uint32_t data, uint32_t size) {
    if (size < 8 || R32(data) != 0xFEFF0901u) return -1;
    Blob x = {HOST(data + 8), size - 8};
    uint32_t p = R32(data + 4);
    uint32_t np = rd(&x, p), nt = rd(&x, p + 4);
    p += 16;
    if (np > MAXP || nt > MAXT) return -2;
    for (uint32_t i = 0; i < np; i++) {
        read_param(e, &x, rd(&x, p), rd(&x, p + 4));
        p += 16 + 8 * rd(&x, p + 12);                           /* annotations */
    }
    for (uint32_t i = 0; i < nt; i++) {
        Tech *t = &e->t[e->nt++];
        rstr(&x, rd(&x, p), t->name, sizeof t->name);
        uint32_t npass = rd(&x, p + 8);
        p += 12 + 8 * rd(&x, p + 4);
        for (uint32_t k = 0; k < npass; k++) {
            Pass *ps = k < 4 ? &t->pass[t->npass++] : NULL;
            if (ps) rstr(&x, rd(&x, p), ps->name, sizeof ps->name);
            uint32_t nst = rd(&x, p + 8);
            p += 12 + 8 * rd(&x, p + 4);
            for (uint32_t s = 0; s < nst; s++, p += 16) {
                uint32_t op = rd(&x, p), obj = rd(&x, rd(&x, p + 12));
                if (!ps) continue;
                if (op == 146) ps->vs_obj = obj;
                else if (op == 147) ps->ps_obj = obj;
                else usf_log("d3dx effect: pass state %u ignored\n", op);
            }
        }
    }
    uint32_t nstr = rd(&x, p), nres = rd(&x, p + 4);
    p += 8;
    for (uint32_t i = 0; i < nstr; i++) p += 8 + ((rd(&x, p + 4) + 3) & ~3u);
    for (uint32_t i = 0; i < nres; i++) {
        uint32_t tech = rd(&x, p), idx = rd(&x, p + 4), st = rd(&x, p + 12), usage = rd(&x, p + 16), len = rd(&x, p + 20);
        uint32_t blob = data + 8 + p + 24;
        if (tech == 0xFFFFFFFFu && usage == 1 && idx < (uint32_t)e->np && st == 0) {
            char name[64];                                     /* a sampler's Texture = <parameter> */
            uint32_t n = len < 63 ? len : 63;
            memcpy(name, HOST(blob), n); name[n] = 0;
            for (int q = 0; q < e->np; q++) if (!strcmp(e->p[q].name, name)) e->p[idx].tex_param = q;
        } else if (tech < (uint32_t)e->nt && idx < (uint32_t)e->t[tech].npass && usage == 0 && len >= 4) {
            Pass *ps = &e->t[tech].pass[idx];
            uint32_t ver = R32(blob), obj = (ver & 0xFFFF0000u) == 0xFFFE0000u ? ps->vs_obj : ps->ps_obj;
            if (obj < 256 && !e->obj_shader[obj] && e->nsh < (int)(sizeof e->sh / sizeof e->sh[0])) {
                Shader *s = &e->sh[e->nsh];
                s->vs = (ver & 0xFFFF0000u) == 0xFFFE0000u;
                s->shader = d3d_create_shader(s->vs ? OBJ_VS : OBJ_PS, blob);
                read_ctab(e, s, blob, len);
                e->obj_shader[obj] = (uint32_t)++e->nsh;
            }
        }
        p += 24 + ((len + 3) & ~3u);
    }
    return 0;
}

/* ---- handles: guest dwords [params | techniques | passes]; a handle may also be a name ---- */
static Effect *fx_of(uint32_t self) { uint32_t i = R32(self + 8); return i < 64 ? effects[i] : NULL; }
static int param_of(Effect *e, uint32_t h) {
    if (!h) return -1;
    if (h >= e->handles && h < e->handles + 4u * (uint32_t)e->np) return (int)((h - e->handles) / 4);
    if (h >= e->handles + 4u * MAXP && h < e->handles + 4u * (MAXP + MAXT + MAXT * 4)) return -1;
    const char *name = (const char *)HOST(h);
    for (int i = 0; i < e->np; i++) if (!strcmp(e->p[i].name, name)) return i;
    return -1;
}
static int tech_of(Effect *e, uint32_t h) {
    if (h >= e->handles + 4u * MAXP && h < e->handles + 4u * (MAXP + (uint32_t)e->nt)) return (int)((h - e->handles) / 4 - MAXP);
    if (!h || h < 0x10000u) return -1;
    const char *name = (const char *)HOST(h);
    for (int i = 0; i < e->nt; i++) if (!strcmp(e->t[i].name, name)) return i;
    return -1;
}

/* ---- applying a pass ---- */
static void upload(Effect *e, Shader *s) {
    for (int k = 0; k < s->nmap; k++) {
        Map *m = &s->map[k];
        Param *p = &e->p[m->param];
        if (m->set == 3) {                                     /* sampler register */
            uint32_t reg = (uint32_t)m->reg + (s->vs ? 12u : 0u);
            if (reg >= 16) continue;
            if (p->tex_param >= 0) g_dev.tex[reg] = e->p[p->tex_param].tex;
            for (int q = 1; q < 14; q++) if (p->ss_set & (1u << q)) g_dev.ss[reg][q] = p->ss[q];
            continue;
        }
        float regs[256][4];
        int n = m->count > 256 ? 256 : m->count;
        memset(regs, 0, 16 * (size_t)n);
        uint32_t rows = p->rows ? p->rows : 1, cols = p->cols ? p->cols : 1, per = rows * cols;
        if (p->cls == C_MROWS || p->cls == C_MCOLS) {
            int col_major = m->cls == C_MCOLS;
            int per_elem = col_major ? (int)cols : (int)rows;
            for (int r = 0; r < n; r++) {
                uint32_t el = (uint32_t)(r / per_elem), i = (uint32_t)(r % per_elem);
                if (el * per >= p->nfloat) break;
                const float *mtx = p->f + el * per;
                for (uint32_t j = 0; j < 4; j++) {
                    if (col_major) { if (j < rows) regs[r][j] = mtx[j * cols + i]; }
                    else if (j < cols) regs[r][j] = mtx[i * cols + j];
                }
            }
        } else {                                               /* scalars / vectors: one register per element */
            for (int r = 0; r < n; r++)
                for (uint32_t j = 0; j < per && j < 4; j++)
                    if ((uint32_t)r * per + j < p->nfloat) regs[r][j] = p->f[(uint32_t)r * per + j];
        }
        if (m->set == 2) {
            if (s->vs) { if (m->reg + n <= 256) memcpy(g_dev.vsc[m->reg], regs, 16 * (size_t)n); }
            else if (m->reg + n <= 224) memcpy(g_dev.psc[m->reg], regs, 16 * (size_t)n);
        } else if (m->set == 1 && m->reg + n <= 16) {
            for (int r = 0; r < n; r++) for (int j = 0; j < 4; j++) (s->vs ? g_dev.vsi : g_dev.psi)[m->reg + r][j] = (int32_t)regs[r][j];
        } else if (m->set == 0 && m->reg + n <= 16) {
            for (int r = 0; r < n; r++) (s->vs ? g_dev.vsb : g_dev.psb)[m->reg + r] = regs[r][0] != 0.0f;
        }
    }
}
static void apply(Effect *e) {
    if (e->cur_tech < 0 || e->cur_pass < 0) return;
    Pass *ps = &e->t[e->cur_tech].pass[e->cur_pass];
    uint32_t vi = ps->vs_obj < 256 ? e->obj_shader[ps->vs_obj] : 0, pi = ps->ps_obj < 256 ? e->obj_shader[ps->ps_obj] : 0;
    if (vi) { g_dev.vs = e->sh[vi - 1].shader; upload(e, &e->sh[vi - 1]); }
    if (pi) { g_dev.ps = e->sh[pi - 1].shader; upload(e, &e->sh[pi - 1]); }
}

/* ---- methods (this = ARG(0)) ---- */
#define FX Effect *e = fx_of(ARG(0)); if (!e) return 0x8876086Cu
static uint32_t m_qi(CPU *c, uint32_t a) { (void)c; W32(ARG(2), ARG(0)); return 0; }
static uint32_t m_ref(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
static uint32_t m_ok(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
static uint32_t m_get_param(CPU *c, uint32_t a) {                 /* (this, parent, index) */
    (void)c; FX;
    return !ARG(1) && ARG(2) < (uint32_t)e->np ? e->handles + 4 * ARG(2) : 0;
}
static uint32_t m_param_by_name(CPU *c, uint32_t a) {             /* (this, parent, name) */
    (void)c; FX;
    for (int i = 0; i < e->np; i++) if (!strcmp(e->p[i].name, STR(ARG(2)))) return e->handles + 4u * (uint32_t)i;
    usf_log("d3dx effect: no parameter %s\n", STR(ARG(2)));
    return 0;
}
static uint32_t m_param_by_semantic(CPU *c, uint32_t a) {
    (void)c; FX;
    for (int i = 0; i < e->np; i++) if (!strcasecmp(e->p[i].semantic, STR(ARG(2)))) return e->handles + 4u * (uint32_t)i;
    return 0;
}
static uint32_t m_get_tech(CPU *c, uint32_t a) { (void)c; FX; return ARG(1) < (uint32_t)e->nt ? e->handles + 4 * (MAXP + ARG(1)) : 0; }
static uint32_t m_tech_by_name(CPU *c, uint32_t a) {
    (void)c; FX;
    int t = tech_of(e, ARG(1));
    return t >= 0 ? e->handles + 4u * (MAXP + (uint32_t)t) : 0;
}
static uint32_t m_get_desc(CPU *c, uint32_t a) {                  /* D3DXEFFECT_DESC: creator, params, techs, funcs */
    (void)c; FX;
    W32(ARG(1), 0); W32(ARG(1) + 4, (uint32_t)e->np); W32(ARG(1) + 8, (uint32_t)e->nt); W32(ARG(1) + 12, 0);
    return 0;
}
static uint32_t m_tech_desc(CPU *c, uint32_t a) {                 /* D3DXTECHNIQUE_DESC: name, passes, annotations */
    (void)c; FX;
    int t = tech_of(e, ARG(1));
    if (t < 0) return 0x8876086Cu;
    W32(ARG(2), e->handles + 4u * (MAXP + (uint32_t)t)); W32(ARG(2) + 4, (uint32_t)e->t[t].npass); W32(ARG(2) + 8, 0);
    return 0;
}
static uint32_t m_set_tech(CPU *c, uint32_t a) {
    (void)c; FX;
    int t = tech_of(e, ARG(1));
    if (t < 0) return 0x8876086Cu;
    e->cur_tech = t;
    return 0;
}
static uint32_t m_cur_tech(CPU *c, uint32_t a) { (void)c; FX; return e->cur_tech >= 0 ? e->handles + 4u * (MAXP + (uint32_t)e->cur_tech) : 0; }
static uint32_t m_validate(CPU *c, uint32_t a) { (void)c; FX; return tech_of(e, ARG(1)) >= 0 ? 0 : 0x8876086Cu; }
static uint32_t m_next_valid(CPU *c, uint32_t a) {                /* (this, after, &next) */
    (void)c; FX;
    int t = ARG(1) ? tech_of(e, ARG(1)) + 1 : 0;
    if (t >= e->nt) { W32(ARG(2), 0); return 1; }               /* S_FALSE */
    W32(ARG(2), e->handles + 4u * (MAXP + (uint32_t)t));
    return 0;
}
static uint32_t m_is_used(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
static uint32_t m_begin(CPU *c, uint32_t a) {                     /* (this, &passes, flags) */
    (void)c; FX;
    if (e->cur_tech < 0) e->cur_tech = 0;
    if (ARG(1)) W32(ARG(1), (uint32_t)e->t[e->cur_tech].npass);
    return 0;
}
static uint32_t m_begin_pass(CPU *c, uint32_t a) {
    (void)c; FX;
    if (e->cur_tech < 0 || ARG(1) >= (uint32_t)e->t[e->cur_tech].npass) return 0x8876086Cu;
    e->cur_pass = (int)ARG(1);
    apply(e);
    return 0;
}
static uint32_t m_commit(CPU *c, uint32_t a) { (void)c; FX; apply(e); return 0; }
static uint32_t m_end_pass(CPU *c, uint32_t a) { (void)c; FX; e->cur_pass = -1; return 0; }

/* setters: (this, handle, value...) */
static Param *param(Effect *e, uint32_t h) { int i = param_of(e, h); return i >= 0 ? &e->p[i] : NULL; }
static void set_floats(Param *p, const float *v, uint32_t n) {
    if (!p || !p->f) return;
    if (n > p->nfloat) n = p->nfloat;
    memcpy(p->f, v, 4 * n);
}
static uint32_t m_set_value(CPU *c, uint32_t a) {                 /* (this, h, data, bytes) */
    (void)c; FX;
    Param *p = param(e, ARG(1));
    if (!p) return 0x8876086Cu;
    if (p->cls == C_OBJECT && p->type >= 5 && p->type <= 9) { p->tex = R32(ARG(2)); return 0; }
    set_floats(p, (const float *)HOST(ARG(2)), ARG(3) / 4);
    return 0;
}
static uint32_t m_set_float(CPU *c, uint32_t a) { (void)c; FX; Param *p = param(e, ARG(1)); if (p && p->f) memcpy(p->f, HOST(a + 8), 4); return 0; }
static uint32_t m_set_int(CPU *c, uint32_t a) { (void)c; FX; Param *p = param(e, ARG(1)); if (p && p->f) p->f[0] = (float)(int32_t)ARG(2); return 0; }
static uint32_t m_set_bool(CPU *c, uint32_t a) { (void)c; FX; Param *p = param(e, ARG(1)); if (p && p->f) p->f[0] = ARG(2) ? 1.0f : 0.0f; return 0; }
static uint32_t m_set_float_array(CPU *c, uint32_t a) { (void)c; FX; set_floats(param(e, ARG(1)), (const float *)HOST(ARG(2)), ARG(3)); return 0; }
static uint32_t m_set_vector(CPU *c, uint32_t a) {
    (void)c; FX;
    Param *p = param(e, ARG(1));
    if (p) set_floats(p, (const float *)HOST(ARG(2)), p->cols && p->cols < 4 ? p->cols : 4);
    return 0;
}
static uint32_t m_set_vector_array(CPU *c, uint32_t a) {          /* (this, h, vectors, count): 4 floats each */
    (void)c; FX;
    Param *p = param(e, ARG(1));
    if (!p || !p->f) return 0;
    uint32_t per = p->cols ? p->cols : 4;
    for (uint32_t i = 0; i < ARG(3) && (i + 1) * per <= p->nfloat; i++) memcpy(p->f + i * per, HOST(ARG(2) + 16 * i), 4 * per);
    return 0;
}
static void set_matrix(Param *p, uint32_t src, uint32_t el, int transpose) {   /* src: D3DXMATRIX (4x4 row-major) */
    if (!p || !p->f) return;
    uint32_t rows = p->rows ? p->rows : 4, cols = p->cols ? p->cols : 4;
    if ((el + 1) * rows * cols > p->nfloat) return;
    for (uint32_t r = 0; r < rows; r++)
        for (uint32_t k = 0; k < cols; k++)
            p->f[el * rows * cols + r * cols + k] = RF32(src + 4 * (transpose ? k * 4 + r : r * 4 + k));
}
static uint32_t m_set_matrix(CPU *c, uint32_t a) { (void)c; FX; set_matrix(param(e, ARG(1)), ARG(2), 0, 0); return 0; }
static uint32_t m_set_matrix_t(CPU *c, uint32_t a) { (void)c; FX; set_matrix(param(e, ARG(1)), ARG(2), 0, 1); return 0; }
static uint32_t m_set_matrix_array(CPU *c, uint32_t a) {
    (void)c; FX;
    for (uint32_t i = 0; i < ARG(3); i++) set_matrix(param(e, ARG(1)), ARG(2) + 64 * i, i, 0);
    return 0;
}
static uint32_t m_set_texture(CPU *c, uint32_t a) { (void)c; FX; Param *p = param(e, ARG(1)); if (p) p->tex = ARG(2); return 0; }
static uint32_t m_get_texture(CPU *c, uint32_t a) { (void)c; FX; Param *p = param(e, ARG(1)); W32(ARG(2), p ? p->tex : 0); return 0; }
static uint32_t m_set_raw(CPU *c, uint32_t a) {                   /* (this, h, data, byte offset, bytes) */
    (void)c; FX;
    Param *p = param(e, ARG(1));
    if (p && p->f && ARG(3) + ARG(4) <= 4 * p->nfloat) memcpy((uint8_t *)p->f + ARG(3), HOST(ARG(2)), ARG(4));
    return 0;
}
static uint32_t m_get_device(CPU *c, uint32_t a) { (void)c; extern uint32_t g_device_guest; W32(ARG(1), g_device_guest); return 0; }

#define NSLOTS 79
static HleFn vt[NSLOTS];
static const uint8_t nargs[NSLOTS] = {
    3, 1, 1, 2, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 3, 3, 2, 2, 3, 3,      /* QI .. GetAnnotationByName */
    4, 4, 3, 3, 4, 4, 3, 3, 4, 4, 3, 3, 4, 4, 3, 3, 4, 4, 3, 3,      /* SetValue .. GetMatrix */
    4, 4, 4, 4, 3, 3, 4, 4, 4, 4, 3, 3, 3, 3, 3, 3, 4,               /* SetMatrixArray .. SetArrayRange */
    2, 2, 1, 2, 3, 3, 3, 2, 1, 1, 1, 2, 1, 1, 2, 2, 1, 2, 2, 2, 3, 5 /* GetPool .. SetRawValue */
};
static uint32_t vt_guest;

static void init_vtable(void) {
    if (vt_guest) return;
    vt[0] = m_qi; vt[1] = m_ref; vt[2] = m_ref; vt[3] = m_get_desc; vt[5] = m_tech_desc;
    vt[8] = m_get_param; vt[9] = m_param_by_name; vt[10] = m_param_by_semantic; vt[12] = m_get_tech; vt[13] = m_tech_by_name;
    vt[20] = m_set_value; vt[22] = m_set_bool; vt[26] = m_set_int; vt[30] = m_set_float; vt[32] = m_set_float_array;
    vt[34] = m_set_vector; vt[36] = m_set_vector_array; vt[38] = m_set_matrix; vt[40] = m_set_matrix_array;
    vt[44] = m_set_matrix_t; vt[52] = m_set_texture; vt[53] = m_get_texture;
    vt[58] = m_set_tech; vt[59] = m_cur_tech; vt[60] = m_validate; vt[61] = m_next_valid; vt[62] = m_is_used;
    vt[63] = m_begin; vt[64] = m_begin_pass; vt[65] = m_commit; vt[66] = m_end_pass; vt[67] = m_ok; vt[68] = m_get_device;
    vt[69] = m_ok; vt[70] = m_ok; vt[71] = m_ok; vt[78] = m_set_raw;
    uint32_t o = obj_new_n("ID3DXEffect", NSLOTS, vt, nargs, 16);
    vt_guest = R32(o);
}

/* D3DXCreateEffect(device, data, size, defines, include, flags, pool, &effect, &errors) */
uint32_t x_D3DXCreateEffect(CPU *c, uint32_t a) {
    (void)c;
    if (ARG(8)) W32(ARG(8), 0);
    if (neffects >= 64) return 0x8007000Eu;
    Effect *e = calloc(1, sizeof *e);
    e->cur_tech = 0;
    e->cur_pass = -1;
    int r = parse(e, ARG(1), ARG(2));
    if (r) {
        fprintf(stderr, "usf: D3DXCreateEffect: not a compiled effect (%d, %u bytes, starts %08x)\n", r, ARG(2), R32(ARG(1)));
        free(e);
        return FXERR;
    }
    init_vtable();
    e->handles = gcalloc(4u * (MAXP + MAXT + MAXT * 4));
    uint32_t g = gcalloc(16);
    W32(g, vt_guest);
    W32(g + 8, (uint32_t)neffects);
    e->guest = g;
    effects[neffects++] = e;
    W32(ARG(7), g);
    usf_log("D3DXCreateEffect: %d parameters, %d techniques, %d shaders\n", e->np, e->nt, e->nsh);
    return 0;
}
