/* kernel32 files and directories, on the host file system.
 *
 * What it does: translates Windows paths to host paths (C:\Rayman\... and relative paths are the game folder;
 * any other drive path, e.g. the user's AppData / Documents where saves go, lands under
 * ~/.local/share/rayman-origins-pc/drive_c/), resolving every component case-insensitively because the game's names and the
 * files on disk do not always agree in case. Files are host file descriptors behind handles; reads and writes are
 * synchronous (an OVERLAPPED read is done at its offset and its event is set). Also find-first/next, attributes,
 * directories, private profile (.ini) strings.
 * The game's folder is never written: a write inside it (the game appends to test\system.log, for one) goes to the
 * same relative path under ~/.local/share/rayman-origins-pc/game/ (the original is copied there first), and later reads of
 * that path see the copy.
 * What it does not do: share modes, locking, real asynchronous I/O, or list the copies in find-first/next.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "game.h"
#include "hle.h"

typedef struct { int fd; char path[1024]; } File;

static void user_root(char *out, size_t n) {
    snprintf(out, n, "%s/drive_c", g_data_dir);
}

static void mkdirs(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

/* Find `name` in directory `dir` ignoring case; writes the real name. */
static int match_ci(const char *dir, const char *name, char *real, size_t n) {
    DIR *d = opendir(dir[0] ? dir : ".");
    if (!d) return 0;
    struct dirent *e;
    int ok = 0;
    while ((e = readdir(d)))
        if (!strcasecmp(e->d_name, name)) { snprintf(real, n, "%s", e->d_name); ok = 1; break; }
    closedir(d);
    return ok;
}

/* Windows path (UTF-8) -> host path. Components that do not exist keep their spelling (for files being created). */
void host_path(const char *win, char *out, size_t n) {
    char rest[1024];
    const char *p = win;
    if (!strncmp(p, "\\\\?\\", 4)) p += 4;
    if (((p[0] | 32) >= 'a' && (p[0] | 32) <= 'z') && p[1] == ':') {
        if (!strncasecmp(p, GAME_WIN_DIR, GAME_WIN_DIR_LEN) && (!p[GAME_WIN_DIR_LEN] || p[GAME_WIN_DIR_LEN] == '\\' || p[GAME_WIN_DIR_LEN] == '/')) {
            snprintf(out, n, "%s", g_game_dir);
            p += GAME_WIN_DIR_LEN;
        } else {
            user_root(out, n);
            p += 2;
        }
    } else {
        snprintf(out, n, "%s", g_game_dir);
    }
    snprintf(rest, sizeof rest, "%s", p);
    for (char *s = rest; *s; s++) if (*s == '\\') *s = '/';
    char *save = NULL;
    for (char *comp = strtok_r(rest, "/", &save); comp; comp = strtok_r(NULL, "/", &save)) {
        if (!strcmp(comp, ".")) continue;
        if (!strcmp(comp, "..")) { char *sl = strrchr(out, '/'); if (sl && sl != out) *sl = 0; continue; }
        char real[256];
        size_t len = strlen(out);
        if (match_ci(out, comp, real, sizeof real)) snprintf(out + len, n - len, "/%s", real);
        else snprintf(out + len, n - len, "/%s", comp);
    }
}

/* The copy of a game-folder path under ~/.local/share/rayman-origins-pc/game/; 0 if `path` is not in the game folder. */
static int overlay_of(const char *path, char *out, size_t n) {
    size_t gl = strlen(g_game_dir);
    if (strncmp(path, g_game_dir, gl) || (path[gl] && path[gl] != '/')) return 0;
    snprintf(out, n, "%s/game%s", g_data_dir, path + gl);
    return 1;
}

static void path_arg(uint32_t p, int wide, char *out, size_t n) {   /* for reading: the copy if there is one */
    char win[1024], ov[1024];
    struct stat st;
    if (wide) w2utf8(p, win, sizeof win); else snprintf(win, sizeof win, "%s", STR(p));
    host_path(win, out, n);
    if (overlay_of(out, ov, sizeof ov) && !stat(ov, &st)) snprintf(out, n, "%s", ov);
}

