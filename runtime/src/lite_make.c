/* Writes GameData/bundle_PC.lite from the player's bundle_PC.ipk (lite.h has the why and the layout).
 *
 * What it does: reads the archive's directory (version 3, big-endian, as ipk.c), cuts the archive into regions (each
 * file, and what lies between files), and stores each region the smallest way that gives its bytes back:
 *   - a RIFF WAVE file of 16-bit PCM at 48 kHz, mono or stereo: Opus (CELT, 96 kb/s stereo, 64 kb/s mono, 20 ms frames,
 *     VBR, the encoder's best effort): about 15 times smaller and not to be told apart by ear;
 *   - another file stored uncompressed: zlib (level 9) when that saves at least a tenth;
 *   - files the archive already compresses (the textures), and everything else: as they are;
 *   - zeros between files: nothing.
 * Regions are compressed by `threads` workers and written in order by the caller's thread (at most 256 ahead).
 * What it does not do: change anything in the original folder, or the movies (the launcher copies those).
 */
#include <errno.h>
#include <fcntl.h>
#include <opus.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#include "lite.h"

typedef struct { uint64_t off; uint32_t len; int file; } Cut;   /* file: a file of the archive (not a gap) */
typedef struct { uint8_t *data; uint32_t len, method; int done; } Out;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
static uint32_t le32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

