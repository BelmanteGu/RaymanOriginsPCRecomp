/* Maps the game executable into guest memory at its own base and binds its imports to thunks.
 *
 * What it does: reads the executable from the Steam folder, copies the headers and every section to
 * 0x00400000 + RVA (the image has no relocations, so this is the only place it can go), and fills every import
 * address table slot with the thunk of that import (usf_thunk). Ordinal imports are named "#<n>".
 * What it does not do: run DllMain-like code (the game has no TLS callbacks) or load the DLLs shipped next to the
 * executable (their functions are answered by the runtime).
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "hle.h"

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

uint32_t g_image_base, g_image_size, g_entry;

static void lower(char *s) { for (; *s; s++) *s = (char)tolower((unsigned char)*s); }

/* DLLs whose functions the game imports by ordinal: the ordinal -> name maps of Windows' ws2_32.dll / oleaut32.dll */
static const struct { const char *dll; unsigned ord; const char *name; } ordinals[] = {
    {"ws2_32.dll", 1, "accept"}, {"ws2_32.dll", 2, "bind"}, {"ws2_32.dll", 3, "closesocket"},
    {"ws2_32.dll", 4, "connect"}, {"ws2_32.dll", 8, "htonl"}, {"ws2_32.dll", 9, "htons"},
    {"ws2_32.dll", 10, "ioctlsocket"}, {"ws2_32.dll", 11, "inet_addr"}, {"ws2_32.dll", 12, "inet_ntoa"},
    {"ws2_32.dll", 14, "ntohl"}, {"ws2_32.dll", 15, "ntohs"}, {"ws2_32.dll", 16, "recv"},
    {"ws2_32.dll", 17, "recvfrom"}, {"ws2_32.dll", 18, "select"}, {"ws2_32.dll", 19, "send"},
    {"ws2_32.dll", 20, "sendto"}, {"ws2_32.dll", 21, "setsockopt"}, {"ws2_32.dll", 23, "socket"},
    {"ws2_32.dll", 51, "gethostbyaddr"}, {"ws2_32.dll", 52, "gethostbyname"}, {"ws2_32.dll", 55, "getservbyname"},
    {"ws2_32.dll", 56, "getservbyport"}, {"ws2_32.dll", 57, "gethostname"}, {"ws2_32.dll", 111, "WSAGetLastError"},
    {"ws2_32.dll", 112, "WSASetLastError"}, {"ws2_32.dll", 115, "WSAStartup"}, {"ws2_32.dll", 116, "WSACleanup"},
    {"xinput1_3.dll", 2, "XInputGetState"}, {"xinput1_3.dll", 3, "XInputSetState"},
    {"oleaut32.dll", 2, "SysAllocString"}, {"oleaut32.dll", 6, "SysFreeString"},
    {NULL, 0, NULL},
};

static const char *ordinal_name(const char *dll, unsigned ord) {
    for (int i = 0; ordinals[i].dll; i++)
        if (ordinals[i].ord == ord && !strcmp(ordinals[i].dll, dll)) return ordinals[i].name;
    return NULL;
}

/* Lifted DLLs shipped with the game: loaded at their preferred base before the executable, so the executable's
 * imports from them resolve to their (lifted) exports instead of thunks. */
typedef struct { char name[64]; uint32_t base, size, entry, exports; } Module;
static Module mods[4];
static int nmods;

static uint32_t export_of(const Module *m, const char *name) {
    uint32_t ed = m->exports;
    if (!ed) return 0;
    uint32_t n = R32(ed + 24), names = m->base + R32(ed + 32), ords = m->base + R32(ed + 36), funcs = m->base + R32(ed + 28);
    uint32_t lo = 0, hi = n;                                   /* the name table is sorted */
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        int c = strcmp(STR(m->base + R32(names + 4 * mid)), name);
        if (!c) return m->base + R32(funcs + 4 * R16(ords + 2 * mid));
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return 0;
}

