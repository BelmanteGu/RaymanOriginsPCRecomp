/* advapi32 (registry, user name, event log), version.dll and psapi.dll, as the game uses them.
 *
 * What it does: the registry is a small text file in our data folder (<data>/registry.txt, one `key\value=text`
 * line per value): the game keeps its settings under HKCU\Software\Ubisoft\RaymanOrigins\Settings (screen size,
 * etc.) as strings or dwords. GetFileVersionInfoW / VerQueryValueW read the executable's own version resource from
 * the loaded image. EnumProcesses and friends describe one process: the game itself (its "already running?" check).
 * What it does not do: registry keys other than as name prefixes, binary values, or other processes.
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "game.h"
#include "hle.h"

extern uint32_t g_image_base;

/* ---- registry ---- */
#define MAXV 256
typedef struct { char name[300]; uint32_t type; uint8_t data[512]; uint32_t size; } RegVal;
static RegVal vals[MAXV];
static int nvals, loaded;
static char keys[64][256];
static pthread_mutex_t reg_lock = PTHREAD_MUTEX_INITIALIZER;

static void reg_path(char *out, size_t n) { snprintf(out, n, "%s/registry.txt", g_data_dir); }

static void reg_load(void) {
    if (loaded) return;
    loaded = 1;
    char p[1100], line[1400];
    reg_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    while (f && fgets(line, sizeof line, f) && nvals < MAXV) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '='), *colon = eq ? strchr(eq, ':') : NULL;
        if (!eq || !colon) continue;
        *eq = 0;
        RegVal *v = &vals[nvals++];
        snprintf(v->name, sizeof v->name, "%s", line);
        v->type = (uint32_t)strtoul(eq + 1, NULL, 10);
        const char *hex = colon + 1;
        for (v->size = 0; hex[0] && hex[1] && v->size < sizeof v->data; hex += 2)
            v->data[v->size++] = (uint8_t)strtoul((char[3]){hex[0], hex[1], 0}, NULL, 16);
    }
    if (f) fclose(f);
}
static void reg_save(void) {
    char p[1100];
    reg_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    for (int i = 0; i < nvals; i++) {
        fprintf(f, "%s=%u:", vals[i].name, vals[i].type);
        for (uint32_t k = 0; k < vals[i].size; k++) fprintf(f, "%02x", vals[i].data[k]);
        fprintf(f, "\n");
    }
    fclose(f);
}
static uint32_t key_handle(uint32_t root, uint32_t sub) {        /* a handle names a key path */
    char name[256];
    if (sub) w2utf8(sub, name, sizeof name); else name[0] = 0;
    size_t l = strlen(name);
    while (l && name[l - 1] == '\\') name[--l] = 0;
    char full[256];
    snprintf(full, sizeof full, "%08x\\%s", root, name);
    pthread_mutex_lock(&reg_lock);
    int i;
    for (i = 0; i < 64 && keys[i][0]; i++)
        if (!strcasecmp(keys[i], full)) break;
    if (i < 64 && !keys[i][0]) snprintf(keys[i], sizeof keys[i], "%s", full);
    pthread_mutex_unlock(&reg_lock);
    return i < 64 ? 0x7A000000u + (uint32_t)i : 0;
}
static RegVal *find_val(uint32_t hkey, uint32_t wname, int create) {
    if (hkey < 0x7A000000u || hkey >= 0x7A000040u) return NULL;
    char vn[128], full[300];
    if (wname) w2utf8(wname, vn, sizeof vn); else vn[0] = 0;
    snprintf(full, sizeof full, "%s\\%s", keys[hkey - 0x7A000000u], vn);
    reg_load();
    for (int i = 0; i < nvals; i++)
        if (!strcasecmp(vals[i].name, full)) return &vals[i];
    if (!create || nvals >= MAXV) return NULL;
    snprintf(vals[nvals].name, sizeof vals[nvals].name, "%s", full);
    return &vals[nvals++];
}
uint32_t x_RegOpenKeyExW(CPU *c, uint32_t a) {                   /* (root, subkey, options, sam, &result) */
    (void)c;
    W32(ARG(4), key_handle(ARG(0), ARG(1)));
    return 0;
}
uint32_t x_RegCreateKeyExW(CPU *c, uint32_t a) {                 /* (root, subkey, 0, class, opt, sam, sec, &result, &disp) */
    (void)c;
    W32(ARG(7), key_handle(ARG(0), ARG(1)));
    if (ARG(8)) W32(ARG(8), 2);                                    /* REG_OPENED_EXISTING_KEY */
    return 0;
}
uint32_t x_RegCloseKey(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
/* The video settings are the runtime's, not the registry's: the window size (RAYMAN_RES=WxH, or 21:9 that fits the
 * desktop) and windowed mode (RAYMAN_FULLSCREEN=1 for full screen). Returns 1 if `name` was one of them. */
static int video_setting(uint32_t wname, uint32_t type_out, uint32_t data, uint32_t size_io) {
    char vn[64];
    w2utf8(wname, vn, sizeof vn);
    int w = GAME_DEFAULT_W, h = GAME_DEFAULT_H;
    const char *r = getenv("RAYMAN_RES");
    if (r) sscanf(r, "%dx%d", &w, &h);
    char text[16];
    if (!strcasecmp(vn, "ScreenWidth") || !strcasecmp(vn, "ScreenHeight")) {
        snprintf(text, sizeof text, "%d", !strcasecmp(vn, "ScreenWidth") ? w : h);
        uint32_t need = 2u * ((uint32_t)strlen(text) + 1);
        if (type_out) W32(type_out, 1);                            /* REG_SZ */
        if (data && size_io && R32(size_io) >= need) utf8_to_w(text, data, need / 2);
        if (size_io) W32(size_io, need);
        return 1;
    }
    if (!strcasecmp(vn, "FullScreen")) {
        if (type_out) W32(type_out, 4);                            /* REG_DWORD */
        if (data) W32(data, getenv("RAYMAN_FULLSCREEN") ? 1u : 0u);
        if (size_io) W32(size_io, 4);
        return 1;
    }
    return 0;
}
uint32_t x_RegQueryValueExW(CPU *c, uint32_t a) {                /* (key, name, 0, &type, data, &size) */
    (void)c;
    if (ARG(1) && video_setting(ARG(1), ARG(3), ARG(4), ARG(5))) return 0;
    pthread_mutex_lock(&reg_lock);
    RegVal *v = find_val(ARG(0), ARG(1), 0);
    uint32_t r = 2;                                                /* ERROR_FILE_NOT_FOUND */
    if (v) {
        if (ARG(3)) W32(ARG(3), v->type);
        uint32_t room = ARG(5) ? R32(ARG(5)) : 0;
        r = 0;
        if (ARG(4)) { if (room < v->size) r = 234; else memcpy(HOST(ARG(4)), v->data, v->size); }   /* ERROR_MORE_DATA */
        if (ARG(5)) W32(ARG(5), v->size);
    }
    pthread_mutex_unlock(&reg_lock);
    return r;
}
uint32_t x_RegSetValueExW(CPU *c, uint32_t a) {                  /* (key, name, 0, type, data, size) */
    (void)c;
    pthread_mutex_lock(&reg_lock);
    RegVal *v = find_val(ARG(0), ARG(1), 1);
    if (v) {
        v->type = ARG(3);
        v->size = ARG(5) < sizeof v->data ? ARG(5) : (uint32_t)sizeof v->data;
        memcpy(v->data, HOST(ARG(4)), v->size);
        reg_save();
    }
    pthread_mutex_unlock(&reg_lock);
    return v ? 0 : 5;
}
uint32_t x_GetUserNameW(CPU *c, uint32_t a) {
    (void)c;
    if (R32(ARG(1)) < 7) { W32(ARG(1), 7); return 0; }
    utf8_to_w("player", ARG(0), 7);
    W32(ARG(1), 7);
    return 1;
}
uint32_t x_RegisterEventSourceA(CPU *c, uint32_t a) { (void)c; (void)a; return 0x7B000001u; }
uint32_t x_DeregisterEventSource(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
uint32_t x_ReportEventA(CPU *c, uint32_t a) {                    /* (h, type, cat, id, sid, nstrings, size, strings, data) */
    (void)c;
    for (uint32_t i = 0; i < (ARG(5) & 0xFFFF) && ARG(7); i++) fprintf(stderr, "[event log] %s\n", STR(R32(ARG(7) + 4 * i)));
    return 1;
}

/* ---- version.dll: the executable's VS_VERSION_INFO resource ---- */
static uint32_t res_find(uint32_t dir, uint32_t base, uint32_t id) {   /* entry for an integer id, or the first */
    uint32_t n = R16(dir + 12) + R16(dir + 14);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = dir + 16 + 8 * i;
        if (id == 0xFFFFFFFFu || R32(e) == id) return R32(e + 4);
    }
    (void)base;
    return 0xFFFFFFFFu;
}
static uint32_t version_resource(uint32_t *size) {
    uint32_t pe = g_image_base + R32(g_image_base + 0x3C), rva = R32(pe + 24 + 112);
    if (!rva) return 0;
    uint32_t root = g_image_base + rva, e = res_find(root, root, 16);   /* RT_VERSION */
    for (int level = 0; level < 2 && e != 0xFFFFFFFFu && (e & 0x80000000u); level++)
        e = res_find(root + (e & 0x7FFFFFFFu), root, 0xFFFFFFFFu);
    if (e == 0xFFFFFFFFu || (e & 0x80000000u)) {
        if (e != 0xFFFFFFFFu) e = res_find(root + (e & 0x7FFFFFFFu), root, 0xFFFFFFFFu);
        if (e == 0xFFFFFFFFu || (e & 0x80000000u)) return 0;
    }
    uint32_t data = root + e;                                      /* IMAGE_RESOURCE_DATA_ENTRY */
    *size = R32(data + 4);
    return g_image_base + R32(data);
}
uint32_t x_GetFileVersionInfoSizeW(CPU *c, uint32_t a) {
    uint32_t size = 0;
    if (ARG(1)) W32(ARG(1), 0);
    if (!version_resource(&size)) { set_last_error(c, 1813); return 0; }
    return size;
}
uint32_t x_GetFileVersionInfoW(CPU *c, uint32_t a) {             /* (file, 0, size, buffer): any file -> the exe's */
    uint32_t size = 0, p = version_resource(&size);
    if (!p) { set_last_error(c, 1813); return 0; }
    memcpy(HOST(ARG(3)), HOST(p), size < ARG(2) ? size : ARG(2));
    return 1;
}
uint32_t x_VerQueryValueW(CPU *c, uint32_t a) {                  /* (block, "\\", &ptr, &len): the fixed info */
    (void)c;
    char sub[64];
    w2utf8(ARG(1), sub, sizeof sub);
    uint32_t blk = ARG(0), size = R16(blk);
    if (strcmp(sub, "\\")) { usf_log("VerQueryValueW(%s): not answered\n", sub); return 0; }
    for (uint32_t o = 0; o + 52 <= size; o += 4)
        if (R32(blk + o) == 0xFEEF04BDu) { W32(ARG(2), blk + o); W32(ARG(3), 52); return 1; }
    return 0;
}

/* ---- psapi: one process, the game ---- */
uint32_t x_EnumProcesses(CPU *c, uint32_t a) {                   /* (ids, bytes, &needed) */
    (void)c;
    if (ARG(1) >= 4) W32(ARG(0), 0x1000);
    W32(ARG(2), 4);
    return 1;
}
uint32_t k32_OpenProcess(CPU *c, uint32_t a) { (void)c; return ARG(2) == 0x1000 ? 0xFFFFFFFFu : 0; }
uint32_t x_EnumProcessModules(CPU *c, uint32_t a) {              /* (process, modules, bytes, &needed) */
    (void)c;
    if (ARG(2) >= 4) W32(ARG(1), g_image_base);
    W32(ARG(3), 4);
    return 1;
}
uint32_t x_GetModuleBaseNameW(CPU *c, uint32_t a) {              /* (process, module, name, size) */
    (void)c;
    utf8_to_w(GAME_EXE, ARG(2), ARG(3));
    return (uint32_t)strlen(GAME_EXE);
}
