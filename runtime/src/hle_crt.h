/* The Visual C++ 2008 C runtime (msvcr90.dll) answered natively: what the hle_crt_*.c files share.
 *
 * What it is for: the game links the C runtime dynamically, so every strcmp, malloc, fopen, sprintf and _CIpow it
 * calls is an import. Each one is a cdecl HLE function (the caller pops the arguments) reading its arguments from the
 * guest stack with ARG(i). Strings are guest pointers (STR), wide strings are UTF-16 in guest memory.
 * What it does not do: run any code of msvcr90.dll (none is loaded) or C++ exception unwinding (a throw is reported).
 */
#ifndef HLE_CRT_H
#define HLE_CRT_H

#include <stdarg.h>
#include <stddef.h>

#include "hle.h"

/* printf-style formatting of guest arguments (hle_crt_fmt.c). `va` is the guest address of the first variadic
 * argument. The result is UTF-8 in out (always NUL-terminated if cap > 0); returns the full length. Wide formats
 * (swprintf...) pass the format already converted to UTF-8 and wide = 1 (then %s is a UTF-16 argument). */
size_t crt_format(char *out, size_t cap, const char *fmt, uint32_t va, int wide);
/* scanf-style parsing into guest pointers (hle_crt_fmt.c); secure = the _s form (buffer sizes follow %s %c %[) */
int crt_scan(const char *in, const char *fmt, uint32_t va, int secure, int wide);

/* errno of the calling thread (a dword in its TEB) */
uint32_t crt_errno_addr(CPU *c);
void crt_set_errno(CPU *c, int host_errno);

/* guest FILE objects (hle_crt_io.c) */
uint32_t crt_iob(void);

/* UTF-16 guest string -> host buffer and back (UTF-8) */
void crt_w2u(uint32_t w, char *out, size_t n);
size_t crt_wlen(uint32_t w);

/* 64-bit results go in edx:eax */
static inline uint32_t ret64(CPU *c, uint64_t v) { c->edx = (uint32_t)(v >> 32); return (uint32_t)v; }

#endif