/* For writing: game-folder paths become their copy (made from the original on first write); others are unchanged. */
static void path_arg_w(uint32_t p, int wide, char *out, size_t n) {
    char win[1024], ov[1024];
    struct stat st;
    if (wide) w2utf8(p, win, sizeof win); else snprintf(win, sizeof win, "%s", STR(p));
    host_path(win, out, n);
    if (!overlay_of(out, ov, sizeof ov)) return;
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", ov);
    char *sl = strrchr(dir, '/');
    if (sl) { *sl = 0; mkdirs(dir); }
    if (stat(ov, &st) && !stat(out, &st) && S_ISREG(st.st_mode)) {
        FILE *src = fopen(out, "rb"), *dst = fopen(ov, "wb");
        char buf[65536];
        size_t k;
        while (src && dst && (k = fread(buf, 1, sizeof buf, src)) > 0) fwrite(buf, 1, k, dst);
        if (src) fclose(src);
        if (dst) fclose(dst);
    }
    snprintf(out, n, "%s", ov);
}

/* For the C runtime (hle_crt_io.c): a Windows path (UTF-8) to the host path to open, for reading or writing. */
void win_path_host(const char *win, int write, char *out, size_t n) {
    char ov[1024];
    struct stat st;
    host_path(win, out, n);
    if (!overlay_of(out, ov, sizeof ov)) return;
    if (!write) { if (!stat(ov, &st)) snprintf(out, n, "%s", ov); return; }
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", ov);
    char *sl = strrchr(dir, '/');
    if (sl) { *sl = 0; mkdirs(dir); }
    if (stat(ov, &st) && !stat(out, &st) && S_ISREG(st.st_mode)) {
        FILE *src = fopen(out, "rb"), *dst = fopen(ov, "wb");
        char buf[65536];
        size_t k;
        while (src && dst && (k = fread(buf, 1, sizeof buf, src)) > 0) fwrite(buf, 1, k, dst);
        if (src) fclose(src);
        if (dst) fclose(dst);
    }
    snprintf(out, n, "%s", ov);
}

static void set_errno_error(CPU *c) {
    set_last_error(c, errno == ENOENT ? 2u : errno == ENOTDIR ? 3u : errno == EEXIST ? 80u : errno == EACCES ? 5u : 31u);
}

static uint32_t create_file(CPU *c, uint32_t a, int wide) {
    char path[1024];
    uint32_t access = ARG(1), disp = ARG(4);
    if ((access & 0x40000000u) || disp == 1 || disp == 2 || disp == 4 || disp == 5) path_arg_w(ARG(0), wide, path, sizeof path);
    else path_arg(ARG(0), wide, path, sizeof path);
    int fl = (access & 0x40000000u) ? ((access & 0x80000000u) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disp) {
    case 1: fl |= O_CREAT | O_EXCL; break;
    case 2: fl |= O_CREAT | O_TRUNC; break;
    case 4: fl |= O_CREAT; break;
    case 5: fl |= O_TRUNC; break;
    }
    if (fl & O_CREAT) { char dir[1024]; snprintf(dir, sizeof dir, "%s", path); char *s = strrchr(dir, '/'); if (s) { *s = 0; mkdirs(dir); } }
    int fd = open(path, fl, 0644);
    usf_log("CreateFile(%s) -> fd %d\n", path, fd);
    if (fd < 0) { set_errno_error(c); return 0xFFFFFFFFu; }
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) { close(fd); set_last_error(c, 5); return 0xFFFFFFFFu; }
    File *f = calloc(1, sizeof *f);
    f->fd = fd;
    snprintf(f->path, sizeof f->path, "%s", path);
    set_last_error(c, (disp == 4 || disp == 2) ? 0u : 0u);
    return handle_new(H_FILE, f);
}
uint32_t k32_CreateFileA(CPU *c, uint32_t a) { return create_file(c, a, 0); }
uint32_t k32_CreateFileW(CPU *c, uint32_t a) { return create_file(c, a, 1); }

