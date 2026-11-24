/* The original archive's bytes from bundle_PC.lite (lite.h has the why and the layout).
 *
 * What it does: lite_pread(off, n) finds the regions that range covers (binary search in the table) and copies their
 * bytes: as stored (RAW), zeros (ZERO), or from a region made back in memory and kept in a cache: ZLIB inflated at
 * once; OPUS decoded progressively, only as far as the game has asked plus half a second (the game streams its music
 * from the start, and a whole 2-minute track at once would hold a loading thread for a moment on a phone): its header
 * and trailing chunks are there at once, its samples come exactly as many as the original's. The cache keeps up to
 * 96 MB of made-back regions, the least recently used going first; one lock serves the game's loading threads.
 * USF_LITE_STATS=1 prints the time spent making regions back.
 * What it does not do: write.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <opus.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "lite.h"

#define CACHE_BYTES (96u << 20)
#define CACHE_N 64
#define AHEAD (48000u * 4 / 2)                      /* decoded beyond what is asked: half a second of stereo */

typedef struct {
    uint32_t region;
    uint8_t *data;                                  /* orig_len bytes; valid: [0, ready) and the suffix */
    uint64_t used;
    uint32_t ready;
    /* a sound still being decoded */
    OpusDecoder *dec;
    uint8_t *src;
    uint32_t src_len, src_pos, ch, pre, end, skip;  /* end: where the samples end (pre + samples * 2 * ch) */
} Slot;
struct Lite {
    int fd;
    uint64_t size;
    uint32_t base, n;
    uint8_t *dir;
    LiteRegion *tab;
    Slot cache[CACHE_N];
    uint64_t clock, cached;
    pthread_mutex_t m;
};

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static double t_made[4], b_made[4];

Lite *lite_open(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    uint8_t h[32];
    uint64_t tab_off;
    Lite *l = calloc(1, sizeof *l);
    if (pread(fd, h, 32, 0) != 32 || memcmp(h, LITE_MAGIC, 8)) goto bad;
    memcpy(&l->size, h + 8, 8); memcpy(&l->base, h + 16, 4); memcpy(&l->n, h + 20, 4); memcpy(&tab_off, h + 24, 8);
    l->dir = malloc(l->base);
    l->tab = malloc((size_t)l->n * sizeof *l->tab);
    if (pread(fd, l->dir, l->base, 32) != (ssize_t)l->base ||
        pread(fd, l->tab, (size_t)l->n * sizeof *l->tab, (off_t)tab_off) != (ssize_t)((size_t)l->n * sizeof *l->tab)) goto bad;
    l->fd = fd;
    pthread_mutex_init(&l->m, NULL);
    for (int i = 0; i < CACHE_N; i++) l->cache[i].region = UINT32_MAX;
    return l;
bad:
    close(fd);
    free(l->dir); free(l->tab); free(l);
    return NULL;
}
uint64_t lite_size(Lite *l) { return l->size; }

static void drop(Slot *s) {
    free(s->data); free(s->src);
    if (s->dec) opus_decoder_destroy(s->dec);
    memset(s, 0, sizeof *s);
    s->region = UINT32_MAX;
}
void lite_close(Lite *l) {
    if (!l) return;
    close(l->fd);
    for (int i = 0; i < CACHE_N; i++) drop(&l->cache[i]);
    free(l->dir); free(l->tab); free(l);
}

