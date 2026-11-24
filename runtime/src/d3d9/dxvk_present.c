/* The DXVK bridge's Present side (see dxvk_bridge.h): the frame count, the statistics line every 2 s (fps, draws, CPU
 * per frame, frame-time p99 and worst, bytes copied by locks), USF_FTLOG / USF_STALLS analysis, and USF_SHOT /
 * USF_SHOT_MS captures of the back buffer (PPM in USF_SHOT_DIR).
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "d3d9.h"
#include "dxvk_bridge.h"

extern uint64_t copied_buf, copied_tex_in, copied_tex_out;   /* dxvk_bridge.c */
extern __thread float call_ms[DXB_NTYPES][120];
extern int usf_stalls;
extern __thread double usf_block_ms[3];

/* ---- Present: frame count, statistics (the same line as the own renderer), captures ---- */
uint32_t usf_ms(void);
uint32_t usf_big_discards, usf_discards;               /* USF_STALLS: DISCARD locks (of buffers of 4 MB or more) */
extern uint64_t checked_bytes, checked_locks;           /* dxvk_bridge.c */
static void shot_if_wanted(void *dev, void *chain);
static int cmp_float(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }
static double ft_p99(float *v, uint32_t n) {             /* 99th percentile frame time (sorts v) */
    if (!n) return 0;
    qsort(v, n, sizeof *v, cmp_float);
    return v[n * 99 / 100 < n ? n * 99 / 100 : n - 1];
}
static uint32_t ft_late(const float *v, uint32_t n) {    /* frames 25% over the target (18 ms at 60 fps) */
    int fps = getenv("USF_FPS") ? atoi(getenv("USF_FPS")) : 60;
    double lim = fps > 0 && !getenv("USF_NOVSYNC") ? 1250.0 / fps : 20.8;
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) k += v[i] > lim;
    return k;
}
void dxb_after_present(void *dev, void *chain);
void dxb_after_present(void *dev, void *chain) {   /* chain: an additional swap chain presented, or NULL */
    g_dev.frames++;
#ifdef __ANDROID__
    if (g_dev.frames == 300) { void gpu_driver_good(void); gpu_driver_good(); }   /* the driver's trial is over */
#endif
    static struct timespec t0, c0;
    static uint32_t f0, d0;
    static double worst;
    static struct timespec last;
    static float fts[4096];                              /* frame times of the current 2 s window */
    static uint32_t nft;
    static FILE *ftlog;                                  /* USF_FTLOG: every frame time, one per line (ms) */
    struct timespec t, ct;
    clock_gettime(CLOCK_MONOTONIC, &t);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ct);
    if (last.tv_sec) {
        double ft = (double)(t.tv_sec - last.tv_sec) * 1e3 + (double)(t.tv_nsec - last.tv_nsec) / 1e6;
        if (ft > worst) worst = ft;
        if (nft < 4096) fts[nft++] = (float)ft;
        if (!ftlog && getenv("USF_FTLOG")) ftlog = fopen(getenv("USF_FTLOG"), "w");
        if (ftlog) fprintf(ftlog, "%u %.3f\n", usf_ms(), ft);
        static struct timespec lc;
        if (usf_stalls < 0) usf_stalls = getenv("USF_STALLS") != NULL;
        if (usf_stalls && ft > 20.8) {
            double cpu = (double)(ct.tv_sec - lc.tv_sec) * 1e3 + (double)(ct.tv_nsec - lc.tv_nsec) / 1e6;
            char top[256] = "";
            for (int k = 0; k < 3; k++) {
                int bt = 0, bs = 0;
                for (int ty = 0; ty < DXB_NTYPES; ty++)
                    for (int sl = 0; sl < 120; sl++) if (call_ms[ty][sl] > call_ms[bt][bs]) { bt = ty; bs = sl; }
                if (call_ms[bt][bs] < 0.5f) break;
                snprintf(top + strlen(top), sizeof top - strlen(top), " t%d.%d=%.1f", bt, bs, call_ms[bt][bs]);
                call_ms[bt][bs] = -call_ms[bt][bs];
            }
            fprintf(stderr, "usf: late frame at %u ms: %.1f ms, cpu %.1f, wait %.1f, sleep %.1f, cs %.1f, dxvk%s\n",
                    usf_ms(), ft, cpu, usf_block_ms[0], usf_block_ms[1], usf_block_ms[2], top);
        }
        lc = ct;
    }
    if (usf_stalls > 0) { memset(call_ms, 0, sizeof call_ms); memset(usf_block_ms, 0, sizeof usf_block_ms); }
    last = t;
    if (!t0.tv_sec) { t0 = t; c0 = ct; f0 = g_dev.frames; d0 = g_dev.draws; }
    double el = (double)(t.tv_sec - t0.tv_sec) * 1e3 + (double)(t.tv_nsec - t0.tv_nsec) / 1e6;
    if (el >= 2000.0) {
        uint32_t nf = g_dev.frames - f0 ? g_dev.frames - f0 : 1;
        double cpu = (double)(ct.tv_sec - c0.tv_sec) * 1e3 + (double)(ct.tv_nsec - c0.tv_nsec) / 1e6;
        fprintf(stderr, "usf: %.1f fps, %u draws/frame, ring 0 KB | cpu %.1f ms/f, draws 0.00 ms/f, gpu wait 0.0 ms/f, "
                "worst frame %.1f ms, p99 %.1f ms, late %u | dxvk, copies/frame: buffers %llu KB, textures in %llu KB out %llu KB\n",
                nf * 1000.0 / el, (g_dev.draws - d0) / nf, cpu / nf, worst, ft_p99(fts, nft), ft_late(fts, nft), (unsigned long long)(copied_buf / nf >> 10),
                (unsigned long long)(copied_tex_in / nf >> 10), (unsigned long long)(copied_tex_out / nf >> 10));
        if (usf_stalls > 0) {
            extern uint64_t usf_mem_bytes[3], usf_mem_calls[3];
            fprintf(stderr, "usf: discards/frame %.1f, of big buffers %.1f; game memcpy %.0f KB in %.0f calls, memmove %.0f KB "
                    "in %.0f, memset %.0f KB in %.0f (per frame, all threads)\n", usf_discards / (double)nf,
                    usf_big_discards / (double)nf, usf_mem_bytes[0] / 1024.0 / nf, usf_mem_calls[0] / (double)nf,
                    usf_mem_bytes[1] / 1024.0 / nf, usf_mem_calls[1] / (double)nf, usf_mem_bytes[2] / 1024.0 / nf,
                    usf_mem_calls[2] / (double)nf);
            fprintf(stderr, "usf: checked locks/frame %.1f, marked %.0f KB/frame\n", checked_locks / (double)nf, checked_bytes / 1024.0 / nf);
            checked_bytes = checked_locks = 0;
            memset(usf_mem_bytes, 0, sizeof usf_mem_bytes); memset(usf_mem_calls, 0, sizeof usf_mem_calls);
        }
        usf_discards = usf_big_discards = 0;
        copied_buf = copied_tex_in = copied_tex_out = 0;
        t0 = t; c0 = ct; f0 = g_dev.frames; d0 = g_dev.draws; worst = 0; nft = 0;
    }
    shot_if_wanted(dev, chain);
    void usf_perf_frame(void);
    usf_perf_frame();
}