typedef struct { uint8_t *p; size_t n, cap; } Buf;
static void put(Buf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 4096; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void put32(Buf *b, uint32_t v) { uint8_t t[4] = {v, v >> 8, v >> 16, v >> 24}; put(b, t, 4); }

/* a 16-bit 48 kHz PCM WAVE file as LITE_OPUS; 0 when it is not one */
static int opus_wave(const uint8_t *f, uint32_t len, Buf *out) {
    if (len < 44 || memcmp(f, "RIFF", 4) || memcmp(f + 8, "WAVE", 4)) return 0;
    uint32_t p = 12, ch = 0, data = 0, dlen = 0;
    int pcm = 0;
    while (p + 8 <= len) {
        uint32_t cl = le32(f + p + 4);
        if (!memcmp(f + p, "fmt ", 4) && cl >= 16 && p + 8 + 16 <= len) {
            pcm = le16(f + p + 8) == 1 && le32(f + p + 12) == 48000 && le16(f + p + 22) == 16;
            ch = le16(f + p + 10);
        } else if (!memcmp(f + p, "data", 4)) {
            data = p + 8;
            dlen = cl;
            break;
        }
        p += 8 + cl + (cl & 1);
    }
    if (!pcm || (ch != 1 && ch != 2) || !data || dlen > len - data || dlen % (2 * ch) || dlen < 2 * ch * 960) return 0;
    uint32_t samples = dlen / (2 * ch);
    int err = 0;
    OpusEncoder *e = opus_encoder_create(48000, (int)ch, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);   /* CELT only: its delay is exactly the lookahead (SILK's is not) */
    if (!e || err) return 0;
    opus_encoder_ctl(e, OPUS_SET_BITRATE(ch == 2 ? 96000 : 64000));
    opus_encoder_ctl(e, OPUS_SET_COMPLEXITY(10));
    opus_encoder_ctl(e, OPUS_SET_VBR(1));
    opus_int32 look = 0;
    opus_encoder_ctl(e, OPUS_GET_LOOKAHEAD(&look));
    uint32_t total = samples + (uint32_t)look, frames = (total + 959) / 960;
    int16_t *pcm16 = calloc((size_t)frames * 960 * ch, 2);           /* the samples, then zeros (the lookahead) */
    memcpy(pcm16, f + data, dlen);
    put32(out, data); put(out, f, data);                             /* everything before the samples */
    uint32_t after = len - data - dlen;
    put32(out, after); put(out, f + data + dlen, after);
    put32(out, ch); put32(out, samples); put32(out, (uint32_t)look); put32(out, frames);
    uint8_t pkt[4000];
    int ok = 1;
    for (uint32_t i = 0; i < frames; i++) {
        opus_int32 n = opus_encode(e, pcm16 + (size_t)i * 960 * ch, 960, pkt, sizeof pkt);
        if (n < 0) { ok = 0; break; }
        uint8_t l2[2] = {(uint8_t)n, (uint8_t)(n >> 8)};
        put(out, l2, 2); put(out, pkt, (size_t)n);
    }
    free(pcm16);
    opus_encoder_destroy(e);
    if (!ok) out->n = 0;
    return ok;
}

typedef struct {
    int fd;
    Cut *cuts;
    Out *outs;
    uint32_t n, next, written;
    pthread_mutex_t m;
    pthread_cond_t cv;
    int failed;
} Job;

static void work_one(Job *j, uint32_t i) {
    Cut *c = &j->cuts[i];
    Out o = {0};
    uint8_t *raw = malloc(c->len ? c->len : 1);
    if (pread(j->fd, raw, c->len, (off_t)c->off) != (ssize_t)c->len) { j->failed = 1; free(raw); return; }
    int zero = 1;
    for (uint32_t k = 0; k < c->len && zero; k++) zero = !raw[k];
    Buf b = {0};
    if (zero) o.method = LITE_ZERO;
    else if (c->file && opus_wave(raw, c->len, &b) && b.n) { o.method = LITE_OPUS; o.data = b.p; o.len = (uint32_t)b.n; b.p = NULL; }
    else {
        uLongf zl = compressBound(c->len);
        uint8_t *z = malloc(zl);
        if (c->len >= 256 && compress2(z, &zl, raw, c->len, 9) == Z_OK && zl < c->len - c->len / 10) {
            o.method = LITE_ZLIB; o.data = z; o.len = (uint32_t)zl; z = NULL;
        } else {
            o.method = LITE_RAW; o.data = raw; o.len = c->len; raw = NULL;
        }
        free(z);
    }
    free(b.p);
    free(raw);
    pthread_mutex_lock(&j->m);
    o.done = 1;
    j->outs[i] = o;
    pthread_cond_broadcast(&j->cv);
    pthread_mutex_unlock(&j->m);
}
static void *worker(void *arg) {
    Job *j = arg;
    for (;;) {
        pthread_mutex_lock(&j->m);
        while (j->next < j->n && j->next > j->written + 256 && !j->failed) pthread_cond_wait(&j->cv, &j->m);
        uint32_t i = j->next < j->n && !j->failed ? j->next++ : j->n;
        pthread_mutex_unlock(&j->m);
        if (i >= j->n) return NULL;
        work_one(j, i);
    }
}

static int by_off(const void *a, const void *b) {
    const Cut *x = a, *y = b;
    return x->off < y->off ? -1 : x->off > y->off;
}

const char *lite_make(const char *ipk_path, const char *out_path, int threads, void (*progress)(uint64_t, uint64_t)) {
    static char err[256];
    int fd = open(ipk_path, O_RDONLY);
    if (fd < 0) { snprintf(err, sizeof err, "cannot open %s: %s", ipk_path, strerror(errno)); return err; }
    uint8_t head[0x30];
    if (pread(fd, head, sizeof head, 0) != sizeof head || be32(head) != 0x50EC12BAu) { close(fd); return "not a game archive (bundle_PC.ipk)"; }
    uint32_t base = be32(head + 12), count = be32(head + 0x2C);
    uint64_t size = (uint64_t)lseek(fd, 0, SEEK_END);
    uint8_t *dir = malloc(base);
    if (pread(fd, dir, base, 0) != (ssize_t)base) { free(dir); close(fd); return "cannot read the archive's directory"; }
    Cut *files = calloc(count ? count : 1, sizeof *files);
    uint32_t nf = 0, p = 0x30;
    for (uint32_t i = 0; i < count && p + 24 <= base; i++) {
        uint32_t parts = be32(dir + p), sz = be32(dir + p + 4), zsz = be32(dir + p + 8);
        p += 20;
        uint64_t off = parts ? be64(dir + p) : 0;
        p += 8 * parts;
        uint32_t nl = be32(dir + p);
        p += 4 + 2 * nl;
        if (parts) files[nf++] = (Cut){base + off, zsz ? zsz : sz, 1};
    }
    qsort(files, nf, sizeof *files, by_off);
    Cut *cuts = calloc((size_t)nf * 2 + 2, sizeof *cuts);
    uint32_t n = 0;
    uint64_t at = base;
    for (uint32_t i = 0; i < nf; i++) {
        if (files[i].off < at) continue;                              /* (overlapping: already covered) */
        if (files[i].off > at) cuts[n++] = (Cut){at, (uint32_t)(files[i].off - at), 0};
        cuts[n++] = files[i];
        at = files[i].off + files[i].len;
    }
    while (at < size) { uint32_t l = size - at > (1u << 30) ? 1u << 30 : (uint32_t)(size - at); cuts[n++] = (Cut){at, l, 0}; at += l; }
    free(files);

    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.part", out_path);
    FILE *o = fopen(tmp, "wb");
    if (!o) { free(dir); free(cuts); close(fd); snprintf(err, sizeof err, "cannot write %s: %s", tmp, strerror(errno)); return err; }
    uint8_t h[32] = {0};
    memcpy(h, LITE_MAGIC, 8);
    memcpy(h + 8, &size, 8); memcpy(h + 16, &base, 4); memcpy(h + 20, &n, 4);
    fwrite(h, 1, 32, o);
    fwrite(dir, 1, base, o);
    free(dir);

    Job j = {fd, cuts, calloc(n ? n : 1, sizeof(Out)), n, 0, 0, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0};
    if (threads < 1) threads = 1;
    if (threads > 16) threads = 16;
    pthread_t th[16];
    for (int i = 0; i < threads; i++) pthread_create(&th[i], NULL, worker, &j);
    LiteRegion *tab = calloc(n ? n : 1, sizeof *tab);
    uint64_t pos = 32 + (uint64_t)base, done = base;
    for (uint32_t i = 0; i < n && !j.failed; i++) {
        pthread_mutex_lock(&j.m);
        while (!j.outs[i].done && !j.failed) pthread_cond_wait(&j.cv, &j.m);
        Out out = j.outs[i];
        pthread_mutex_unlock(&j.m);
        if (j.failed) break;
        tab[i] = (LiteRegion){cuts[i].off, cuts[i].len, out.method, pos, out.method == LITE_ZERO ? 0 : out.len, 0};
        if (tab[i].pack_len && fwrite(out.data, 1, tab[i].pack_len, o) != tab[i].pack_len) j.failed = 2;
        pos += tab[i].pack_len;
        free(out.data);
        j.outs[i].data = NULL;
        done += cuts[i].len;
        pthread_mutex_lock(&j.m);
        j.written = i;
        pthread_cond_broadcast(&j.cv);
        pthread_mutex_unlock(&j.m);
        if (progress) progress(done, size);
    }
    pthread_mutex_lock(&j.m); j.next = n; pthread_cond_broadcast(&j.cv); pthread_mutex_unlock(&j.m);
    for (int i = 0; i < threads; i++) pthread_join(th[i], NULL);
    close(fd);
    const char *r = NULL;
    if (j.failed) r = j.failed == 2 ? "the device is full" : "cannot read the archive";
    else {
        fwrite(tab, sizeof *tab, n, o);
        fseek(o, 24, SEEK_SET);
        fwrite(&pos, 8, 1, o);
        if (fclose(o) || rename(tmp, out_path)) r = "cannot finish writing the compressed archive";
        o = NULL;
    }
    if (o) fclose(o);
    if (r) unlink(tmp);
    for (uint32_t i = 0; i < n; i++) free(j.outs[i].data);
    free(j.outs); free(tab); free(cuts);
    return r;
}

#ifdef LITE_MAKE_MAIN
/* the PC tool: lite_make <bundle_PC.ipk> <bundle_PC.lite> [threads] */
static void show(uint64_t d, uint64_t t) {
    static int last = -1;
    int pc = (int)(d * 100 / (t ? t : 1));
    if (pc != last) { fprintf(stderr, "\r%3d%%", pc); last = pc; }
}
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <bundle_PC.ipk> <bundle_PC.lite> [threads]\n", argv[0]); return 2; }
    const char *e = lite_make(argv[1], argv[2], argc > 3 ? atoi(argv[3]) : 4, show);
    fprintf(stderr, "\n%s\n", e ? e : "done");
    return e != NULL;
}
#endif