static int load_pe(const char *path, uint32_t *base_out, uint32_t *size_out, uint32_t *entry_out, int reserve) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc((size_t)n);
    if (!file || fread(file, 1, (size_t)n, f) != (size_t)n) { fclose(f); return -2; }
    fclose(f);
    uint32_t pe = rd32(file + 0x3C);
    uint32_t nsec = file[pe + 6] | file[pe + 7] << 8;
    uint32_t opt = pe + 24, optsize = file[pe + 20] | file[pe + 21] << 8;
    uint32_t base = rd32(file + opt + 28), size = rd32(file + opt + 56);
    if (reserve && gpages(base, size) != base) { free(file); return -3; }   /* a DLL: claim its range first */
    uint32_t hdr = rd32(file + opt + 60);
    memcpy(HOST(base), file, hdr);
    for (uint32_t i = 0; i < nsec; i++) {
        const uint8_t *s = file + opt + optsize + 40 * i;
        uint32_t vsize = rd32(s + 8), va = rd32(s + 12), raw = rd32(s + 16), off = rd32(s + 20);
        memcpy(HOST(base + va), file + off, raw < vsize ? raw : vsize);
    }
    *base_out = base;
    *size_out = size;
    *entry_out = base + rd32(file + opt + 16);
    free(file);

    uint32_t dir = base + R32(base + opt + 104);
    int bound = 0, from_mods = 0;
    for (uint32_t d = dir; R32(d + 12); d += 20) {
        char dll[64];
        snprintf(dll, sizeof dll, "%s", STR(base + R32(d + 12)));
        lower(dll);
        const Module *m = NULL;
        for (int k = 0; k < nmods; k++) if (!strcmp(mods[k].name, dll)) m = &mods[k];
        uint32_t names = R32(d) ? R32(d) : R32(d + 16);
        for (uint32_t k = 0;; k++) {
            uint32_t ent = R32(base + names + 4 * k);
            if (!ent) break;
            char name[512];
            if (ent & 0x80000000u) {
                const char *nm = ordinal_name(dll, ent & 0xFFFFu);
                if (nm) snprintf(name, sizeof name, "%s", nm);
                else snprintf(name, sizeof name, "#%u", ent & 0xFFFFu);
            }
            else snprintf(name, sizeof name, "%s", STR(base + ent + 2));
            uint32_t target = m && name[0] != '#' ? export_of(m, name) : 0;
            if (target) from_mods++;
            else {
                const HleImport *imp = hle_find(dll, name);
                target = imp && imp->conv == CONV_DATA && imp->fn ? imp->fn(NULL, 0) : usf_thunk(dll, name);
            }
            W32(base + R32(d + 16) + 4 * k, target);
            bound++;
        }
    }
    usf_log("%s: %08x..%08x, %d imports bound (%d to lifted DLLs)\n", path, base, base + size, bound, from_mods);
    return 0;
}

/* A DLL that has lifted code (see usf/table.h): loaded, its exports kept for the executable's imports. */
int load_module(const char *path, const char *name, uint32_t *entry) {
    if (nmods >= 4) return -1;
    Module *m = &mods[nmods];
    int r = load_pe(path, &m->base, &m->size, &m->entry, 1);
    if (r) return r;
    snprintf(m->name, sizeof m->name, "%s", name);
    m->exports = R32(m->base + R32(m->base + 0x3C) + 24 + 96) ? m->base + R32(m->base + R32(m->base + 0x3C) + 24 + 96) : 0;
    *entry = m->entry;
    nmods++;
    return 0;
}

uint32_t module_base(const char *name);
uint32_t module_base(const char *name) {
    for (int k = 0; k < nmods; k++) if (!strcasecmp(mods[k].name, name)) return mods[k].base;
    return 0;
}

extern uint32_t g_text_lo, g_text_hi;
int load_image(const char *path) {
    int r = load_pe(path, &g_image_base, &g_image_size, &g_entry, 0);
    if (!r) {                                                  /* the first section is the code (.text) */
        uint32_t pe = g_image_base + R32(g_image_base + 0x3C), sec = pe + 24 + R16(pe + 20);
        g_text_lo = g_image_base + R32(sec + 12);
        g_text_hi = g_text_lo + R32(sec + 8);
    }
    return r;
}
