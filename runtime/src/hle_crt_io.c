/* msvcr90.dll: stdio streams, file status and directory search, environment, time.
 *
 * What it does: a guest FILE is a 32-byte block in a table (the first three are stdin, stdout, stderr, as __iob_func
 * returns them); each one stands for a host FILE*. Paths go through the same Windows -> host translation as
 * CreateFile (the game folder is read in place; writes into it go to our copy). _stat / _findfirst fill the 32-bit
 * Windows structures. Time is the host's, with the local time zone.
 * What it does not do: text-mode CR/LF translation (files are opened binary; the game reads its data binary),
 * or wide-character streams.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "hle_crt.h"

#define D(name) uint32_t x_##name(CPU *c, uint32_t a)
#define NFILES 256

static uint32_t iob;                                           /* guest FILE table, 32 bytes each */
static FILE *hfile[NFILES];
static pthread_mutex_t io_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t crt_iob(void) {
    pthread_mutex_lock(&io_lock);
    if (!iob) {
        iob = gcalloc(32 * NFILES);
        hfile[0] = stdin; hfile[1] = stderr; hfile[2] = stderr;   /* the game's stdout goes to our log too */
    }
    pthread_mutex_unlock(&io_lock);
    return iob;
}
static FILE *hf(uint32_t g) {
    uint32_t base = crt_iob();
    if (g < base || g >= base + 32 * NFILES || (g - base) % 32) return NULL;
    return hfile[(g - base) / 32];
}

uint32_t crt_errno_addr(CPU *c) { return c->fs_base + 0x1040u; }   /* a free dword of the TEB (TLS slots end at 0xF10) */
void crt_set_errno(CPU *c, int e) {
    int w = e == ENOENT ? 2 : e == EACCES ? 13 : e == EEXIST ? 17 : e == EINVAL ? 22 : e == ERANGE ? 34 : e == ENOMEM ? 12 : e;
    W32(crt_errno_addr(c), (uint32_t)w);
}
D(_errno) { (void)a; return crt_errno_addr(c); }
D(__iob_func) { (void)c; (void)a; return crt_iob(); }

/* ---- streams ---- */
static uint32_t open_stream(CPU *c, const char *win, const char *mode) {
    char path[1200], m[8] = {0};
    int write = strpbrk(mode, "wa+") != NULL;
    win_path_host(win, write, path, sizeof path);
    for (int i = 0, k = 0; mode[i] && k < 6; i++)
        if (strchr("rwa+", mode[i])) m[k++] = mode[i];
    strcat(m, "b");
    FILE *f = fopen(path, m);
    usf_log("fopen(%s, %s) -> %s%s\n", win, mode, path, f ? "" : " (failed)");
    if (!f) { crt_set_errno(c, errno); return 0; }
    uint32_t base = crt_iob();
    pthread_mutex_lock(&io_lock);
    for (int i = 3; i < NFILES; i++)
        if (!hfile[i]) { hfile[i] = f; pthread_mutex_unlock(&io_lock); memset(HOST(base + 32u * i), 0, 32); return base + 32u * i; }
    pthread_mutex_unlock(&io_lock);
    fclose(f);
    crt_set_errno(c, EMFILE);
    return 0;
}
D(fopen) { return open_stream(c, STR(ARG(0)), STR(ARG(1))); }
D(fclose) {
    (void)c;
    FILE *f = hf(ARG(0));
    if (!f) return 0xFFFFFFFFu;
    uint32_t i = (ARG(0) - crt_iob()) / 32;
    if (i >= 3) { fclose(f); hfile[i] = NULL; }
    return 0;
}
D(fread) {                                                     /* (buffer, size, count, stream) */
    (void)c;
    FILE *f = hf(ARG(3));
    if (!f || !ARG(1)) return 0;
    return (uint32_t)fread(HOST(ARG(0)), ARG(1), ARG(2), f);
}
D(fwrite) {
    (void)c;
    FILE *f = hf(ARG(3));
    if (!f || !ARG(1)) return 0;
    return (uint32_t)fwrite(HOST(ARG(0)), ARG(1), ARG(2), f);
}
D(fseek) { (void)c; FILE *f = hf(ARG(0)); return f ? (uint32_t)fseeko(f, (off_t)(int32_t)ARG(1), (int)ARG(2)) : 0xFFFFFFFFu; }
D(ftell) { (void)c; FILE *f = hf(ARG(0)); return f ? (uint32_t)ftello(f) : 0xFFFFFFFFu; }
D(feof) { (void)c; FILE *f = hf(ARG(0)); return f ? (uint32_t)(feof(f) != 0) : 0; }
D(ferror) { (void)c; FILE *f = hf(ARG(0)); return f ? (uint32_t)(ferror(f) != 0) : 0; }
D(fflush) { (void)c; FILE *f = hf(ARG(0)); if (f) fflush(f); else if (!ARG(0)) fflush(NULL); return 0; }
D(fgets) {                                                     /* (buffer, n, stream) */
    (void)c;
    FILE *f = hf(ARG(2));
    if (!f || (int32_t)ARG(1) <= 0) return 0;
    return fgets((char *)HOST(ARG(0)), (int)ARG(1), f) ? ARG(0) : 0;
}
D(fputs) { (void)c; FILE *f = hf(ARG(1)); return f && fputs(STR(ARG(0)), f) >= 0 ? 0 : 0xFFFFFFFFu; }
D(fputc) { (void)c; FILE *f = hf(ARG(1)); return f ? (uint32_t)fputc((int)(ARG(0) & 0xFF), f) : 0xFFFFFFFFu; }
D(_fileno) { (void)c; uint32_t i = (ARG(0) - crt_iob()) / 32; return hf(ARG(0)) ? i : 0xFFFFFFFFu; }
D(_setmode) { (void)c; (void)a; return 0x8000; }              /* previous mode: _O_BINARY */
D(_getch) { (void)c; (void)a; return 0; }
static uint32_t stream_printf(uint32_t g, const char *fmt, uint32_t va) {
    FILE *f = hf(g);
    char *tmp = malloc(65536);
    size_t n = crt_format(tmp, 65536, fmt, va, 0);
    if (f) fwrite(tmp, 1, n < 65535 ? n : 65535, f);
    free(tmp);
    return f ? (uint32_t)n : 0xFFFFFFFFu;
}
D(fprintf) { (void)c; return stream_printf(ARG(0), STR(ARG(1)), a + 8); }
D(vfprintf) { (void)c; return stream_printf(ARG(0), STR(ARG(1)), ARG(2)); }