/* USF_SHOT / USF_SHOT_MS / USF_SHOT_DIR as with the own renderer: the back buffer as PPM */
static int want_shot(void) {
    const char *run = getenv("USF_SHOT_RUN");             /* "<ms>:<frames>,...": every frame of each run from <ms> on */
    if (run) {
        static uint32_t start[8], k;
        uint32_t now = usf_ms(), i = 0;
        for (const char *p = run; *p && i < 8; i++) {
            uint32_t ms = (uint32_t)strtoul(p, NULL, 10), n = 0;
            const char *c = strchr(p, ':');
            if (c) n = (uint32_t)strtoul(c + 1, NULL, 10);
            if (now >= ms && !start[i]) start[i] = g_dev.frames ? g_dev.frames : 1;
            if (start[i] && g_dev.frames - start[i] < n) { k++; return 1; }
            p = strchr(p, ',');
            if (!p) break;
            p++;
        }
        return 0;
    }
    const char *lm = getenv("USF_SHOT_MS");
    if (lm) {
        static uint32_t done;
        uint32_t now = usf_ms(), k = 0;
        for (const char *p = lm; *p; k++) {
            if (k == done && now >= (uint32_t)strtoul(p, NULL, 10)) { done++; return 1; }
            p = strchr(p, ',');
            if (!p) break;
            p++;
        }
        return 0;
    }
    const char *l = getenv("USF_SHOT");
    for (const char *p = l; p && *p;) {
        if ((uint32_t)strtoul(p, NULL, 10) == g_dev.frames) return 1;
        p = strchr(p, ',');
        if (p) p++;
    }
    return 0;
}
static void shot_if_wanted(void *dev, void *chain) {
    if (!want_shot()) return;
    void *bb = NULL, *sys = NULL;
    /* the back buffer of what was presented: the device's implicit swap chain, or the additional one */
    if (chain ? (int32_t)dxb_method(chain, 5)(chain, 0, 0, (uint64_t)(uintptr_t)&bb, 0, 0, 0, 0, 0, 0, 0, 0) < 0
              : (int32_t)dxb_method(dev, 18)(dev, 0, 0, 0, (uint64_t)(uintptr_t)&bb, 0, 0, 0, 0, 0, 0, 0) < 0 || !bb)
        return;
    if (!bb) return;
    uint32_t desc[8];
    dxb_method(bb, 12)(bb, (uint64_t)(uintptr_t)desc, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    uint32_t w = desc[6], h = desc[7];
    if ((int32_t)dxb_method(dev, 36)(dev, w, h, desc[0], 2, (uint64_t)(uintptr_t)&sys, 0, 0, 0, 0, 0, 0) >= 0 && sys &&
        (int32_t)dxb_method(dev, 32)(dev, (uint64_t)(uintptr_t)bb, (uint64_t)(uintptr_t)sys, 0, 0, 0, 0, 0, 0, 0, 0, 0) >= 0) {
        struct { int32_t pitch; int32_t pad; uint8_t *bits; } lr;
        if ((int32_t)dxb_method(sys, 13)(sys, (uint64_t)(uintptr_t)&lr, 0, 0x10, 0, 0, 0, 0, 0, 0, 0, 0) >= 0) {
            char fn[1200];
            const char *dir = getenv("USF_SHOT_DIR");
            snprintf(fn, sizeof fn, "%s/frame_%05u.ppm", dir ? dir : ".", g_dev.frames);
            FILE *f = fopen(fn, "wb");
            if (f) {
                fprintf(f, "P6\n%u %u\n255\n", w, h);
                uint8_t *row = malloc(3u * w);
                for (uint32_t y = 0; y < h; y++) {
                    const uint8_t *s = lr.bits + (size_t)y * (uint32_t)lr.pitch;
                    for (uint32_t x = 0; x < w; x++) { row[3 * x] = s[4 * x + 2]; row[3 * x + 1] = s[4 * x + 1]; row[3 * x + 2] = s[4 * x]; }
                    fwrite(row, 3, w, f);
                }
                free(row);
                fclose(f);
            }
            dxb_method(sys, 14)(sys, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        }
    }
    if (sys) dxb_method(sys, 2)(sys, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    dxb_method(bb, 2)(bb, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