/* a LITE_OPUS region: its header and trailing chunks now, its samples later (decode_to); 0 if it is damaged */
static int opus_start(Slot *s, uint8_t *src, uint32_t sl, uint32_t ol) {
    uint32_t pre = rd32(src);
    if (pre > ol || 4 + (uint64_t)pre + 4 > sl) return 0;
    memcpy(s->data, src + 4, pre);
    uint32_t p = 4 + pre, post = rd32(src + p);
    if (p + 4 + (uint64_t)post + 16 > sl || post > ol - pre) return 0;
    memcpy(s->data + ol - post, src + p + 4, post);
    p += 4 + post;
    uint32_t ch = rd32(src + p), samples = rd32(src + p + 4), look = rd32(src + p + 8);
    if ((ch != 1 && ch != 2) || pre + (uint64_t)samples * 2 * ch + post != ol) return 0;
    int err = 0;
    s->dec = opus_decoder_create(48000, (int)ch, &err);
    if (!s->dec || err) return 0;
    s->src = src; s->src_len = sl; s->src_pos = p + 16;
    s->ch = ch; s->pre = pre; s->end = pre + samples * 2 * ch; s->skip = look;
    s->ready = pre;
    return 1;
}
/* decodes until [0, upto) is valid (or the samples end); the rest after them is silence if the packets run out */
static void decode_to(Slot *s, uint32_t upto) {
    int16_t frame[960 * 2 * 3];
    while (s->dec && s->ready < upto && s->ready < s->end) {
        if (s->src_pos + 2 > s->src_len) break;
        uint32_t n = s->src[s->src_pos] | (uint32_t)s->src[s->src_pos + 1] << 8;
        s->src_pos += 2;
        if (s->src_pos + n > s->src_len) break;
        int got = opus_decode(s->dec, s->src + s->src_pos, (opus_int32)n, frame, 960 * 3, 0);
        s->src_pos += n;
        if (got <= 0) continue;
        uint32_t from = s->skip < (uint32_t)got ? s->skip : (uint32_t)got;
        s->skip -= from;
        uint32_t bytes = ((uint32_t)got - from) * 2 * s->ch;
        if (bytes > s->end - s->ready) bytes = s->end - s->ready;
        memcpy(s->data + s->ready, frame + from * s->ch, bytes);
        s->ready += bytes;
    }
    if (s->ready >= s->end || s->src_pos + 2 > s->src_len) {      /* finished (or damaged: silence) */
        if (s->ready < s->end) { memset(s->data + s->ready, 0, s->end - s->ready); s->ready = s->end; }
        opus_decoder_destroy(s->dec); s->dec = NULL;
        free(s->src); s->src = NULL;
        s->ready = UINT32_MAX;
    }
}

/* region i's bytes, valid at least up to `upto` (from the start), from the cache or made now; the lock is held */
static const uint8_t *made(Lite *l, uint32_t i, uint32_t upto) {
    Slot *s = NULL;
    for (int k = 0; k < CACHE_N && !s; k++) if (l->cache[k].region == i) s = &l->cache[k];
    LiteRegion *r = &l->tab[i];
    double t0 = now_ms();
    if (!s) {
        for (;;) {                                                 /* room: drop the least recently used */
            int free_slot = -1, old = -1;
            for (int k = 0; k < CACHE_N; k++) {
                if (l->cache[k].region == UINT32_MAX) { if (free_slot < 0) free_slot = k; }
                else if (old < 0 || l->cache[k].used < l->cache[old].used) old = k;
            }
            if (free_slot >= 0 && (l->cached + r->orig_len <= CACHE_BYTES || old < 0)) { s = &l->cache[free_slot]; break; }
            l->cached -= l->tab[l->cache[old].region].orig_len;
            drop(&l->cache[old]);
        }
        s->region = i;
        s->data = malloc(r->orig_len ? r->orig_len : 1);
        l->cached += r->orig_len;
        uint8_t *src = malloc(r->pack_len ? r->pack_len : 1);
        int ok = pread(l->fd, src, r->pack_len, (off_t)r->pack_off) == (ssize_t)r->pack_len;
        if (ok && r->method == LITE_ZLIB) {
            uLongf ol = r->orig_len;
            ok = uncompress(s->data, &ol, src, r->pack_len) == Z_OK && ol == r->orig_len;
            free(src);
            s->ready = UINT32_MAX;
        } else if (ok && opus_start(s, src, r->pack_len, r->orig_len)) {
            src = NULL;                                            /* (kept by the slot until the sound is decoded) */
        } else {
            ok = 0;
            free(src);
        }
        if (!ok) {
            fprintf(stderr, "usf: bundle_PC.lite: region %u (offset %llu) could not be read back\n", i, (unsigned long long)r->orig_off);
            memset(s->data, 0, r->orig_len);
            s->ready = UINT32_MAX;
        }
    }
    s->used = ++l->clock;
    if (s->ready != UINT32_MAX && s->ready < upto) decode_to(s, upto + AHEAD > upto ? upto + AHEAD : UINT32_MAX - 1);
    t_made[r->method & 3] += now_ms() - t0;
    static int stats = -1;
    if (stats < 0) stats = getenv("USF_LITE_STATS") != NULL;
    if (stats) {
        b_made[r->method & 3] += 1;
        if ((int)(b_made[1] + b_made[2]) % 200 == 0)
            fprintf(stderr, "usf: lite: zlib %.0f calls %.0f ms, opus %.0f calls %.0f ms\n", b_made[1], t_made[1], b_made[2], t_made[2]);
    }
    return s->data;
}