/* ---- file status ---- */
static void stat_path(uint32_t p, int wide, char *out, size_t n) {
    char win[1024];
    if (wide) w2utf8(p, win, sizeof win); else snprintf(win, sizeof win, "%s", STR(p));
    size_t l = strlen(win);
    while (l > 3 && (win[l - 1] == '\\' || win[l - 1] == '/')) win[--l] = 0;   /* "dir\" stats the directory */
    win_path_host(win, 0, out, n);
}
static uint32_t win_mode(const struct stat *st) {
    return (S_ISDIR(st->st_mode) ? 0x4000u : 0x8000u) | 0x0100u | 0x0080u | ((st->st_mode & 0111) ? 0x40u : 0u);
}
static uint32_t do_stat(CPU *c, uint32_t path, int wide, uint32_t out, int size64) {
    char hp[1200];
    struct stat st;
    stat_path(path, wide, hp, sizeof hp);
    if (stat(hp, &st)) { crt_set_errno(c, errno); return 0xFFFFFFFFu; }
    memset(HOST(out), 0, size64 ? 56 : 48);
    W32(out + 0, 2);                                           /* st_dev: drive C */
    W16(out + 6, win_mode(&st));
    W16(out + 8, 1);
    W32(out + 16, 2);                                          /* st_rdev */
    if (size64) {                                              /* struct _stat64 */
        W64(out + 24, (uint64_t)st.st_size);
        W64(out + 32, (uint64_t)st.st_atime); W64(out + 40, (uint64_t)st.st_mtime); W64(out + 48, (uint64_t)st.st_ctime);
    } else {                                                   /* struct _stat64i32 */
        W32(out + 20, (uint32_t)st.st_size);
        W64(out + 24, (uint64_t)st.st_atime); W64(out + 32, (uint64_t)st.st_mtime); W64(out + 40, (uint64_t)st.st_ctime);
    }
    return 0;
}
D(_stat64i32) { return do_stat(c, ARG(0), 0, ARG(1), 0); }
D(_stat64) { return do_stat(c, ARG(0), 0, ARG(1), 1); }
D(_wstat64i32) { return do_stat(c, ARG(0), 1, ARG(1), 0); }
D(_waccess_s) {                                                /* (path, mode) -> errno value */
    char hp[1200];
    stat_path(ARG(0), 1, hp, sizeof hp);
    if (access(hp, F_OK)) { crt_set_errno(c, errno); return 2; }
    return 0;
}
D(_wremove) {
    char win[1024], hp[1200];
    w2utf8(ARG(0), win, sizeof win);
    win_path_host(win, 1, hp, sizeof hp);
    if (remove(hp)) { crt_set_errno(c, errno); return 0xFFFFFFFFu; }
    return 0;
}

