/* D3DXAssembleShader: Direct3D 9 shader assembly text -> shader bytecode (the subset the game assembles at run time).
 *
 * What it does: tokenizes the text (version line, dcl_*, def, arithmetic instructions with destination write masks,
 * source swizzles and negation) and writes the same tokens the Microsoft assembler writes, with instruction lengths
 * (as SM2+ does), so the bytecode goes through sm3.c like every other shader. Comments (; and //) are skipped.
 * What it does not do: texture instructions of ps 1.x, flow control, relative addressing (reported as an error).
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "d3d9.h"

typedef struct { const char *name; uint32_t op; int nsrc; } Op;
static const Op ops[] = {
    {"nop", 0, 0}, {"mov", 1, 1}, {"add", 2, 2}, {"sub", 3, 2}, {"mad", 4, 3}, {"mul", 5, 2}, {"rcp", 6, 1}, {"rsq", 7, 1},
    {"dp3", 8, 2}, {"dp4", 9, 2}, {"min", 10, 2}, {"max", 11, 2}, {"slt", 12, 2}, {"sge", 13, 2}, {"exp", 14, 1},
    {"log", 15, 1}, {"frc", 19, 1}, {"lrp", 18, 3}, {"m4x4", 20, 2}, {"m4x3", 21, 2}, {"m3x4", 22, 2}, {"m3x3", 23, 2},
    {"m3x2", 24, 2}, {"pow", 32, 2}, {"abs", 35, 1}, {"nrm", 36, 1}, {"cmp", 88, 3}, {"cnd", 80, 3}, {"dp2add", 90, 3},
    {NULL, 0, 0}};

static const struct { const char *s; uint32_t usage; } usages[] = {
    {"position", 0}, {"blendweight", 1}, {"blendindices", 2}, {"normal", 3}, {"psize", 4}, {"texcoord", 5},
    {"tangent", 6}, {"binormal", 7}, {"color", 10}, {"fog", 11}, {"depth", 12}, {NULL, 0}};

static uint32_t regtok(uint32_t type, uint32_t num) {
    return 0x80000000u | (num & 0x7FF) | ((type & 7u) << 28) | ((type & 0x18u) << 8);
}

/* "oPos.x", "-c4.xyzw", "v0", "r0_bx2"... -> parameter token; dst: write mask, else swizzle */
static int param(const char *t, int dst, int ps, uint32_t *out) {
    int neg = 0;
    if (*t == '-') { neg = 1; t++; }
    char name[32];
    int k = 0;
    while (*t && *t != '.' && k < 31) name[k++] = (char)tolower((unsigned char)*t++);
    name[k] = 0;
    uint32_t type, num = 0;
    const char *n = name;
    if (!strcmp(n, "opos")) { type = 4; num = 0; }
    else if (!strcmp(n, "ofog")) { type = 4; num = 1; }
    else if (!strcmp(n, "opts")) { type = 4; num = 2; }
    else if (!strncmp(n, "od", 2)) { type = 5; num = (uint32_t)atoi(n + 2); }
    else if (!strncmp(n, "ot", 2)) { type = 6; num = (uint32_t)atoi(n + 2); }
    else if (!strncmp(n, "oc", 2)) { type = 8; num = (uint32_t)atoi(n + 2); }
    else if (n[0] == 'r') { type = 0; num = (uint32_t)atoi(n + 1); }
    else if (n[0] == 'v') { type = 1; num = (uint32_t)atoi(n + 1); }
    else if (n[0] == 'c') { type = 2; num = (uint32_t)atoi(n + 1); }
    else if (n[0] == 't') { type = 3; num = (uint32_t)atoi(n + 1); }
    else if (n[0] == 'a') { type = 3; num = 0; }
    else if (n[0] == 's') { type = 10; num = (uint32_t)atoi(n + 1); }
    else return -1;
    (void)ps;
    uint32_t tok = regtok(type, num);
    if (dst) {
        uint32_t mask = 0xF;
        if (*t == '.') { mask = 0; for (t++; *t; t++) mask |= *t == 'x' || *t == 'r' ? 1u : *t == 'y' || *t == 'g' ? 2u : *t == 'z' || *t == 'b' ? 4u : *t == 'w' || *t == 'a' ? 8u : 0u; }
        tok |= mask << 16;
    } else {
        uint32_t sw = 0xE4;
        if (*t == '.') {
            int c[4], nc = 0;
            for (t++; *t && nc < 4; t++) c[nc++] = *t == 'x' || *t == 'r' ? 0 : *t == 'y' || *t == 'g' ? 1 : *t == 'z' || *t == 'b' ? 2 : 3;
            for (int i = nc; i < 4; i++) c[i] = c[nc - 1];      /* .x -> .xxxx, .xy -> .xyyy */
            sw = (uint32_t)(c[0] | c[1] << 2 | c[2] << 4 | c[3] << 6);
        }
        tok |= sw << 16;
        if (neg) tok |= 1u << 24;
    }
    *out = tok;
    return 0;
}