static File *fget(uint32_t h) { return handle_get(h, H_FILE); }

uint32_t k32_ReadFile(CPU *c, uint32_t a) {
    File *f = fget(ARG(0));
    if (!f) { set_last_error(c, 6); return 0; }
    uint32_t ov = ARG(4);
    ssize_t r = ov ? pread(f->fd, HOST(ARG(1)), ARG(2), (off_t)(R32(ov + 8) | (uint64_t)R32(ov + 12) << 32))
                   : read(f->fd, HOST(ARG(1)), ARG(2));
    if (r < 0) { set_errno_error(c); return 0; }
    if (ARG(3)) W32(ARG(3), (uint32_t)r);
    if (ov) {
        W32(ov, 0);                                            /* Internal: STATUS_SUCCESS */
        W32(ov + 4, (uint32_t)r);                              /* InternalHigh: bytes transferred */
        if (R32(ov + 16)) { uint32_t t = gmalloc(4); W32(t, R32(ov + 16)); extern uint32_t k32_SetEvent(CPU *, uint32_t); k32_SetEvent(c, t); gfree(t); }
    }
    return 1;
}

uint32_t k32_WriteFile(CPU *c, uint32_t a) {
    uint32_t h = ARG(0);
    if (h == 0x14u || h == 0x18u) {
        fwrite(HOST(ARG(1)), 1, ARG(2), stderr);
        if (ARG(3)) W32(ARG(3), ARG(2));
        return 1;
    }
    File *f = fget(h);
    if (!f) { set_last_error(c, 6); return 0; }
    uint32_t ov = ARG(4);
    ssize_t r = ov ? pwrite(f->fd, HOST(ARG(1)), ARG(2), (off_t)(R32(ov + 8) | (uint64_t)R32(ov + 12) << 32))
                   : write(f->fd, HOST(ARG(1)), ARG(2));
    if (r < 0) { set_errno_error(c); return 0; }
    if (ARG(3)) W32(ARG(3), (uint32_t)r);
    return 1;
}

uint32_t k32_WriteConsoleW(CPU *c, uint32_t a) {
    (void)c;
    char buf[4096];
    w2utf8(ARG(1), buf, sizeof buf);
    fputs(buf, stderr);
    if (ARG(3)) W32(ARG(3), ARG(2));
    return 1;
}

uint32_t k32_SetFilePointer(CPU *c, uint32_t a) {
    File *f = fget(ARG(0));
    if (!f) { set_last_error(c, 6); return 0xFFFFFFFFu; }
    int64_t off = (int32_t)ARG(1);
    if (ARG(2)) off = (int64_t)((uint64_t)R32(ARG(2)) << 32 | ARG(1));
    off_t r = lseek(f->fd, (off_t)off, ARG(3) == 0 ? SEEK_SET : ARG(3) == 1 ? SEEK_CUR : SEEK_END);
    if (r < 0) { set_errno_error(c); return 0xFFFFFFFFu; }
    if (ARG(2)) W32(ARG(2), (uint32_t)((uint64_t)r >> 32));
    set_last_error(c, 0);
    return (uint32_t)r;
}