/* ---- _findfirst64i32 / _findnext64i32: struct _finddata64i32_t (attrib, 3 x __time64_t, size, name[260]) ---- */
typedef struct { DIR *d; char dir[1024]; char pat[256]; } Find;
static int find_next(Find *f, uint32_t out) {
    struct dirent *e;
    while ((e = readdir(f->d))) {
        if (fnmatch(f->pat, e->d_name, FNM_CASEFOLD)) continue;
        char full[1400];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", f->dir, e->d_name);
        if (stat(full, &st)) continue;
        memset(HOST(out), 0, 296);
        W32(out, S_ISDIR(st.st_mode) ? 0x10u : 0x20u);
        W64(out + 8, (uint64_t)st.st_ctime); W64(out + 16, (uint64_t)st.st_atime); W64(out + 24, (uint64_t)st.st_mtime);
        W32(out + 32, (uint32_t)st.st_size);
        snprintf((char *)HOST(out + 36), 260, "%s", e->d_name);
        return 1;
    }
    return 0;
}
D(_findfirst64i32) {
    char win[1024], hp[1200];
    snprintf(win, sizeof win, "%s", STR(ARG(0)));
    win_path_host(win, 0, hp, sizeof hp);
    Find *f = calloc(1, sizeof *f);
    char *sl = strrchr(hp, '/');
    snprintf(f->pat, sizeof f->pat, "%s", sl ? sl + 1 : hp);
    if (!strcmp(f->pat, "*.*")) strcpy(f->pat, "*");
    if (sl) *sl = 0;
    snprintf(f->dir, sizeof f->dir, "%s", sl ? hp : ".");
    f->d = opendir(f->dir);
    if (!f->d || !find_next(f, ARG(1))) {
        if (f->d) closedir(f->d);
        free(f);
        crt_set_errno(c, ENOENT);
        return 0xFFFFFFFFu;
    }
    return handle_new(H_FIND, f);
}
D(_findnext64i32) {
    Find *f = handle_get(ARG(0), H_FIND);
    if (!f || !find_next(f, ARG(1))) { crt_set_errno(c, ENOENT); return 0xFFFFFFFFu; }
    return 0;
}
D(_findclose) {
    (void)c;
    Find *f = handle_get(ARG(0), H_FIND);
    if (f) { closedir(f->d); free(f); handle_close(ARG(0)); }
    return 0;
}

/* ---- environment ---- */
D(getenv) {
    (void)c;
    static uint32_t buf;
    const char *name = STR(ARG(0)), *v = NULL;
    if (!strcasecmp(name, "TEMP") || !strcasecmp(name, "TMP")) v = "C:\\Temp";
    else if (!strcasecmp(name, "USERPROFILE")) v = "C:\\Users\\player";
    else if (!strcasecmp(name, "APPDATA")) v = "C:\\Users\\player\\AppData\\Roaming";
    else if (!strcasecmp(name, "SystemRoot") || !strcasecmp(name, "windir")) v = "C:\\Windows";
    usf_log("getenv(%s) -> %s\n", name, v ? v : "(none)");
    if (!v) return 0;
    if (!buf) buf = gmalloc(256);
    snprintf((char *)HOST(buf), 256, "%s", v);
    return buf;
}

/* ---- time ---- */
D(_time64) {
    int64_t t = (int64_t)time(NULL);
    if (ARG(0)) W64(ARG(0), (uint64_t)t);
    return ret64(c, (uint64_t)t);
}
static void tm_out(uint32_t g, const struct tm *t) {           /* struct tm: nine ints, the same on both sides */
    W32(g, t->tm_sec); W32(g + 4, t->tm_min); W32(g + 8, t->tm_hour); W32(g + 12, t->tm_mday); W32(g + 16, t->tm_mon);
    W32(g + 20, t->tm_year); W32(g + 24, t->tm_wday); W32(g + 28, t->tm_yday); W32(g + 32, t->tm_isdst);
}
static uint32_t tm_buf(CPU *c) { return c->fs_base + 0x1000u; }       /* per thread, as the CRT's is */
D(_localtime64) {
    struct tm t;
    time_t v = (time_t)(int64_t)R64(ARG(0));
    if (!localtime_r(&v, &t)) return 0;
    tm_out(tm_buf(c), &t);
    return tm_buf(c);
}
D(_gmtime64) {
    struct tm t;
    time_t v = (time_t)(int64_t)R64(ARG(0));
    if (!gmtime_r(&v, &t)) return 0;
    tm_out(tm_buf(c), &t);
    return tm_buf(c);
}
D(strftime) {                                                  /* (dst, max, format, tm*) */
    (void)c;
    struct tm t = {0};
    uint32_t g = ARG(3);
    t.tm_sec = (int)R32(g); t.tm_min = (int)R32(g + 4); t.tm_hour = (int)R32(g + 8); t.tm_mday = (int)R32(g + 12);
    t.tm_mon = (int)R32(g + 16); t.tm_year = (int)R32(g + 20); t.tm_wday = (int)R32(g + 24); t.tm_yday = (int)R32(g + 28);
    t.tm_isdst = (int)R32(g + 32);
    return (uint32_t)strftime((char *)HOST(ARG(0)), ARG(1), STR(ARG(2)), &t);
}
