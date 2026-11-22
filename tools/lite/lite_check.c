/* Checks bundle_PC.lite against the archive it was made from: every byte of the regions kept exactly, and for the
 * sounds the header, the length and the signal-to-noise ratio of the decoded samples (worst and average).
 * usage: lite_check <bundle_PC.ipk> <bundle_PC.lite>   (built by hand: see docs/PROGRESS.md) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lite.h"
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    FILE *f = fopen(argv[1], "rb");
    Lite *l = lite_open(argv[2]);
    if (!f || !l) { fprintf(stderr, "cannot open\n"); return 1; }
    FILE *t = fopen(argv[2], "rb");
    uint8_t h[32]; fread(h, 1, 32, t);
    uint32_t n; uint64_t tab; memcpy(&n, h + 20, 4); memcpy(&tab, h + 24, 8);
    LiteRegion *r = malloc(n * sizeof *r); fseeko(t, (off_t)tab, SEEK_SET); fread(r, sizeof *r, n, t);
    uint64_t bad = 0, sounds = 0; double worst = 1e9, sum = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *a = malloc(r[i].orig_len + 1), *b = malloc(r[i].orig_len + 1);
        fseeko(f, (off_t)r[i].orig_off, SEEK_SET); fread(a, 1, r[i].orig_len, f);
        if (lite_pread(l, b, r[i].orig_len, r[i].orig_off) != (int64_t)r[i].orig_len) { bad++; goto next; }
        if (r[i].method != LITE_OPUS) { if (memcmp(a, b, r[i].orig_len)) { bad++; fprintf(stderr, "region %u differs\n", i); } goto next; }
        {
            uint32_t pre = 0; memcpy(&pre, (uint8_t[4]){0}, 0);
            /* the data chunk: find it again */
            uint32_t p = 12; while (p + 8 <= r[i].orig_len && memcmp(a + p, "data", 4)) { uint32_t cl; memcpy(&cl, a + p + 4, 4); p += 8 + cl + (cl & 1); }
            uint32_t dl; memcpy(&dl, a + p + 4, 4); pre = p + 8;
            if (memcmp(a, b, pre) || memcmp(a + pre + dl, b + pre + dl, r[i].orig_len - pre - dl)) { bad++; fprintf(stderr, "sound %u: header differs\n", i); goto next; }
            int16_t *x = (int16_t *)(a + pre), *y = (int16_t *)(b + pre);
            double s = 0, e = 0;
            for (uint32_t k = 0; k < dl / 2; k++) { s += (double)x[k] * x[k]; e += (double)(x[k] - y[k]) * (x[k] - y[k]); }
            double snr = s > 0 ? 10 * log10(s / (e + 1)) : 99;
            if (s > 1e6) { if (snr < worst) worst = snr; sum += snr; sounds++; }
        }
    next:
        free(a); free(b);
        if (i % 1000 == 0) fprintf(stderr, "\r%u/%u", i, n);
    }
    printf("\nregions %u, wrong %llu; sounds %llu: SNR worst %.1f dB, average %.1f dB\n", n, (unsigned long long)bad,
           (unsigned long long)sounds, worst, sounds ? sum / sounds : 0);
    return bad != 0;
}
