/* Objects the runtime hands to the game (Steam interfaces, Direct3D, DirectInput, DirectSound...), and the call-site
 * argument table that lets their methods pop the right number of bytes.
 *
 * What it does: obj_new builds, in guest memory, a vtable whose slots are thunks ("<iface>::<index>") and an object
 * whose first dword points at it; callsite_args reads private/analysis/callsite_args.bin (tools/funcs/callsites.py)
 * once and answers, for a return address, how many argument bytes the game pushed before that call.
 * What it does not do: reference counting or interface queries (the owners of each interface do that).
 */
#include <stdlib.h>
#include <string.h>

#include "hle.h"

static uint32_t *sites;
static uint32_t nsites;

/* the Android build links the table into the library (tools/android/build_native.sh) */
extern const unsigned char usf_callsites_blob[] __attribute__((weak)), usf_callsites_blob_end[] __attribute__((weak));

static void load_sites(void) {
    if (usf_callsites_blob && usf_callsites_blob_end > usf_callsites_blob) {
        size_t n = (size_t)(usf_callsites_blob_end - usf_callsites_blob);
        sites = malloc(n);
        memcpy(sites, usf_callsites_blob, n);
        nsites = (uint32_t)(n / 8);
        return;
    }
    FILE *f = fopen(getenv("USF_CALLSITES") ? getenv("USF_CALLSITES") : "private/analysis/callsite_args.bin", "rb");
    if (!f) { nsites = 0; sites = calloc(2, 4); fprintf(stderr, "usf: no call-site table (run tools/funcs/callsites.py)\n"); return; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    sites = malloc((size_t)n);
    nsites = (uint32_t)(fread(sites, 1, (size_t)n, f) / 8);
    fclose(f);
}

void callsites_init(void);
void callsites_init(void) { if (!sites) load_sites(); }

uint32_t callsite_args(uint32_t ret) {
    uint32_t lo = 0, hi = nsites;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (sites[2 * mid] < ret) lo = mid + 1; else hi = mid;
    }
    if (lo < nsites && sites[2 * lo] == ret) return sites[2 * lo + 1];
    fprintf(stderr, "usf: call site %08x not in the call-site table: popping no arguments\n", ret);
    return 0;
}

uint32_t obj_new_n(const char *iface, int nslots, const HleFn *impl, const uint8_t *nargs, uint32_t size) {
    uint32_t vt = gcalloc(4u * (uint32_t)nslots);
    for (int i = 0; i < nslots; i++) {
        char label[64];
        snprintf(label, sizeof label, "%s::%d", iface, i);
        W32(vt + 4u * (uint32_t)i, usf_thunk_fn(label, impl ? impl[i] : NULL, nargs ? nargs[i] : 0,
                                                nargs ? CONV_STDCALL : CONV_CALLSITE));
    }
    uint32_t o = gcalloc(size < 16 ? 16 : size);
    W32(o, vt);
    return o;
}

uint32_t obj_new(const char *iface, int nslots, const HleFn *impl, uint32_t size) {
    return obj_new_n(iface, nslots, impl, NULL, size);
}