uint32_t k32_GetFileSize(CPU *c, uint32_t a) {
    File *f = fget(ARG(0));
    struct stat st;
    if (!f || fstat(f->fd, &st)) { set_last_error(c, 6); return 0xFFFFFFFFu; }
    if (ARG(1)) W32(ARG(1), (uint32_t)((uint64_t)st.st_size >> 32));
    return (uint32_t)st.st_size;
}
uint32_t k32_GetFileSizeEx(CPU *c, uint32_t a) {
    File *f = fget(ARG(0));
    struct stat st;
    if (!f || fstat(f->fd, &st)) { set_last_error(c, 6); return 0; }
    W64(ARG(1), (uint64_t)st.st_size);
    return 1;
}
uint32_t k32_GetFileType(CPU *c, uint32_t a) { (void)c; return fget(ARG(0)) ? 1u : 2u; }
uint32_t k32_FlushFileBuffers(CPU *c, uint32_t a) { (void)c; File *f = fget(ARG(0)); if (f) fsync(f->fd); return 1; }
uint32_t k32_SetEndOfFile(CPU *c, uint32_t a) {
    (void)c;
    File *f = fget(ARG(0));
    if (!f) return 0;
    return ftruncate(f->fd, lseek(f->fd, 0, SEEK_CUR)) == 0;
}
uint32_t k32_PeekNamedPipe(CPU *c, uint32_t a) { set_last_error(c, 1); (void)a; return 0; }

static uint64_t ft_of(struct timespec t) { return (uint64_t)t.tv_sec * 10000000ull + (uint64_t)t.tv_nsec / 100 + 116444736000000000ull; }

uint32_t k32_GetFileTime(CPU *c, uint32_t a) {
    (void)c;
    File *f = fget(ARG(0));
    struct stat st;
    if (!f || fstat(f->fd, &st)) return 0;
    if (ARG(1)) W64(ARG(1), ft_of(st.st_mtim));
    if (ARG(2)) W64(ARG(2), ft_of(st.st_atim));
    if (ARG(3)) W64(ARG(3), ft_of(st.st_mtim));
    return 1;
}

uint32_t k32_GetFileInformationByHandle(CPU *c, uint32_t a) {
    (void)c;
    File *f = fget(ARG(0));
    struct stat st;
    if (!f || fstat(f->fd, &st)) return 0;
    uint32_t p = ARG(1);
    memset(HOST(p), 0, 52);
    W32(p, 0x80);                                              /* FILE_ATTRIBUTE_NORMAL */
    W64(p + 4, ft_of(st.st_mtim)); W64(p + 12, ft_of(st.st_atim)); W64(p + 20, ft_of(st.st_mtim));
    W32(p + 32, (uint32_t)((uint64_t)st.st_size >> 32)); W32(p + 36, (uint32_t)st.st_size);
    W32(p + 40, 1);
    return 1;
}