/* Assembles `src` (len bytes); returns a malloc'd token array and its size in dwords, or NULL with err set. */
uint32_t *d3d_assemble(const char *src, uint32_t len, uint32_t *ntok, char *err, size_t errn) {
    char *text = malloc(len + 1);
    memcpy(text, src, len);
    text[len] = 0;
    for (char *p = text; *p; p++) {                             /* strip comments */
        if (*p == ';' || (p[0] == '/' && p[1] == '/')) while (*p && *p != '\n') *p++ = ' ';
        if (!*p) break;
    }
    uint32_t cap = 256, n = 0, *tok = malloc(cap * 4);
    int ps = 0;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n\r", &save); line; line = strtok_r(NULL, "\n\r", &save)) {
        char *w[8];
        int nw = 0;
        for (char *q = strtok(line, " \t,"); q && nw < 8; q = strtok(NULL, " \t,")) w[nw++] = q;
        if (!nw) continue;
        if (n + 16 > cap) { cap *= 2; tok = realloc(tok, cap * 4); }
        if (w[0][0] == '+') w[0]++;                              /* co-issue (ps 1.x): rgb / alpha halves, run in order */
        for (char *c = w[0]; *c; c++) *c = (char)tolower((unsigned char)*c);
        if (!strncmp(w[0], "vs_", 3) || !strncmp(w[0], "ps_", 3)) {
            ps = w[0][0] == 'p';
            uint32_t major = (uint32_t)(w[0][3] - '0'), minor = (uint32_t)(w[0][5] == 'x' ? 0 : w[0][5] - '0');
            tok[n++] = (ps ? 0xFFFF0000u : 0xFFFE0000u) | major << 8 | minor;
            continue;
        }
        if (!strncmp(w[0], "dcl", 3)) {
            uint32_t usage = 0, idx = 0;
            const char *u = w[0][3] == '_' ? w[0] + 4 : "";
            for (int i = 0; usages[i].s; i++)
                if (!strncmp(u, usages[i].s, strlen(usages[i].s))) { usage = usages[i].usage; idx = (uint32_t)atoi(u + strlen(usages[i].s)); }
            uint32_t d;
            if (nw < 2 || param(w[1], 1, ps, &d)) { snprintf(err, errn, "bad dcl"); free(text); free(tok); return NULL; }
            tok[n++] = 31u | 2u << 24;
            tok[n++] = 0x80000000u | usage | idx << 16;
            tok[n++] = d;
            continue;
        }
        if (!strcmp(w[0], "def") && nw == 6) {
            uint32_t d;
            param(w[1], 1, ps, &d);
            tok[n++] = 81u | 5u << 24;
            tok[n++] = d;
            for (int i = 0; i < 4; i++) { float f = strtof(w[2 + i], NULL); memcpy(&tok[n++], &f, 4); }
            continue;
        }
        if (!strcmp(w[0], "phase")) { tok[n++] = 0xFFFDu; continue; }   /* ps 1.4 */
        if (!strcmp(w[0], "texld") && nw == 3) {               /* ps 1.4: rN = sample(stage N, tM) */
            uint32_t d, s;
            if (param(w[1], 1, ps, &d) || param(w[2], 0, ps, &s)) { snprintf(err, errn, "bad texld"); free(text); free(tok); return NULL; }
            tok[n++] = 66u | 2u << 24;
            tok[n++] = d;
            tok[n++] = s;
            continue;
        }
        if (!strcmp(w[0], "tex") && nw == 2) {                  /* ps 1.x: tN = sample(stage N, texcoord N) */
            uint32_t d;
            if (param(w[1], 1, ps, &d)) { snprintf(err, errn, "bad tex"); free(text); free(tok); return NULL; }
            tok[n++] = 66u | 1u << 24;
            tok[n++] = d;
            continue;
        }
        char *sat = strstr(w[0], "_sat");
        if (sat) *sat = 0;
        const Op *o = NULL;
        for (int i = 0; ops[i].name; i++) if (!strcmp(ops[i].name, w[0])) o = &ops[i];
        if (!o || nw != 2 + o->nsrc - (o->op == 0 ? 2 : 0)) {
            snprintf(err, errn, "instruction '%s' not assembled", w[0]);
            free(text); free(tok); return NULL;
        }
        tok[n++] = o->op | (uint32_t)(1 + o->nsrc) << 24;
        uint32_t d;
        if (param(w[1], 1, ps, &d)) { snprintf(err, errn, "bad register %s", w[1]); free(text); free(tok); return NULL; }
        if (sat) d |= 1u << 20;
        tok[n++] = d;
        for (int i = 0; i < o->nsrc; i++) {
            uint32_t s;
            if (param(w[2 + i], 0, ps, &s)) { snprintf(err, errn, "bad register %s", w[2 + i]); free(text); free(tok); return NULL; }
            tok[n++] = s;
        }
    }
    tok[n++] = 0x0000FFFFu;
    free(text);
    *ntok = n;
    return tok;
}