int64_t lite_pread(Lite *l, void *buf, uint64_t n, uint64_t off) {
    if (off >= l->size) return 0;
    if (n > l->size - off) n = l->size - off;
    uint8_t *o = buf;
    uint64_t left = n;
    if (off < l->base) {                                          /* the directory */
        uint64_t k = l->base - off < left ? l->base - off : left;
        memcpy(o, l->dir + off, k);
        o += k; off += k; left -= k;
    }
    pthread_mutex_lock(&l->m);
    uint32_t lo = 0, hi = l->n;                                   /* the region holding off */
    while (hi - lo > 1) { uint32_t mid = (lo + hi) / 2; if (l->tab[mid].orig_off <= off) lo = mid; else hi = mid; }
    for (uint32_t i = lo; left && i < l->n; i++) {
        LiteRegion *r = &l->tab[i];
        if (off < r->orig_off) break;
        uint64_t in = off - r->orig_off;
        if (in >= r->orig_len) continue;
        uint64_t k = r->orig_len - in < left ? r->orig_len - in : left;
        if (r->method == LITE_ZERO) memset(o, 0, k);
        else if (r->method == LITE_RAW) {
            if (pread(l->fd, o, k, (off_t)(r->pack_off + in)) != (ssize_t)k) { pthread_mutex_unlock(&l->m); return -1; }
        } else memcpy(o, made(l, i, (uint32_t)(in + k)) + in, k);
        o += k; off += k; left -= k;
    }
    pthread_mutex_unlock(&l->m);
    return (int64_t)(n - left);
}

/* For the file layer (hle_k32_file.c, ipk.c): a host path to the game's archive that is not there, but whose folder
 * has bundle_PC.lite (any case): that file's path in out, 1. */
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
int lite_path_for(const char *path, char *out, size_t n) {
    struct stat st;
    const char *sl = strrchr(path, '/');
    if (!stat(path, &st) || strcasecmp(sl ? sl + 1 : path, "bundle_pc.ipk")) return 0;
    char dir[1024];
    snprintf(dir, sizeof dir, "%.*s", sl ? (int)(sl - path) : 1, sl ? path : ".");
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int ok = 0;
    while ((e = readdir(d)))
        if (!strcasecmp(e->d_name, "bundle_pc.lite")) { snprintf(out, n, "%s/%s", dir, e->d_name); ok = 1; break; }
    closedir(d);
    return ok;
}
/* one Lite per path for the whole process (every handle on the archive shares it: it is thread-safe) */
Lite *lite_shared(const char *path) {
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    static struct { char path[1024]; Lite *l; } open_[4];
    pthread_mutex_lock(&m);
    Lite *r = NULL;
    for (int i = 0; i < 4 && !r; i++) if (open_[i].l && !strcmp(open_[i].path, path)) r = open_[i].l;
    for (int i = 0; i < 4 && !r; i++)
        if (!open_[i].l && (r = open_[i].l = lite_open(path))) {
            snprintf(open_[i].path, sizeof open_[i].path, "%s", path);
            fprintf(stderr, "usf: the game's archive is the compressed copy %s\n", path);
        }
    pthread_mutex_unlock(&m);
    return r;
}