static uint32_t attrs(const char *path) {
    struct stat st;
    if (stat(path, &st)) return 0xFFFFFFFFu;
    return S_ISDIR(st.st_mode) ? 0x10u : 0x80u;
}
uint32_t k32_GetFileAttributesW(CPU *c, uint32_t a) {
    char path[1024];
    path_arg(ARG(0), 1, path, sizeof path);
    uint32_t r = attrs(path);
    if (r == 0xFFFFFFFFu) set_last_error(c, 2);
    return r;
}
uint32_t k32_SetFileAttributesW(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
uint32_t k32_SetFileTime(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
uint32_t k32_GetFileAttributesA(CPU *c, uint32_t a) {
    char path[1024];
    path_arg(ARG(0), 0, path, sizeof path);
    uint32_t r = attrs(path);
    if (r == 0xFFFFFFFFu) set_last_error(c, 2);
    return r;
}

uint32_t k32_CreateDirectoryA(CPU *c, uint32_t a) {
    char path[1024];
    path_arg_w(ARG(0), 0, path, sizeof path);
    if (mkdir(path, 0755) && errno != EEXIST) { set_errno_error(c); return 0; }
    if (errno == EEXIST) { set_last_error(c, 183); return 0; }
    return 1;
}
uint32_t k32_CreateDirectoryW(CPU *c, uint32_t a) {
    char path[1024];
    path_arg_w(ARG(0), 1, path, sizeof path);
    errno = 0;
    if (mkdir(path, 0755)) { set_last_error(c, errno == EEXIST ? 183u : 3u); return 0; }
    return 1;
}
uint32_t x_SHCreateDirectoryExA(CPU *c, uint32_t a) {
    (void)c;
    char path[1024];
    path_arg_w(ARG(1), 0, path, sizeof path);
    mkdirs(path);
    return 0;
}
uint32_t k32_DeleteFileA(CPU *c, uint32_t a) {
    char path[1024];
    path_arg_w(ARG(0), 0, path, sizeof path);
    if (unlink(path)) { set_errno_error(c); return 0; }
    return 1;
}
uint32_t k32_MoveFileA(CPU *c, uint32_t a) {
    char p1[1024], p2[1024];
    path_arg_w(ARG(0), 0, p1, sizeof p1);
    path_arg_w(ARG(1), 0, p2, sizeof p2);
    if (rename(p1, p2)) { set_errno_error(c); return 0; }
    return 1;
}

/* ---- find first / next ---- */
typedef struct { DIR *d; char dir[1024]; char pat[256]; int wide; } Find;

static int find_next(Find *f, uint32_t out) {
    struct dirent *e;
    while ((e = readdir(f->d))) {
        if (fnmatch(f->pat, e->d_name, FNM_CASEFOLD)) continue;
        char full[1400];
        snprintf(full, sizeof full, "%s/%s", f->dir, e->d_name);
        struct stat st;
        if (stat(full, &st)) continue;
        uint32_t size = f->wide ? 592u : 320u;
        memset(HOST(out), 0, size);
        W32(out, S_ISDIR(st.st_mode) ? 0x10u : 0x80u);
        W64(out + 4, ft_of(st.st_mtim)); W64(out + 12, ft_of(st.st_atim)); W64(out + 20, ft_of(st.st_mtim));
        W32(out + 28, (uint32_t)((uint64_t)st.st_size >> 32)); W32(out + 32, (uint32_t)st.st_size);
        if (f->wide) utf8_to_w(e->d_name, out + 44, 260);
        else snprintf((char *)HOST(out + 44), 260, "%s", e->d_name);
        return 1;
    }
    return 0;
}

static uint32_t find_first(CPU *c, uint32_t pattern, uint32_t out, int wide) {
    char path[1024];
    path_arg(pattern, wide, path, sizeof path);
    Find *f = calloc(1, sizeof *f);
    char *sl = strrchr(path, '/');
    snprintf(f->pat, sizeof f->pat, "%s", sl ? sl + 1 : path);
    if (sl) *sl = 0;
    snprintf(f->dir, sizeof f->dir, "%s", sl ? path : ".");
    f->wide = wide;
    f->d = opendir(f->dir);
    if (!f->d || !find_next(f, out)) {
        if (f->d) closedir(f->d);
        free(f);
        set_last_error(c, 2);
        return 0xFFFFFFFFu;
    }
    return handle_new(H_FIND, f);
}
uint32_t k32_FindFirstFileA(CPU *c, uint32_t a) { return find_first(c, ARG(0), ARG(1), 0); }
uint32_t k32_FindFirstFileExW(CPU *c, uint32_t a) { return find_first(c, ARG(0), ARG(2), 1); }
uint32_t k32_FindNextFileA(CPU *c, uint32_t a) {
    Find *f = handle_get(ARG(0), H_FIND);
    if (!f || !find_next(f, ARG(1))) { set_last_error(c, 18); return 0; }   /* ERROR_NO_MORE_FILES */
    return 1;
}
uint32_t k32_FindClose(CPU *c, uint32_t a) {
    (void)c;
    Find *f = handle_get(ARG(0), H_FIND);
    if (f) { closedir(f->d); free(f); handle_close(ARG(0)); }
    return 1;
}

/* ---- paths and directories ---- */
uint32_t k32_GetCurrentDirectoryW(CPU *c, uint32_t a) {
    (void)c;
    const char *d = GAME_WIN_DIR;
    uint32_t n = (uint32_t)strlen(d);
    if (ARG(0) > n) utf8_to_w(d, ARG(1), ARG(0));
    return ARG(0) > n ? n : n + 1;
}
uint32_t k32_SetCurrentDirectoryW(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }

static uint32_t full_path(const char *in, char *out, size_t n) {
    if (in[1] == ':') snprintf(out, n, "%s", in);
    else snprintf(out, n, "%s\\%s", GAME_WIN_DIR, in);
    return (uint32_t)strlen(out);
}
uint32_t k32_GetFullPathNameA(CPU *c, uint32_t a) {
    (void)c;
    char out[1024];
    uint32_t n = full_path(STR(ARG(0)), out, sizeof out);
    if (ARG(1) <= n) return n + 1;
    memcpy(HOST(ARG(2)), out, n + 1);
    if (ARG(3)) { char *s = strrchr(out, '\\'); W32(ARG(3), s ? ARG(2) + (uint32_t)(s + 1 - out) : 0); }
    return n;
}
uint32_t k32_GetFullPathNameW(CPU *c, uint32_t a) {
    (void)c;
    char in[1024], out[1024];
    w2utf8(ARG(0), in, sizeof in);
    uint32_t n = full_path(in, out, sizeof out);
    if (ARG(1) <= n) return n + 1;
    utf8_to_w(out, ARG(2), ARG(1));
    if (ARG(3)) { char *s = strrchr(out, '\\'); W32(ARG(3), s ? ARG(2) + 2u * (uint32_t)(s + 1 - out) : 0); }
    return n;
}
uint32_t k32_GetDriveTypeW(CPU *c, uint32_t a) { (void)c; (void)a; return 3; }   /* DRIVE_FIXED */
uint32_t k32_GetDiskFreeSpaceExW(CPU *c, uint32_t a) {
    (void)c;
    if (ARG(1)) W64(ARG(1), 100ull << 30);
    if (ARG(2)) W64(ARG(2), 200ull << 30);
    if (ARG(3)) W64(ARG(3), 100ull << 30);
    return 1;
}

/* SHGetFolderPathA: every special folder is a directory under the user root (saves, settings). */
uint32_t x_SHGetFolderPathA(CPU *c, uint32_t a) {
    (void)c;
    const char *p;
    switch (ARG(1) & 0xFF) {
    case 0x05: p = "C:\\Users\\player\\Documents"; break;                 /* CSIDL_PERSONAL */
    case 0x1A: p = "C:\\Users\\player\\AppData\\Roaming"; break;          /* CSIDL_APPDATA */
    case 0x1C: p = "C:\\Users\\player\\AppData\\Local"; break;            /* CSIDL_LOCAL_APPDATA */
    case 0x23: p = "C:\\ProgramData"; break;                              /* CSIDL_COMMON_APPDATA */
    default: p = "C:\\Users\\player"; break;
    }
    snprintf((char *)HOST(ARG(4)), 260, "%s", p);
    char host[1024];
    host_path(p, host, sizeof host);
    mkdirs(host);
    return 0;
}

/* ---- private profile strings (.ini) ---- */
uint32_t k32_GetPrivateProfileStringA(CPU *c, uint32_t a) {
    (void)c;
    char path[1024], line[1024], sect[256] = "";
    path_arg(ARG(5), 0, path, sizeof path);
    const char *want_s = STR(ARG(0)), *want_k = ARG(1) ? STR(ARG(1)) : NULL, *def = ARG(2) ? STR(ARG(2)) : "";
    char val[1024];
    snprintf(val, sizeof val, "%s", def);
    FILE *f = fopen(path, "r");
    if (f && want_k) {
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (line[0] == '[') { char *e = strchr(line, ']'); if (e) *e = 0; snprintf(sect, sizeof sect, "%s", line + 1); continue; }
            char *eq = strchr(line, '=');
            if (!eq || strcasecmp(sect, want_s)) continue;
            *eq = 0;
            if (!strcasecmp(line, want_k)) { snprintf(val, sizeof val, "%s", eq + 1); break; }
        }
    }
    if (f) fclose(f);
    int usf_auto_resolution(uint32_t *w, uint32_t *h);
    uint32_t rw, rh;
    if (want_k && !strcasecmp(want_s, "DISPLAY") && !strcasecmp(want_k, "Resolution") && usf_auto_resolution(&rw, &rh))
        snprintf(val, sizeof val, "%ux%u", rw, rh);                          /* the screen's shape (hle_user32.c) */
    usf_log("GetPrivateProfileString [%s] %s (%s) -> \"%s\"\n", want_s, want_k ? want_k : "(all)", path, val);
    uint32_t n = (uint32_t)strlen(val);
    if (ARG(4) == 0) return 0;
    if (n >= ARG(4)) n = ARG(4) - 1;
    memcpy(HOST(ARG(3)), val, n);
    W8(ARG(3) + n, 0);
    return n;
}

/* Rewrites the .ini with [section] key=value set (NULL key: section removed; NULL value: key removed). */
uint32_t k32_WritePrivateProfileStringA(CPU *c, uint32_t a) {
    (void)c;
    char path[1024], line[1024], sect[256] = "";
    path_arg_w(ARG(3), 0, path, sizeof path);
    const char *ws = STR(ARG(0)), *wk = ARG(1) ? STR(ARG(1)) : NULL, *wv = ARG(2) ? STR(ARG(2)) : NULL;
    static char out[1 << 16];
    size_t o = 0;
    int in_sect = 0, done = 0, seen_sect = 0;
    FILE *f = fopen(path, "r");
    while (f && fgets(line, sizeof line, f)) {
        char t[1024];
        snprintf(t, sizeof t, "%s", line);
        t[strcspn(t, "\r\n")] = 0;
        if (t[0] == '[') {
            if (in_sect && !done && wk && wv) { o += (size_t)snprintf(out + o, sizeof out - o, "%s=%s\r\n", wk, wv); done = 1; }
            snprintf(sect, sizeof sect, "%s", t + 1);
            char *e = strchr(sect, ']'); if (e) *e = 0;
            if (!e) { size_t l = strlen(t); if (l + 1 < sizeof t) { t[l] = ']'; t[l + 1] = 0; } }   /* files written before the fix */
            in_sect = !strcasecmp(sect, ws);
            seen_sect |= in_sect;
            if (in_sect && !wk) continue;
        } else if (in_sect) {
            if (!wk) continue;
            char *eq = strchr(t, '=');
            if (eq && (size_t)(eq - t) == strlen(wk) && !strncasecmp(t, wk, strlen(wk))) {
                if (wv && !done) o += (size_t)snprintf(out + o, sizeof out - o, "%s=%s\r\n", wk, wv);
                done = 1;
                continue;
            }
        }
        o += (size_t)snprintf(out + o, sizeof out - o, "%s\r\n", t);
        if (o > sizeof out - 2048) break;
    }
    if (f) fclose(f);
    if (wk && wv && !done) {
        if (!seen_sect) o += (size_t)snprintf(out + o, sizeof out - o, "[%s]\r\n", ws);
        else if (in_sect) { /* section was last: append below it */ }
        o += (size_t)snprintf(out + o, sizeof out - o, "%s=%s\r\n", wk, wv);
    }
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    char *sl = strrchr(dir, '/');
    if (sl) { *sl = 0; mkdirs(dir); }
    f = fopen(path, "w");
    if (!f) return 0;
    fwrite(out, 1, o, f);
    fclose(f);
    return 1;
}

/* Called by CloseHandle: closes the host file behind a file or find handle. 1 if it was one. */
int file_close_handle(uint32_t h) {
    File *f = fget(h);
    if (!f) return 0;
    close(f->fd);
    free(f);
    handle_close(h);
    return 1;
}
