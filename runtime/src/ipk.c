/* Read access to the game's archive (GameData/bundle_PC.ipk) for the runtime's own needs.
 *
 * What it is for: the game reads its archive itself (through CreateFile/ReadFile). The runtime only needs a few
 * files from it directly: the precompiled shaders under shaders/compiled/win32/ (see d3dx9_math.c,
 * D3DXCompileShader). This reads the IPK directory once (version 3, big-endian: per entry part count, size,
 * compressed size, timestamp, offsets, UTF-16BE path) and returns a file's bytes.
 * What it does not do: compressed entries (zlib; the shader files are stored), or write anything.
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "hle.h"
#include "lite.h"

typedef struct { char path[160]; uint64_t off; uint32_t size, zsize; } Entry;
static Entry *ents;
static uint32_t nents;
static int tried;
static char ipk_path[1200];

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* n bytes of the archive at off: the file, or the compressed copy (bundle_PC.lite, lite_read.c) */
static uint64_t read_at(void *buf, uint64_t n, uint64_t off) {
    char lp[1100];
    if (lite_path_for(ipk_path, lp, sizeof lp)) {
        Lite *l = lite_shared(lp);
        int64_t r = l ? lite_pread(l, buf, n, off) : -1;
        return r < 0 ? 0 : (uint64_t)r;
    }
    FILE *f = fopen(ipk_path, "rb");
    if (!f) return 0;
    uint64_t r = fseeko(f, (off_t)off, SEEK_SET) ? 0 : fread(buf, 1, n, f);
    fclose(f);
    return r;
}

static void load(void) {
    if (tried) return;
    tried = 1;
    snprintf(ipk_path, sizeof ipk_path, "%s/GameData/bundle_PC.ipk", g_game_dir);
    uint8_t head[0x30];
    if (read_at(head, sizeof head, 0) != sizeof head || be32(head) != 0x50EC12BAu) { fprintf(stderr, "usf: cannot open %s\n", ipk_path); return; }
    uint32_t base = be32(head + 12), count = be32(head + 0x2C);
    uint8_t *d = malloc(base);
    if (!d || read_at(d, base, 0) != base) { free(d); return; }
    ents = calloc(count, sizeof *ents);
    uint32_t p = 0x30;
    for (uint32_t i = 0; i < count && p + 24 < base; i++) {
        uint32_t parts = be32(d + p), size = be32(d + p + 4), zsize = be32(d + p + 8);
        p += 20;
        uint64_t off = (uint64_t)be32(d + p) << 32 | be32(d + p + 4);
        p += 8 * parts;
        uint32_t n = be32(d + p);
        p += 4;
        Entry *e = &ents[nents++];
        uint32_t k;
        for (k = 0; k < n && k < sizeof e->path - 1; k++) e->path[k] = (char)d[p + 2 * k + 1];
        e->path[k] = 0;
        p += 2 * n;
        e->off = base + off;
        e->size = size;
        e->zsize = zsize;
    }
    free(d);
    usf_log("ipk: %u entries in %s\n", nents, ipk_path);
}

/* The bytes of `path` (case-insensitive), malloc'd; NULL if absent or compressed. */
uint8_t *ipk_read(const char *path, uint32_t *size);
uint8_t *ipk_read(const char *path, uint32_t *size) {
    load();
    for (uint32_t i = 0; i < nents; i++) {
        if (strcasecmp(ents[i].path, path)) continue;
        if (ents[i].zsize) { fprintf(stderr, "usf: ipk: %s is compressed (not supported here)\n", path); return NULL; }
        uint8_t *b = malloc(ents[i].size ? ents[i].size : 1);
        if (!b || read_at(b, ents[i].size, ents[i].off) != ents[i].size) { free(b); return NULL; }
        *size = ents[i].size;
        return b;
    }
    return NULL;
}
