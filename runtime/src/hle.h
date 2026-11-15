/* The runtime's side of the Windows calls the game makes (HLE: we answer the API, we do not run Windows code).
 *
 * What it is for: every import of the game executable (and every function handed out later by GetProcAddress or a COM vtable)
 * is a "thunk": a guest address in the thunk page that usf_icall recognises. Calling it runs a C function
 * `uint32_t fn(CPU *c, uint32_t a)` where `a` is the guest address of the first stack argument; its return value
 * goes to eax, and the dispatcher pops the return address plus the arguments for stdcall/thiscall (`nargs`
 * dwords), or only the return address for cdecl.
 * What it does not do: implement anything itself (hle_*.c do), or emulate code that runs inside Windows DLLs.
 */
#ifndef USF_HLE_H
#define USF_HLE_H

#include <stdint.h>
#include <stdio.h>

#include "usf/cpu.h"

typedef uint32_t (*HleFn)(CPU *c, uint32_t a);

enum { CONV_STDCALL = 0, CONV_CDECL = 1, CONV_CALLSITE = 2, CONV_DATA = 3 };
/* CALLSITE: pop what the caller pushed. DATA: an imported variable; fn(NULL, 0) returns its guest address */

typedef struct {
    const char *dll;      /* lower case, e.g. "kernel32.dll" */
    const char *name;     /* "CreateFileW", or "#12" for an ordinal */
    int nargs;            /* dwords popped by the callee (stdcall); ignored for cdecl */
    int conv;
    HleFn fn;             /* NULL: not implemented, logs and returns 0 */
} HleImport;

/* Thunks */
#define THUNK_BASE 0x7FF00000u
#define THUNK_SIZE 0x00010000u
uint32_t usf_thunk(const char *dll, const char *name);        /* thunk for a known import (or a logging stub) */
uint32_t usf_thunk_fn(const char *label, HleFn fn, int nargs, int conv);   /* thunk for our own function */
int usf_thunk_call(CPU *c, uint32_t target);                  /* 1 if target was a thunk and was called */

/* Arguments: ARG(i) is the i-th dword argument (0-based) */
#define ARG(i) R32(a + 4u * (i))
#define STR(p) ((const char *)HOST(p))

/* Guest strings */
size_t wlen(uint32_t p);
void w2utf8(uint32_t p, char *out, size_t n);                 /* UTF-16 guest string -> UTF-8 host */
void host_path(const char *win, char *out, size_t n);           /* Windows path -> host path (hle_k32_file.c) */
void win_path_host(const char *win, int write, char *out, size_t n);   /* the same, with the game-folder copy */
void utf8_to_w(const char *s, uint32_t dst, size_t nchars);   /* UTF-8 host -> UTF-16 guest (NUL-terminated) */

/* Guest memory allocation (heap.c) */
uint32_t gmalloc(uint32_t size);
uint32_t gcalloc(uint32_t size);
void gfree(uint32_t p);
uint32_t grealloc(uint32_t p, uint32_t size);
uint32_t gsize(uint32_t p);
uint32_t gpages(uint32_t addr, uint32_t size);                /* VirtualAlloc-style, 64 KiB granular */
void gpages_free(uint32_t addr);
void heap_init(void);

/* Process state */
extern uint32_t g_last_error_tls;                             /* not used: last error lives in the TEB */
uint32_t teb_of(CPU *c);
void set_last_error(CPU *c, uint32_t e);
extern int g_headless;                                        /* USF_HEADLESS: hidden window, silent (tests) */
extern char g_data_dir[1024];                                 /* our data: saves, settings, copies (USF_DATA) */
extern char g_game_dir[1024];                                 /* the Steam folder, host path */
extern int usf_log_on;                                             /* USF_LOG=1: log every import call */
void usf_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void hle_fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

/* Handles (hle_handles.c) */
enum { H_FILE = 1, H_EVENT, H_MUTEX, H_SEMAPHORE, H_THREAD, H_FIND, H_MODULE, H_HEAP, H_OTHER };
uint32_t handle_new(int kind, void *obj);
void *handle_get(uint32_t h, int kind);
void handle_close(uint32_t h);

/* Import tables, one per area (hle_kernel32*.c, hle_user32.c, ...) */
extern const HleImport hle_kernel32[], hle_user32[], hle_misc[];
const HleImport *hle_find(const char *dll, const char *name);

/* Objects the runtime hands to the game (objects.c): a guest object whose vtable slots are thunks.
 * impl[i] answers method i (NULL: logged, returns 0); every method pops what its call site pushed. */
uint32_t obj_new(const char *iface, int nslots, const HleFn *impl, uint32_t size);
/* the same with known argument counts (dwords, `this` included for COM): exact stdcall pops */
uint32_t obj_new_n(const char *iface, int nslots, const HleFn *impl, const uint8_t *nargs, uint32_t size);
uint32_t callsite_args(uint32_t ret);                         /* argument bytes pushed before the call returning to ret */
uint32_t guest_call(CPU *c, uint32_t addr, int conv, int n, ...);
uint32_t guest_thiscall(CPU *c, uint32_t obj, uint32_t addr, int n, ...);   /* ecx = obj, callee pops */

/* Threads (process.c) */
void thread_init_main(CPU *c, uint32_t stack_size);
CPU *cpu_alloc(void);

#endif
