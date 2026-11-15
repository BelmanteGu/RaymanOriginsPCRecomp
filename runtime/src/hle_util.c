/* Small pieces every HLE file uses: the handle table and UTF-16 <-> UTF-8 guest strings.
 *
 * What it does: handle_new/get/close map Windows handles (multiples of 4 from 0x100) to host objects with a kind
 * tag, so a handle of the wrong kind is refused instead of misused. wlen/w2utf8/utf8_to_w convert the game's
 * UTF-16 strings (surrogate pairs included).
 * What it does not do: close the host object behind a handle (the owner of the kind does, before handle_close).
 */
#include <pthread.h>
#include <string.h>

#include "hle.h"

#define MAXH 8192
static struct { int kind; void *obj; } handles[MAXH];
static pthread_mutex_t hlock = PTHREAD_MUTEX_INITIALIZER;

uint32_t handle_new(int kind, void *obj) {
    pthread_mutex_lock(&hlock);
    for (uint32_t i = 1; i < MAXH; i++)
        if (!handles[i].kind) {
            handles[i].kind = kind;
            handles[i].obj = obj;
            pthread_mutex_unlock(&hlock);
            return 0x100u + 4u * i;
        }
    pthread_mutex_unlock(&hlock);
    hle_fatal("out of handles");
}

void *handle_get(uint32_t h, int kind) {
    if (h < 0x104u || (h & 3u)) return NULL;
    uint32_t i = (h - 0x100u) / 4u;
    if (i >= MAXH || (kind && handles[i].kind != kind)) return NULL;
    return handles[i].obj;
}

int handle_kind(uint32_t h);
int handle_kind(uint32_t h) {
    if (h < 0x104u || (h & 3u)) return 0;
    uint32_t i = (h - 0x100u) / 4u;
    return i < MAXH ? handles[i].kind : 0;
}

void handle_close(uint32_t h) {
    if (h < 0x104u || (h & 3u)) return;
    uint32_t i = (h - 0x100u) / 4u;
    if (i < MAXH) { handles[i].kind = 0; handles[i].obj = NULL; }
}

size_t wlen(uint32_t p) {
    size_t n = 0;
    while (R16(p + 2u * n)) n++;
    return n;
}

void w2utf8(uint32_t p, char *out, size_t n) {
    size_t o = 0;
    for (uint32_t i = 0;; i++) {
        uint32_t ch = R16(p + 2u * i);
        if (!ch) break;
        if (ch >= 0xD800 && ch < 0xDC00) {
            uint32_t lo = R16(p + 2u * (i + 1));
            if (lo >= 0xDC00 && lo < 0xE000) { ch = 0x10000u + ((ch - 0xD800u) << 10) + (lo - 0xDC00u); i++; }
        }
        char tmp[4];
        size_t k;
        if (ch < 0x80) { tmp[0] = (char)ch; k = 1; }
        else if (ch < 0x800) { tmp[0] = (char)(0xC0 | ch >> 6); tmp[1] = (char)(0x80 | (ch & 63)); k = 2; }
        else if (ch < 0x10000) {
            tmp[0] = (char)(0xE0 | ch >> 12); tmp[1] = (char)(0x80 | ((ch >> 6) & 63)); tmp[2] = (char)(0x80 | (ch & 63)); k = 3;
        } else {
            tmp[0] = (char)(0xF0 | ch >> 18); tmp[1] = (char)(0x80 | ((ch >> 12) & 63));
            tmp[2] = (char)(0x80 | ((ch >> 6) & 63)); tmp[3] = (char)(0x80 | (ch & 63)); k = 4;
        }
        if (o + k + 1 > n) break;
        memcpy(out + o, tmp, k);
        o += k;
    }
    if (n) out[o] = 0;
}

void utf8_to_w(const char *s, uint32_t dst, size_t nchars) {
    size_t o = 0;
    const unsigned char *u = (const unsigned char *)s;
    while (*u && o + 1 < nchars) {
        uint32_t ch;
        if (*u < 0x80) ch = *u++;
        else if ((*u & 0xE0) == 0xC0) { ch = (*u & 31u) << 6 | (u[1] & 63u); u += 2; }
        else if ((*u & 0xF0) == 0xE0) { ch = (*u & 15u) << 12 | (u[1] & 63u) << 6 | (u[2] & 63u); u += 3; }
        else { ch = (*u & 7u) << 18 | (u[1] & 63u) << 12 | (u[2] & 63u) << 6 | (u[3] & 63u); u += 4; }
        if (ch >= 0x10000 && o + 2 < nchars) {
            ch -= 0x10000;
            W16(dst + 2u * o++, 0xD800u + (ch >> 10));
            W16(dst + 2u * o++, 0xDC00u + (ch & 0x3FFu));
        } else {
            W16(dst + 2u * o++, ch);
        }
    }
    if (nchars) W16(dst + 2u * o, 0);
}
